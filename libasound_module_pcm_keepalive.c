/*
 * libasound_module_pcm_keepalive.c - ALSA keepalive proxy plugin
 *
 * Sits inline in the ALSA PCM chain. Holds the downstream (slave)
 * device open permanently. When the upstream client closes or stops,
 * feeds silence to the slave to maintain continuous audio frames.
 *
 * Prevents HDMI receivers, SPDIF decoders, and USB DACs from losing
 * signal lock during idle periods.
 *
 * Copyright (C) 2026 Just a Nerd
 * License: GPL-2.0-or-later
 *
 * Usage in asound.conf:
 *
 *   pcm.keepaliveProxy {
 *       type keepalive
 *       slave.pcm "downstream"
 *   }
 */

/* expose POSIX and GNU extensions: usleep, strdup, eventfd */
#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* suppress warn_unused_result on eventfd read/write.
 * these are advisory signals; failure is non-fatal. */
static inline void eventfd_signal(int fd)
{
    uint64_t val = 1;
    ssize_t r = write(fd, &val, sizeof(val));
    (void)r;
}

static inline void eventfd_consume(int fd)
{
    uint64_t val;
    ssize_t r = read(fd, &val, sizeof(val));
    (void)r;
}

/* --------------------------------------------------------------------------
 * Persistent state
 *
 * Survives close/reopen cycles. The slave PCM and the silence thread
 * live here. When the upstream client closes, both persist. The next
 * client instance reclaims the slave without any gap in the audio
 * stream - the silence thread keeps feeding frames throughout.
 * -------------------------------------------------------------------------- */

static struct keepalive_persist {
    pthread_mutex_t     lock;

    /* slave connection */
    snd_pcm_t          *slave;
    char                slave_name[256];
    unsigned int        rate;
    unsigned int        channels;
    snd_pcm_format_t    format;
    snd_pcm_uframes_t   period_size;
    snd_pcm_uframes_t   buffer_size;
    int                 configured;     /* 1 = slave has valid hw_params */

    /* silence thread - persistent across instance close/reopen */
    pthread_t           thread;
    pthread_mutex_t     thread_mtx;
    pthread_cond_t      thread_cond;
    int                 thread_running; /* 1 = thread exists */
    int                 active;         /* 1 = instance writing audio */
    int                 in_write;       /* 1 = silence write in progress */

    /* silence buffer */
    char               *silence_buf;
    snd_pcm_uframes_t   silence_frames;
    unsigned int        frame_bytes;
} persist = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .thread_mtx = PTHREAD_MUTEX_INITIALIZER,
    .thread_cond = PTHREAD_COND_INITIALIZER,
};

/* --------------------------------------------------------------------------
 * Per-instance state
 * -------------------------------------------------------------------------- */

struct keepalive_data {
    snd_pcm_ioplug_t   io;
    char               *slave_name;     /* from asound.conf config */

    /* poll mechanism */
    int                 event_fd;

    /* pointer tracking */
    snd_pcm_uframes_t   hw_ptr;
    unsigned int        frame_bytes;    /* cached for transfer offset calc */
};

/* --------------------------------------------------------------------------
 * Silence thread (persistent)
 *
 * Writes zero frames to the slave when no upstream instance is
 * actively playing. Feeds one period at a time, sleeps in between.
 * Pauses when active=1 and resumes when active=0.
 *
 * This thread is created on first use and runs until the process
 * exits. It survives instance close/reopen cycles, ensuring the
 * slave device always receives frames.
 * -------------------------------------------------------------------------- */

static void *silence_thread_fn(void *arg)
{
    snd_pcm_t *slave;
    snd_pcm_sframes_t ret;

    (void)arg;

    while (1) {
        pthread_mutex_lock(&persist.thread_mtx);

        /* wait while an instance is actively writing audio */
        while (persist.active)
            pthread_cond_wait(&persist.thread_cond, &persist.thread_mtx);

        /* mark that we are about to write */
        persist.in_write = 1;
        pthread_mutex_unlock(&persist.thread_mtx);

        /* write one period of silence to the slave */
        pthread_mutex_lock(&persist.lock);
        slave = persist.slave;
        pthread_mutex_unlock(&persist.lock);

        if (slave && persist.silence_buf) {
            ret = snd_pcm_writei(slave, persist.silence_buf,
                                 persist.silence_frames);
            if (ret == -EPIPE) {
                snd_pcm_prepare(slave);
                snd_pcm_writei(slave, persist.silence_buf,
                               persist.silence_frames);
            } else if (ret == -ESTRPIPE) {
                /* suspended - wait for resume */
                while (snd_pcm_resume(slave) == -EAGAIN)
                    usleep(10000);
                snd_pcm_prepare(slave);
            }
        }

        /* clear write flag, wake anyone waiting in start callback */
        pthread_mutex_lock(&persist.thread_mtx);
        persist.in_write = 0;
        pthread_cond_broadcast(&persist.thread_cond);
        pthread_mutex_unlock(&persist.thread_mtx);

        /* sleep for roughly half a period to avoid busy-loop.
         * the exact timing is not critical - the slave's own
         * buffer provides the flow control. */
        if (persist.silence_frames && persist.rate)
            usleep((unsigned long)persist.silence_frames * 500000
                   / persist.rate);
        else
            usleep(10000);
    }

    return NULL;
}

/* start the persistent silence thread if not already running */
static void ensure_silence_thread(void)
{
    pthread_mutex_lock(&persist.thread_mtx);
    if (!persist.thread_running) {
        persist.active = 0;
        persist.in_write = 0;
        if (pthread_create(&persist.thread, NULL,
                           silence_thread_fn, NULL) == 0) {
            persist.thread_running = 1;
        }
    }
    pthread_mutex_unlock(&persist.thread_mtx);
}

/* --------------------------------------------------------------------------
 * Slave management
 * -------------------------------------------------------------------------- */

static int open_slave(struct keepalive_data *kd)
{
    snd_pcm_t *pcm;
    snd_pcm_hw_params_t *hw;
    int err;

    /* pause silence thread while we reconfigure the slave */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 1;
    pthread_cond_broadcast(&persist.thread_cond);
    while (persist.in_write)
        pthread_cond_wait(&persist.thread_cond, &persist.thread_mtx);
    pthread_mutex_unlock(&persist.thread_mtx);

    err = snd_pcm_open(&pcm, kd->slave_name,
                        SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
        goto resume_silence;

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    err = snd_pcm_hw_params_set_access(pcm, hw,
            SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) goto fail;

    err = snd_pcm_hw_params_set_format(pcm, hw, kd->io.format);
    if (err < 0) goto fail;

    err = snd_pcm_hw_params_set_channels(pcm, hw, kd->io.channels);
    if (err < 0) goto fail;

    err = snd_pcm_hw_params_set_rate(pcm, hw, kd->io.rate, 0);
    if (err < 0) goto fail;

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) goto fail;

    /* read back actual parameters */
    snd_pcm_uframes_t period, buffer;
    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);

    err = snd_pcm_prepare(pcm);
    if (err < 0) goto fail;

    /* update persistent state */
    pthread_mutex_lock(&persist.lock);

    if (persist.slave) {
        snd_pcm_drop(persist.slave);
        snd_pcm_close(persist.slave);
    }

    persist.slave       = pcm;
    persist.rate        = kd->io.rate;
    persist.channels    = kd->io.channels;
    persist.format      = kd->io.format;
    persist.period_size = period;
    persist.buffer_size = buffer;
    persist.configured  = 1;
    snprintf(persist.slave_name, sizeof(persist.slave_name),
             "%s", kd->slave_name);

    /* reallocate silence buffer for new format */
    {
        unsigned int fb = (snd_pcm_format_physical_width(kd->io.format) / 8)
                          * kd->io.channels;
        free(persist.silence_buf);
        persist.silence_buf = calloc(period, fb);
        persist.silence_frames = period;
        persist.frame_bytes = fb;
    }

    pthread_mutex_unlock(&persist.lock);

    /* resume silence thread feeding with new format */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);

    return 0;

fail:
    snd_pcm_close(pcm);
resume_silence:
    /* resume silence on failure */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);
    return err;
}

/* check if persistent slave matches current parameters */
static int slave_matches(struct keepalive_data *kd)
{
    int match = 0;

    pthread_mutex_lock(&persist.lock);
    if (persist.slave && persist.configured &&
        persist.rate     == kd->io.rate &&
        persist.channels == kd->io.channels &&
        persist.format   == kd->io.format &&
        strcmp(persist.slave_name, kd->slave_name) == 0) {
        match = 1;
    }
    pthread_mutex_unlock(&persist.lock);

    return match;
}

/* --------------------------------------------------------------------------
 * ioplug callbacks
 * -------------------------------------------------------------------------- */

static int keepalive_hw_params(snd_pcm_ioplug_t *io,
                               snd_pcm_hw_params_t *params)
{
    struct keepalive_data *kd = io->private_data;
    int err;

    (void)params;

    /* open or reconfigure the slave if format changed */
    if (!slave_matches(kd)) {
        err = open_slave(kd);
        if (err < 0)
            return err;
    }

    /* cache frame_bytes for transfer offset calculation */
    kd->frame_bytes = persist.frame_bytes;

    /* ensure the persistent silence thread is running */
    ensure_silence_thread();

    return 0;
}

static int keepalive_prepare(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;

    kd->hw_ptr = 0;

    /* ensure slave is prepared */
    pthread_mutex_lock(&persist.lock);
    if (persist.slave)
        snd_pcm_prepare(persist.slave);
    pthread_mutex_unlock(&persist.lock);

    return 0;
}

static int keepalive_start(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;

    (void)kd;

    /* switch to audio passthrough: pause the silence thread */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 1;
    pthread_cond_broadcast(&persist.thread_cond);

    /* wait for any in-progress silence write to finish */
    while (persist.in_write)
        pthread_cond_wait(&persist.thread_cond, &persist.thread_mtx);

    pthread_mutex_unlock(&persist.thread_mtx);

    /* signal eventfd so framework starts calling transfer */
    eventfd_signal(kd->event_fd);

    return 0;
}

static int keepalive_stop(snd_pcm_ioplug_t *io)
{
    (void)io;

    /* switch to silence mode: resume the silence thread */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);

    return 0;
}

static snd_pcm_sframes_t keepalive_transfer(snd_pcm_ioplug_t *io,
    const snd_pcm_channel_area_t *areas,
    snd_pcm_uframes_t offset,
    snd_pcm_uframes_t size)
{
    struct keepalive_data *kd = io->private_data;
    snd_pcm_t *slave;
    snd_pcm_sframes_t ret;

    /* calculate pointer to audio data in the ioplug mmap buffer */
    const char *buf = (const char *)areas[0].addr
                    + (areas[0].first / 8)
                    + (offset * kd->frame_bytes);

    pthread_mutex_lock(&persist.lock);
    slave = persist.slave;
    pthread_mutex_unlock(&persist.lock);

    if (!slave)
        return -ENODEV;

    ret = snd_pcm_writei(slave, buf, size);

    if (ret == -EPIPE) {
        snd_pcm_prepare(slave);
        ret = snd_pcm_writei(slave, buf, size);
    }

    if (ret > 0) {
        kd->hw_ptr += ret;
        eventfd_signal(kd->event_fd);
    }

    return ret;
}

static snd_pcm_sframes_t keepalive_pointer(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;
    return kd->hw_ptr % io->buffer_size;
}

static int keepalive_drain(snd_pcm_ioplug_t *io)
{
    (void)io;

    /* drain remaining audio from the slave, then switch to silence */
    pthread_mutex_lock(&persist.lock);
    if (persist.slave)
        snd_pcm_drain(persist.slave);
    pthread_mutex_unlock(&persist.lock);

    /* prepare slave for next use (silence or audio) */
    pthread_mutex_lock(&persist.lock);
    if (persist.slave)
        snd_pcm_prepare(persist.slave);
    pthread_mutex_unlock(&persist.lock);

    /* resume silence */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);

    return 0;
}

static int keepalive_close(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;

    /* resume silence thread if this instance was active.
     * do NOT stop the thread - it persists across instances.
     * do NOT close the slave - it persists across instances. */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);

    if (kd->event_fd >= 0)
        close(kd->event_fd);

    free(kd->slave_name);
    free(kd);

    return 0;
}

static int keepalive_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
    (void)io;
    return 1;
}

static int keepalive_poll_descriptors(snd_pcm_ioplug_t *io,
    struct pollfd *pfd, unsigned int space)
{
    struct keepalive_data *kd = io->private_data;

    if (space < 1)
        return 0;

    pfd[0].fd      = kd->event_fd;
    pfd[0].events  = POLLIN;
    pfd[0].revents = 0;

    return 1;
}

static int keepalive_poll_revents(snd_pcm_ioplug_t *io,
    struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
    struct keepalive_data *kd = io->private_data;

    (void)nfds;

    /* consume eventfd notification */
    if (pfd[0].revents & POLLIN)
        eventfd_consume(kd->event_fd);

    /* always report writable - slave flow control handles pacing */
    *revents = io->stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;

    return 0;
}

/* --------------------------------------------------------------------------
 * Callback table
 * -------------------------------------------------------------------------- */

static const snd_pcm_ioplug_callback_t keepalive_callbacks = {
    .hw_params              = keepalive_hw_params,
    .prepare                = keepalive_prepare,
    .start                  = keepalive_start,
    .stop                   = keepalive_stop,
    .transfer               = keepalive_transfer,
    .pointer                = keepalive_pointer,
    .drain                  = keepalive_drain,
    .close                  = keepalive_close,
    .poll_descriptors_count = keepalive_poll_descriptors_count,
    .poll_descriptors       = keepalive_poll_descriptors,
    .poll_revents           = keepalive_poll_revents,
};

/* --------------------------------------------------------------------------
 * Plugin constructor
 *
 * Called by ALSA when a PCM of type "keepalive" is opened.
 * Parses configuration and creates the ioplug instance.
 * -------------------------------------------------------------------------- */

SND_PCM_PLUGIN_DEFINE_FUNC(keepalive)
{
    snd_config_iterator_t i, next;
    snd_config_t *slave_conf = NULL;
    const char *slave_pcm = NULL;
    struct keepalive_data *kd;
    int err;

    /* parse configuration */
    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;

        if (snd_config_get_id(n, &id) < 0)
            continue;

        if (strcmp(id, "type") == 0 || strcmp(id, "comment") == 0)
            continue;

        if (strcmp(id, "slave") == 0) {
            slave_conf = n;
            continue;
        }

        SNDERR("keepalive: unknown config field '%s'", id);
        return -EINVAL;
    }

    /* get slave PCM name */
    if (slave_conf) {
        snd_config_iterator_t si, snext;
        snd_config_for_each(si, snext, slave_conf) {
            snd_config_t *sn = snd_config_iterator_entry(si);
            const char *sid;

            if (snd_config_get_id(sn, &sid) < 0)
                continue;

            if (strcmp(sid, "pcm") == 0) {
                if (snd_config_get_string(sn, &slave_pcm) < 0) {
                    SNDERR("keepalive: slave.pcm must be a string");
                    return -EINVAL;
                }
            }
        }
    }

    if (!slave_pcm) {
        SNDERR("keepalive: missing slave.pcm");
        return -EINVAL;
    }

    /* allocate instance */
    kd = calloc(1, sizeof(*kd));
    if (!kd)
        return -ENOMEM;

    kd->slave_name = strdup(slave_pcm);
    if (!kd->slave_name) {
        free(kd);
        return -ENOMEM;
    }

    /* eventfd for poll */
    kd->event_fd = eventfd(0, EFD_NONBLOCK);
    if (kd->event_fd < 0) {
        err = -errno;
        free(kd->slave_name);
        free(kd);
        return err;
    }

    /* create the ioplug */
    kd->io.version  = SND_PCM_IOPLUG_VERSION;
    kd->io.name     = "ALSA keepalive proxy";
    kd->io.callback = &keepalive_callbacks;
    kd->io.private_data = kd;
    kd->io.poll_fd  = kd->event_fd;
    kd->io.poll_events = POLLIN;
    kd->io.mmap_rw  = 1;

    err = snd_pcm_ioplug_create(&kd->io, name, stream, mode);
    if (err < 0) {
        close(kd->event_fd);
        free(kd->slave_name);
        free(kd);
        return err;
    }

    /* set hardware parameter constraints */
    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_CHANNELS, 1, 8);
    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_RATE, 8000, 384000);
    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_PERIOD_BYTES, 128, 2048 * 1024);
    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_BUFFER_BYTES, 256, 4096 * 1024);

    /* accept common PCM formats */
    {
        static const unsigned int fmts[] = {
            SND_PCM_FORMAT_S8,
            SND_PCM_FORMAT_S16_LE,
            SND_PCM_FORMAT_S16_BE,
            SND_PCM_FORMAT_S24_LE,
            SND_PCM_FORMAT_S24_BE,
            SND_PCM_FORMAT_S24_3LE,
            SND_PCM_FORMAT_S24_3BE,
            SND_PCM_FORMAT_S32_LE,
            SND_PCM_FORMAT_S32_BE,
            SND_PCM_FORMAT_FLOAT_LE,
        };
        snd_pcm_ioplug_set_param_list(&kd->io,
            SND_PCM_IOPLUG_HW_FORMAT,
            sizeof(fmts) / sizeof(fmts[0]),
            fmts);
    }

    *pcmp = kd->io.pcm;
    return 0;
}

SND_PCM_PLUGIN_SYMBOL(keepalive);

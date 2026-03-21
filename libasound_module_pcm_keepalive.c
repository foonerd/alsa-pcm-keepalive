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
 * Data flow model (based on Volumio's volumioswitch architecture):
 * - transfer callback: accepts data into ioplug mmap buffer, returns size
 * - pointer callback: reads timerfd, writes from mmap buffer to slave,
 *   advances hw_ptr (this is where actual I/O happens)
 * - poll: timerfd (pacing) + eventfd (space available signaling)
 * - silence thread: feeds -100dB noise to slave when no client is active
 *
 * Copyright (C) 2026 Just a Nerd
 * License: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

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
 * Persistent state - survives close/reopen cycles
 * -------------------------------------------------------------------------- */

static struct keepalive_persist {
    pthread_mutex_t     lock;

    snd_pcm_t          *slave;
    char                slave_name[256];
    unsigned int        rate;
    unsigned int        channels;
    snd_pcm_format_t    format;
    snd_pcm_uframes_t   period_size;
    snd_pcm_uframes_t   buffer_size;
    int                 configured;

    /* silence thread */
    pthread_t           thread;
    pthread_mutex_t     thread_mtx;
    pthread_cond_t      thread_cond;
    int                 thread_running;
    int                 active;         /* 1 = client writing audio */
    int                 in_write;       /* 1 = silence write in progress */

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
    char               *slave_name;

    int                 timer_fd;
    int                 event_fd;
    int                 timer_active;
    int                 event_active;

    unsigned int        frame_bytes;
};

/* --------------------------------------------------------------------------
 * Silence thread (persistent)
 * -------------------------------------------------------------------------- */

static void *silence_thread_fn(void *arg)
{
    snd_pcm_t *slave;
    snd_pcm_sframes_t ret;

    (void)arg;

    while (1) {
        pthread_mutex_lock(&persist.thread_mtx);
        while (persist.active)
            pthread_cond_wait(&persist.thread_cond, &persist.thread_mtx);
        persist.in_write = 1;
        pthread_mutex_unlock(&persist.thread_mtx);

        pthread_mutex_lock(&persist.lock);
        slave = persist.slave;
        pthread_mutex_unlock(&persist.lock);

        if (slave && persist.silence_buf) {
            ret = snd_pcm_writei(slave, persist.silence_buf,
                                 persist.silence_frames);
            if (ret == -EAGAIN) {
                /* nonblock device buffer full - normal on HDMI/iec958,
                 * just skip this cycle and retry next iteration */
            } else if (ret == -EPIPE) {
                snd_pcm_prepare(slave);
                snd_pcm_writei(slave, persist.silence_buf,
                               persist.silence_frames);
            } else if (ret == -ESTRPIPE) {
                while (snd_pcm_resume(slave) == -EAGAIN)
                    usleep(10000);
                snd_pcm_prepare(slave);
            }
        }

        pthread_mutex_lock(&persist.thread_mtx);
        persist.in_write = 0;
        pthread_cond_broadcast(&persist.thread_cond);
        pthread_mutex_unlock(&persist.thread_mtx);

        if (persist.silence_frames && persist.rate)
            usleep((unsigned long)persist.silence_frames * 500000
                   / persist.rate);
        else
            usleep(10000);
    }

    return NULL;
}

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
 * Timer and event helpers (from volumioswitch pattern)
 * -------------------------------------------------------------------------- */

static int set_timer(struct keepalive_data *kd, int on)
{
    if ((kd->timer_active && on) || (!kd->timer_active && !on))
        return 0;

    long long wake_ns = 0;
    if (on) {
        pthread_mutex_lock(&persist.lock);
        snd_pcm_uframes_t ps = persist.period_size;
        pthread_mutex_unlock(&persist.lock);
        if (ps && kd->io.rate)
            wake_ns = (long long)ps * 500000000LL / (long long)kd->io.rate;
        else
            wake_ns = 10000000LL; /* 10ms fallback */
    }

    struct itimerspec timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_nsec = on ? 1 : 0;
    timer.it_interval.tv_sec = wake_ns / 1000000000LL;
    timer.it_interval.tv_nsec = wake_ns % 1000000000LL;

    int err = timerfd_settime(kd->timer_fd, 0, &timer, NULL);
    if (err < 0)
        return -errno;

    kd->timer_active = on;
    return 0;
}

static int set_event(struct keepalive_data *kd, int force_on)
{
    int on = force_on;

    if (!on) {
        switch (kd->io.state) {
        case SND_PCM_STATE_RUNNING:
            on = snd_pcm_ioplug_avail(&kd->io, kd->io.hw_ptr,
                                      kd->io.appl_ptr) >= kd->io.period_size;
            break;
        case SND_PCM_STATE_DRAINING:
            on = 0;
            break;
        default:
            on = 1;
            break;
        }
    }

    if (on && !kd->event_active) {
        eventfd_signal(kd->event_fd);
        kd->event_active = 1;
    } else if (!on && kd->event_active) {
        eventfd_consume(kd->event_fd);
        kd->event_active = 0;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Noise buffer fill (-100dB Gaussian-like noise)
 *
 * Matches Kodi's GenerateNoise() approach: inaudible random noise
 * instead of digital silence. Some AVRs detect prolonged all-zero
 * samples as "no signal" and enter standby despite the PCM stream
 * being in RUNNING state. Noise at -100dB prevents this.
 *
 * -100dB = 10^(-100/20) = 0.00001 of full scale
 * -------------------------------------------------------------------------- */

static void fill_noise_buffer(char *buf, snd_pcm_uframes_t frames,
                              unsigned int channels,
                              snd_pcm_format_t format)
{
    unsigned int sample_bits = snd_pcm_format_physical_width(format);
    unsigned int sample_bytes = sample_bits / 8;
    unsigned int total_samples = frames * channels;
    unsigned int i;

    /* full scale peak for this format, then -100dB */
    long long full_scale = 1LL << (snd_pcm_format_width(format) - 1);
    long long amplitude = (long long)(full_scale * 0.00001);
    if (amplitude < 1)
        amplitude = 1;

    /* seed with fixed value for reproducible pattern */
    unsigned int seed = 0xDEADBEEF;

    for (i = 0; i < total_samples; i++) {
        /* simple PRNG - xorshift32 */
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;

        /* map to [-amplitude, +amplitude] */
        long long val = ((long long)(seed % (unsigned int)(2 * amplitude + 1)))
                        - amplitude;

        /* write as little-endian */
        switch (sample_bytes) {
        case 2: /* S16_LE */
            buf[0] = (char)(val & 0xFF);
            buf[1] = (char)((val >> 8) & 0xFF);
            break;
        case 3: /* S24_3LE */
            buf[0] = (char)(val & 0xFF);
            buf[1] = (char)((val >> 8) & 0xFF);
            buf[2] = (char)((val >> 16) & 0xFF);
            break;
        case 4: /* S24_LE, S32_LE */
            buf[0] = (char)(val & 0xFF);
            buf[1] = (char)((val >> 8) & 0xFF);
            buf[2] = (char)((val >> 16) & 0xFF);
            buf[3] = (char)((val >> 24) & 0xFF);
            break;
        default: /* S8 or unknown - single byte */
            buf[0] = (char)(val & 0xFF);
            break;
        }
        buf += sample_bytes;
    }
}

/* --------------------------------------------------------------------------
 * Slave management
 * -------------------------------------------------------------------------- */

static int open_slave(struct keepalive_data *kd)
{
    snd_pcm_t *pcm;
    snd_pcm_hw_params_t *hw;
    int err;

    /* pause silence thread */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 1;
    pthread_cond_broadcast(&persist.thread_cond);
    while (persist.in_write)
        pthread_cond_wait(&persist.thread_cond, &persist.thread_mtx);
    pthread_mutex_unlock(&persist.thread_mtx);

    err = snd_pcm_open(&pcm, kd->slave_name,
                        SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
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

    snd_pcm_uframes_t period, buffer;
    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);

    err = snd_pcm_prepare(pcm);
    if (err < 0) goto fail;

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

    {
        unsigned int fb = (snd_pcm_format_physical_width(kd->io.format) / 8)
                          * kd->io.channels;
        free(persist.silence_buf);
        persist.silence_buf = malloc(period * fb);
        if (persist.silence_buf)
            fill_noise_buffer(persist.silence_buf, period,
                              kd->io.channels, kd->io.format);
        persist.silence_frames = period;
        persist.frame_bytes = fb;
    }
    pthread_mutex_unlock(&persist.lock);

    /* resume silence thread */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);

    return 0;

fail:
    snd_pcm_close(pcm);
resume_silence:
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);
    return err;
}

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
 * Write from ioplug mmap buffer to slave (called from pointer callback)
 * -------------------------------------------------------------------------- */

static snd_pcm_sframes_t write_to_slave(snd_pcm_ioplug_t *io,
    snd_pcm_sframes_t ptr_offset, snd_pcm_uframes_t size)
{
    snd_pcm_sframes_t written = 0;
    snd_pcm_sframes_t loop_written;
    snd_pcm_uframes_t target_size;

    snd_pcm_uframes_t offset = (io->hw_ptr + ptr_offset) % io->buffer_size;
    snd_pcm_uframes_t remaining = io->buffer_size - offset;

    const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);

    pthread_mutex_lock(&persist.lock);
    snd_pcm_t *slave = persist.slave;
    pthread_mutex_unlock(&persist.lock);

    if (!slave)
        return -ENODEV;

    while (written < (snd_pcm_sframes_t)size) {
        target_size = size - written;
        if (target_size > remaining)
            target_size = remaining;

        char *buf = (char *)areas[0].addr
                  + (areas[0].first + areas[0].step * offset) / 8;

        loop_written = snd_pcm_writei(slave, buf, target_size);

        if (loop_written == -EAGAIN) {
            /* nonblock device buffer full - return what we have so far.
             * pointer callback will advance hw_ptr by partial amount,
             * next timer tick will pick up the rest. */
            break;
        } else if (loop_written == -EPIPE) {
            snd_pcm_prepare(slave);
            loop_written = snd_pcm_writei(slave, buf, target_size);
            if (loop_written <= 0)
                break;
        } else if (loop_written <= 0) {
            break;
        }

        written += loop_written;
        offset = (offset + loop_written) % io->buffer_size;
        remaining = io->buffer_size - offset;
    }

    /* partial writes are OK - return what was written.
     * only return error if nothing was written at all. */
    return written > 0 ? written : -EPIPE;
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

    if (!slave_matches(kd)) {
        err = open_slave(kd);
        if (err < 0)
            return err;
    }

    kd->frame_bytes = persist.frame_bytes;
    ensure_silence_thread();

    return 0;
}

static int keepalive_prepare(snd_pcm_ioplug_t *io)
{
    (void)io;

    /* pause silence thread before touching the slave.
     * prepare is called BEFORE start, so the silence thread
     * may still be running. calling snd_pcm_prepare on the slave
     * while the silence thread is mid-write corrupts the device
     * state on HDMI/iec958 outputs. */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 1;
    pthread_cond_broadcast(&persist.thread_cond);
    while (persist.in_write)
        pthread_cond_wait(&persist.thread_cond, &persist.thread_mtx);
    pthread_mutex_unlock(&persist.thread_mtx);

    pthread_mutex_lock(&persist.lock);
    if (persist.slave)
        snd_pcm_prepare(persist.slave);
    pthread_mutex_unlock(&persist.lock);

    /* leave silence thread paused - start will keep it paused,
     * close/stop will resume it */
    return 0;
}

static int keepalive_start(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;

    /* pause silence thread */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 1;
    pthread_cond_broadcast(&persist.thread_cond);
    while (persist.in_write)
        pthread_cond_wait(&persist.thread_cond, &persist.thread_mtx);
    pthread_mutex_unlock(&persist.thread_mtx);

    /* start timer and signal event */
    int err = set_timer(kd, 1);
    if (err == 0)
        err = set_event(kd, 1);

    return err;
}

static int keepalive_stop(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;

    /* stop timer */
    set_timer(kd, 0);
    set_event(kd, 1);

    /* drop slave to clear buffers */
    pthread_mutex_lock(&persist.lock);
    if (persist.slave)
        snd_pcm_drop(persist.slave);
    pthread_mutex_unlock(&persist.lock);

    /* prepare slave for silence thread */
    pthread_mutex_lock(&persist.lock);
    if (persist.slave)
        snd_pcm_prepare(persist.slave);
    pthread_mutex_unlock(&persist.lock);

    /* resume silence thread */
    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);

    return 0;
}

/* transfer: accept data into ioplug mmap buffer. Do NOT write to slave. */
static snd_pcm_sframes_t keepalive_transfer(snd_pcm_ioplug_t *io,
    const snd_pcm_channel_area_t *areas,
    snd_pcm_uframes_t offset,
    snd_pcm_uframes_t size)
{
    struct keepalive_data *kd = io->private_data;
    (void)areas;
    (void)offset;

    set_event(kd, 0);

    return size;
}

/* pointer: read timer, write buffered data to slave, advance hw_ptr */
static snd_pcm_sframes_t keepalive_pointer(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;
    snd_pcm_sframes_t moved = 0;
    uint64_t timer_val = 0;

    if (io->state == SND_PCM_STATE_XRUN)
        return -EPIPE;

    if (io->state != SND_PCM_STATE_RUNNING &&
        io->state != SND_PCM_STATE_DRAINING)
        return io->hw_ptr;

    /* read timer - non-blocking */
    ssize_t s = read(kd->timer_fd, &timer_val, sizeof(uint64_t));
    if (s != sizeof(uint64_t) || timer_val == 0)
        return io->hw_ptr;

    /* how much data does the client have buffered? */
    snd_pcm_sframes_t buffered = snd_pcm_ioplug_hw_avail(io,
                                    io->hw_ptr, io->appl_ptr);

    /* how much space does the slave have? */
    pthread_mutex_lock(&persist.lock);
    snd_pcm_t *slave = persist.slave;
    snd_pcm_uframes_t target_period = persist.period_size;
    pthread_mutex_unlock(&persist.lock);

    if (!slave)
        return -EPIPE;

    snd_pcm_sframes_t target_avail = snd_pcm_avail(slave);
    if (target_avail < 0)
        return -EPIPE;

    if (target_avail < (snd_pcm_sframes_t)target_period)
        return io->hw_ptr;

    if (buffered >= (snd_pcm_sframes_t)target_period) {
        snd_pcm_state_t slave_state = snd_pcm_state(slave);
        if (slave_state != SND_PCM_STATE_PREPARED &&
            slave_state != SND_PCM_STATE_RUNNING)
            return -EPIPE;

        snd_pcm_sframes_t to_copy = target_period * ((timer_val / 2) + 1);
        while (to_copy > buffered || to_copy > target_avail)
            to_copy -= target_period;

        if (to_copy > 0) {
            moved = write_to_slave(io, 0, to_copy);
            if (moved < 0)
                return -EPIPE;
        }
    }

    set_event(kd, 0);

    return io->hw_ptr + moved;
}

static int keepalive_close(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;

    pthread_mutex_lock(&persist.thread_mtx);
    persist.active = 0;
    pthread_cond_broadcast(&persist.thread_cond);
    pthread_mutex_unlock(&persist.thread_mtx);

    if (kd->timer_fd >= 0)
        close(kd->timer_fd);
    if (kd->event_fd >= 0)
        close(kd->event_fd);

    free(kd->slave_name);
    free(kd);

    return 0;
}

static int keepalive_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
    (void)io;
    return 2;
}

static int keepalive_poll_descriptors(snd_pcm_ioplug_t *io,
    struct pollfd *pfd, unsigned int space)
{
    struct keepalive_data *kd = io->private_data;

    if (space < 2)
        return -EINVAL;

    pfd[0].fd      = kd->timer_fd;
    pfd[0].events  = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd      = kd->event_fd;
    pfd[1].events  = POLLIN;
    pfd[1].revents = 0;

    return 2;
}

static int keepalive_poll_revents(snd_pcm_ioplug_t *io,
    struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
    struct keepalive_data *kd = io->private_data;
    int timer = 0, event = 0;

    *revents = 0;

    for (unsigned int i = 0; i < nfds; i++) {
        if (pfd[i].revents & POLLERR) {
            *revents |= POLLERR;
            return 0;
        }
        if (pfd[i].fd == kd->timer_fd && (pfd[i].revents & POLLIN))
            timer = 1;
        if (pfd[i].fd == kd->event_fd && (pfd[i].revents & POLLIN))
            event = 1;
    }

    if (timer || event) {
        int err = snd_pcm_hwsync(io->pcm);
        if (err == 0) {
            snd_pcm_sframes_t avail = snd_pcm_ioplug_avail(io,
                                        io->hw_ptr, io->appl_ptr);
            if (event || avail > (snd_pcm_sframes_t)io->period_size)
                *revents |= POLLOUT;
        }
    }

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
    .close                  = keepalive_close,
    .poll_descriptors_count = keepalive_poll_descriptors_count,
    .poll_descriptors       = keepalive_poll_descriptors,
    .poll_revents           = keepalive_poll_revents,
};

/* --------------------------------------------------------------------------
 * Plugin constructor
 * -------------------------------------------------------------------------- */

SND_PCM_PLUGIN_DEFINE_FUNC(keepalive)
{
    snd_config_iterator_t i, next;
    snd_config_t *slave_conf = NULL;
    const char *slave_pcm = NULL;
    struct keepalive_data *kd;
    int err;

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

    kd = calloc(1, sizeof(*kd));
    if (!kd)
        return -ENOMEM;

    kd->slave_name = strdup(slave_pcm);
    if (!kd->slave_name) {
        free(kd);
        return -ENOMEM;
    }

    kd->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (kd->timer_fd < 0) {
        err = -errno;
        free(kd->slave_name);
        free(kd);
        return err;
    }

    kd->event_fd = eventfd(0, EFD_NONBLOCK);
    if (kd->event_fd < 0) {
        err = -errno;
        close(kd->timer_fd);
        free(kd->slave_name);
        free(kd);
        return err;
    }

    kd->timer_active = 0;
    kd->event_active = 0;

    kd->io.version      = SND_PCM_IOPLUG_VERSION;
    kd->io.name          = "ALSA keepalive proxy";
    kd->io.callback      = &keepalive_callbacks;
    kd->io.private_data  = kd;
    kd->io.poll_fd       = kd->event_fd;
    kd->io.poll_events   = POLLIN;
    kd->io.mmap_rw       = 1;

    err = snd_pcm_ioplug_create(&kd->io, name, stream, mode);
    if (err < 0) {
        close(kd->timer_fd);
        close(kd->event_fd);
        free(kd->slave_name);
        free(kd);
        return err;
    }

    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_CHANNELS, 1, 8);
    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_RATE, 8000, 384000);
    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_PERIOD_BYTES, 128, 2048 * 1024);
    snd_pcm_ioplug_set_param_minmax(&kd->io,
        SND_PCM_IOPLUG_HW_BUFFER_BYTES, 256, 4096 * 1024);

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

/* -DPIC must be defined at compile time */
SND_PCM_PLUGIN_SYMBOL(keepalive);

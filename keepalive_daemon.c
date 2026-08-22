/*
 * audio-keepalive-daemon
 *
 * Single writer for the Volumio output path. Holds the downstream PCM
 * open permanently and writes -100 dB Gaussian noise mixed with any
 * frames received from the thin ALSA client plugin.
 *
 * Copyright (C) 2026 Just a Nerd
 * License: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include "keepalive.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_PREFIX "audio-keepalive: "
#define RING_PERIODS 8
#define OPEN_RETRY_MS 500
#define INOTIFY_DEBOUNCE_MS 250

static volatile sig_atomic_t g_run = 1;
static volatile sig_atomic_t g_reopen = 0;

static const char *g_pcm_name = KA_PCM_NAME;
static const char *g_fallback_pcm = KA_FALLBACK_PCM;
static const char *g_socket_path = KA_SOCKET_PATH;

static snd_pcm_t *g_pcm;
static unsigned int g_rate = KA_DEFAULT_RATE;
static unsigned int g_channels = KA_DEFAULT_CHANNELS;
static snd_pcm_format_t g_format = KA_DEFAULT_FORMAT;
static snd_pcm_uframes_t g_period;
static snd_pcm_uframes_t g_buffer;
static unsigned int g_frame_bytes;
static int g_configured;

static char *g_noise;
static char *g_mix;
static char *g_music;

static char *g_ring;
static size_t g_ring_cap;
static size_t g_ring_r;
static size_t g_ring_w;
static size_t g_ring_count;

static int g_listen_fd = -1;
static int g_client_fd = -1;
static int g_client_active;
static int g_inotify_fd = -1;
static int g_inotify_wd = -1;
static int g_inotify_data_wd = -1;
static int g_test_mode;
static double g_noise_db = KA_NOISE_DB_IDLE;

static void log_info(const char *fmt, ...)
{
    va_list ap;
    fputs(LOG_PREFIX, stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void log_err(const char *fmt, ...)
{
    va_list ap;
    fputs(LOG_PREFIX, stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        g_reopen = 1;
    else
        g_run = 0;
}

static void ring_reset(void)
{
    g_ring_r = g_ring_w = g_ring_count = 0;
}

static int ring_ensure(size_t frames)
{
    size_t bytes;

    if (frames < (size_t)g_period * RING_PERIODS)
        frames = (size_t)g_period * RING_PERIODS;
    if (frames < 4096)
        frames = 4096;

    bytes = frames * g_frame_bytes;
    if (!bytes)
        return -EINVAL;

    if (g_ring && g_ring_cap == frames)
        return 0;

    free(g_ring);
    g_ring = malloc(bytes);
    if (!g_ring) {
        g_ring_cap = 0;
        return -ENOMEM;
    }
    g_ring_cap = frames;
    ring_reset();
    return 0;
}

static size_t ring_space(void)
{
    if (g_ring_count >= g_ring_cap)
        return 0;
    return g_ring_cap - g_ring_count;
}

static void ring_push(const char *src, size_t frames)
{
    size_t i;

    for (i = 0; i < frames; i++) {
        memcpy(g_ring + g_ring_w * g_frame_bytes, src + i * g_frame_bytes,
               g_frame_bytes);
        g_ring_w++;
        if (g_ring_w == g_ring_cap)
            g_ring_w = 0;
        g_ring_count++;
    }
}

static void ring_pop(char *dst, size_t frames)
{
    size_t i;

    for (i = 0; i < frames; i++) {
        memcpy(dst + i * g_frame_bytes, g_ring + g_ring_r * g_frame_bytes,
               g_frame_bytes);
        g_ring_r++;
        if (g_ring_r == g_ring_cap)
            g_ring_r = 0;
        g_ring_count--;
    }
}

static void close_output(void)
{
    if (g_pcm) {
        snd_pcm_drop(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
    g_configured = 0;
}

static int setup_buffers(void)
{
    free(g_noise);
    free(g_mix);
    free(g_music);
    g_noise = g_mix = g_music = NULL;

    if (!g_period || !g_frame_bytes)
        return -EINVAL;

    g_noise = malloc(g_period * g_frame_bytes);
    g_mix = malloc(g_period * g_frame_bytes);
    g_music = malloc(g_period * g_frame_bytes);
    if (!g_noise || !g_mix || !g_music)
        return -ENOMEM;

    ka_fill_noise(g_noise, g_period, g_channels, g_format, g_noise_db);
    return ring_ensure(g_buffer ? (size_t)g_buffer : (size_t)g_period * RING_PERIODS);
}

static double sentinel_db(int *present)
{
    FILE *f;
    char buf[64];
    double db = KA_NOISE_DB_TEST;

    f = fopen(KA_SENTINEL_PATH, "r");
    if (!f) {
        if (present)
            *present = 0;
        return KA_NOISE_DB_IDLE;
    }
    if (present)
        *present = 1;

    if (fgets(buf, sizeof(buf), f)) {
        char *end = NULL;
        double v = strtod(buf, &end);
        if (end != buf) {
            if (v > 0.0)
                v = -v;
            if (v > KA_NOISE_DB_MAX)
                v = KA_NOISE_DB_MAX;
            if (v < KA_NOISE_DB_MIN)
                v = KA_NOISE_DB_MIN;
            db = v;
        }
    }
    fclose(f);
    return db;
}

static void apply_sentinel(int force_log)
{
    int present = 0;
    double db = sentinel_db(&present);

    if (!force_log && present == g_test_mode && db == g_noise_db)
        return;

    g_test_mode = present;
    g_noise_db = db;

    if (g_noise && g_period && g_frame_bytes)
        ka_fill_noise(g_noise, g_period, g_channels, g_format, g_noise_db);

    if (present)
        log_info("test sentinel %s — audible noise %.0f dB",
                 KA_SENTINEL_PATH, db);
    else
        log_info("no test sentinel — idle noise %.0f dB", db);
}

static int open_named(const char *name, unsigned int rate, unsigned int channels,
                      snd_pcm_format_t format, snd_pcm_uframes_t want_period,
                      snd_pcm_uframes_t want_buffer)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_t *pcm;
    snd_pcm_uframes_t period = want_period ? want_period : 1024;
    snd_pcm_uframes_t buffer = want_buffer ? want_buffer : period * 4;
    int err;

    err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0)
        return err;

    snd_pcm_hw_params_alloca(&hw);
    err = snd_pcm_hw_params_any(pcm, hw);
    if (err < 0)
        goto fail;

    err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0)
        goto fail;

    err = snd_pcm_hw_params_set_format(pcm, hw, format);
    if (err < 0)
        goto fail;

    err = snd_pcm_hw_params_set_channels(pcm, hw, channels);
    if (err < 0)
        goto fail;

    err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
    if (err < 0)
        goto fail;

    err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    if (err < 0)
        goto fail;

    err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    if (err < 0)
        goto fail;

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0)
        goto fail;

    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);

    snd_pcm_sw_params_alloca(&sw);
    err = snd_pcm_sw_params_current(pcm, sw);
    if (err < 0)
        goto fail;
    err = snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
    if (err < 0)
        goto fail;
    err = snd_pcm_sw_params(pcm, sw);
    if (err < 0)
        goto fail;

    err = snd_pcm_prepare(pcm);
    if (err < 0)
        goto fail;

    g_pcm = pcm;
    g_rate = rate;
    g_channels = channels;
    g_format = format;
    g_period = period;
    g_buffer = buffer;
    g_frame_bytes = ka_frame_bytes(format, channels);
    g_configured = 1;

    err = setup_buffers();
    if (err < 0) {
        close_output();
        return err;
    }

    log_info("opened %s rate=%u ch=%u fmt=%s period=%lu buffer=%lu",
             name, rate, channels, snd_pcm_format_name(format),
             (unsigned long)period, (unsigned long)buffer);
    return 0;

fail:
    snd_pcm_close(pcm);
    return err;
}

static int open_output(unsigned int rate, unsigned int channels,
                       snd_pcm_format_t format, snd_pcm_uframes_t period,
                       snd_pcm_uframes_t buffer)
{
    int err;

    /* hw/iec958 is exclusive. Opening a second handle while we still
     * own the device returns EBUSY (and MPD then faults on OPEN_ERR). */
    close_output();

    err = open_named(g_pcm_name, rate, channels, format, period, buffer);
    if (err < 0 && g_fallback_pcm && strcmp(g_pcm_name, g_fallback_pcm) != 0) {
        log_err("open %s failed: %s — trying %s",
                g_pcm_name, snd_strerror(err), g_fallback_pcm);
        err = open_named(g_fallback_pcm, rate, channels, format, period, buffer);
    }
    if (err < 0)
        log_err("open output failed: %s", snd_strerror(err));
    return err;
}

static int recover_output(int err)
{
    if (!g_pcm)
        return err;

    if (err == -EPIPE) {
        err = snd_pcm_prepare(g_pcm);
        return err;
    }
    if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(g_pcm)) == -EAGAIN)
            usleep(10000);
        if (err < 0)
            err = snd_pcm_prepare(g_pcm);
        return err;
    }
    if (err == -ENODEV || err == -EBADFD || err == -ENXIO) {
        log_err("output vanished (%s) — reopening", snd_strerror(err));
        return open_output(g_rate, g_channels, g_format, g_period, g_buffer);
    }
    return err;
}

static void write_period(void)
{
    snd_pcm_sframes_t avail;
    snd_pcm_sframes_t ret;
    int have_music = 0;

    if (!g_pcm || !g_configured)
        return;

    avail = snd_pcm_avail_update(g_pcm);
    if (avail < 0) {
        recover_output((int)avail);
        return;
    }
    if (avail < (snd_pcm_sframes_t)g_period)
        return;

    if (g_client_active && g_ring_count >= g_period) {
        ring_pop(g_music, g_period);
        ka_mix(g_mix, g_music, g_noise, g_period, g_channels, g_format);
        have_music = 1;
    }
    if (!have_music)
        memcpy(g_mix, g_noise, (size_t)g_period * g_frame_bytes);

    ret = snd_pcm_writei(g_pcm, g_mix, g_period);
    if (ret == -EAGAIN)
        return;
    if (ret < 0) {
        recover_output((int)ret);
        return;
    }
}

static void close_client(void)
{
    if (g_client_fd >= 0) {
        close(g_client_fd);
        g_client_fd = -1;
    }
    g_client_active = 0;
    ring_reset();
}

static void handle_open(const struct ka_open *req)
{
    struct ka_open_ok ok;
    struct ka_open_err er;
    int need_reopen = 0;
    int err = 0;

    if (!req->rate || !req->channels) {
        er.err = -EINVAL;
        ka_send_msg(g_client_fd, KA_MSG_OPEN_ERR, &er, sizeof(er));
        return;
    }

    if (!g_configured ||
        g_rate != req->rate ||
        g_channels != req->channels ||
        g_format != (snd_pcm_format_t)req->format)
        need_reopen = 1;

    if (need_reopen) {
        log_info("reopen for client rate=%u ch=%u fmt=%s (was %u/%u/%s)",
                 req->rate, req->channels,
                 snd_pcm_format_name((snd_pcm_format_t)req->format),
                 g_rate, g_channels, snd_pcm_format_name(g_format));
        err = open_output(req->rate, req->channels,
                          (snd_pcm_format_t)req->format,
                          req->period_size, req->buffer_size);
        if (err < 0) {
            er.err = err;
            ka_send_msg(g_client_fd, KA_MSG_OPEN_ERR, &er, sizeof(er));
            return;
        }
    } else if (req->buffer_size) {
        ring_ensure(req->buffer_size);
    }

    ring_reset();
    g_client_active = 1;

    ok.period_size = (uint32_t)g_period;
    ok.buffer_size = (uint32_t)g_ring_cap;
    ok.frame_bytes = g_frame_bytes;
    ka_send_msg(g_client_fd, KA_MSG_OPEN_OK, &ok, sizeof(ok));

    log_info("client open rate=%u ch=%u fmt=%s%s",
             req->rate, req->channels,
             snd_pcm_format_name((snd_pcm_format_t)req->format),
             need_reopen ? " (reopened output)" : "");
}

static void handle_data(const char *payload, uint32_t size)
{
    struct ka_ack ack;
    size_t frames;
    size_t take;

    if (!g_frame_bytes || !g_client_active) {
        ack.frames = -EINVAL;
        ack.avail = 0;
        ka_send_msg(g_client_fd, KA_MSG_ACK, &ack, sizeof(ack));
        return;
    }

    frames = size / g_frame_bytes;
    take = frames;
    if (take > ring_space())
        take = ring_space();

    if (take)
        ring_push(payload, take);

    ack.frames = (int32_t)take;
    ack.avail = (uint32_t)ring_space();
    ka_send_msg(g_client_fd, KA_MSG_ACK, &ack, sizeof(ack));
}

static void handle_client(void)
{
    struct ka_hdr hdr;
    char *payload = NULL;
    int err;

    err = ka_recv_hdr(g_client_fd, &hdr);
    if (err < 0)
        goto drop;

    if (hdr.size) {
        payload = malloc(hdr.size);
        if (!payload)
            goto drop;
        err = ka_recv_payload(g_client_fd, payload, hdr.size);
        if (err < 0)
            goto drop;
    }

    switch (hdr.type) {
    case KA_MSG_OPEN:
        if (hdr.size < sizeof(struct ka_open))
            goto drop;
        handle_open((const struct ka_open *)payload);
        break;
    case KA_MSG_DATA:
        handle_data(payload, hdr.size);
        break;
    case KA_MSG_CLOSE:
        log_info("client close — continuing noise");
        g_client_active = 0;
        ring_reset();
        break;
    default:
        break;
    }

    free(payload);
    return;

drop:
    free(payload);
    log_info("client disconnected");
    close_client();
}

static void accept_client(void)
{
    int fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0)
        return;

    if (g_client_fd >= 0) {
        log_info("replacing existing client");
        close_client();
    }

    g_client_fd = fd;
    g_client_active = 0;
    log_info("client connected");
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[256];
    size_t len;
    size_t i;

    if (!path || !*path)
        return -EINVAL;
    len = strlen(path);
    if (len >= sizeof(tmp))
        return -ENAMETOOLONG;
    memcpy(tmp, path, len + 1);

    for (i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -errno;
        tmp[i] = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -errno;
    return 0;
}

static int setup_socket(void)
{
    struct sockaddr_un addr;
    char dir[256];
    char *slash;
    int fd;
    int err;

    snprintf(dir, sizeof(dir), "%s", g_socket_path);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        err = mkdir_p(dir, 0755);
        if (err < 0)
            return err;
    }

    unlink(g_socket_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        err = -errno;
        close(fd);
        return err;
    }

    chmod(g_socket_path, 0666);

    if (listen(fd, 2) < 0) {
        err = -errno;
        close(fd);
        unlink(g_socket_path);
        return err;
    }

    g_listen_fd = fd;
    log_info("listening on %s", g_socket_path);
    return 0;
}

static void setup_inotify(void)
{
    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify_fd < 0)
        return;
    g_inotify_wd = inotify_add_watch(g_inotify_fd, "/etc",
                                     IN_MOVED_TO | IN_CREATE | IN_MODIFY);
    g_inotify_data_wd = inotify_add_watch(g_inotify_fd, "/data",
                                          IN_MOVED_TO | IN_MOVED_FROM |
                                          IN_CREATE | IN_DELETE | IN_MODIFY);
    if (g_inotify_wd < 0 && g_inotify_data_wd < 0) {
        close(g_inotify_fd);
        g_inotify_fd = -1;
    }
}

static void drain_inotify(void)
{
    char buf[4096];
    ssize_t n;
    int asound_hit = 0;
    int sentinel_hit = 0;

    if (g_inotify_fd < 0)
        return;

    n = read(g_inotify_fd, buf, sizeof(buf));
    if (n <= 0)
        return;

    {
        char *p = buf;
        while (p < buf + n) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->len && strcmp(ev->name, "asound.conf") == 0)
                asound_hit = 1;
            if (ev->len && strcmp(ev->name, "keepalive") == 0)
                sentinel_hit = 1;
            p += sizeof(*ev) + ev->len;
        }
    }

    if (asound_hit) {
        log_info("asound.conf changed — reopening output");
        usleep(INOTIFY_DEBOUNCE_MS * 1000);
        open_output(g_rate, g_channels, g_format, g_period, g_buffer);
    }
    if (sentinel_hit)
        apply_sentinel(0);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --pcm NAME           Output PCM (default %s)\n"
            "  --fallback-pcm NAME  Fallback PCM (default %s)\n"
            "  --socket PATH        Control socket (default %s)\n",
            argv0, KA_PCM_NAME, KA_FALLBACK_PCM, KA_SOCKET_PATH);
}

int main(int argc, char **argv)
{
    static struct option opts[] = {
        { "pcm", required_argument, NULL, 'p' },
        { "fallback-pcm", required_argument, NULL, 'f' },
        { "socket", required_argument, NULL, 's' },
        { "help", no_argument, NULL, 'h' },
        { 0, 0, 0, 0 }
    };
    int err;
    unsigned long last_open_try = 0;

    while (1) {
        int c = getopt_long(argc, argv, "p:f:s:h", opts, NULL);
        if (c < 0)
            break;
        switch (c) {
        case 'p':
            g_pcm_name = optarg;
            break;
        case 'f':
            g_fallback_pcm = optarg;
            break;
        case 's':
            g_socket_path = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    err = setup_socket();
    if (err < 0) {
        log_err("socket setup failed: %s", strerror(-err));
        return 1;
    }
    setup_inotify();
    apply_sentinel(1);

    log_info("starting — continuous mix, no volume control");

    while (g_run) {
        struct pollfd pfds[8];
        int nfds = 0;
        int timeout = OPEN_RETRY_MS;
        int i;
        static unsigned long last_sentinel_check;

        if (g_reopen) {
            g_reopen = 0;
            log_info("SIGHUP — reopening output");
            open_output(g_rate, g_channels, g_format, g_period, g_buffer);
        }

        if (!g_configured) {
            struct timespec ts;
            unsigned long now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (unsigned long)ts.tv_sec * 1000ul
                + (unsigned long)ts.tv_nsec / 1000000ul;
            if (now - last_open_try >= OPEN_RETRY_MS) {
                last_open_try = now;
                open_output(g_rate, g_channels, g_format, 1024, 4096);
            }
        }

        {
            struct timespec ts;
            unsigned long now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (unsigned long)ts.tv_sec * 1000ul
                + (unsigned long)ts.tv_nsec / 1000000ul;
            if (now - last_sentinel_check >= 2000ul) {
                last_sentinel_check = now;
                if (g_inotify_fd >= 0 && g_inotify_data_wd < 0)
                    g_inotify_data_wd = inotify_add_watch(
                        g_inotify_fd, "/data",
                        IN_MOVED_TO | IN_MOVED_FROM |
                        IN_CREATE | IN_DELETE | IN_MODIFY);
                apply_sentinel(0);
            }
        }

        pfds[nfds].fd = g_listen_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;

        if (g_client_fd >= 0) {
            pfds[nfds].fd = g_client_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (g_inotify_fd >= 0) {
            pfds[nfds].fd = g_inotify_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (g_pcm) {
            int n = snd_pcm_poll_descriptors_count(g_pcm);
            if (n > 0 && nfds + n <= (int)(sizeof(pfds) / sizeof(pfds[0]))) {
                snd_pcm_poll_descriptors(g_pcm, &pfds[nfds], (unsigned int)n);
                nfds += n;
            }
            timeout = 20;
        }

        err = poll(pfds, (nfds_t)nfds, timeout);
        if (err < 0) {
            if (errno == EINTR)
                continue;
            log_err("poll: %s", strerror(errno));
            break;
        }

        if (pfds[0].revents & POLLIN)
            accept_client();

        for (i = 1; i < nfds; i++) {
            if (g_client_fd >= 0 && pfds[i].fd == g_client_fd &&
                (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (pfds[i].revents & POLLIN)
                    handle_client();
                else
                    close_client();
            }
            if (g_inotify_fd >= 0 && pfds[i].fd == g_inotify_fd &&
                (pfds[i].revents & POLLIN))
                drain_inotify();
        }

        /* Hardware clock and poll timeout both drive a period write so
         * idle noise never depends on a client being connected. */
        write_period();
    }

    close_client();
    close_output();
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    unlink(g_socket_path);
    if (g_inotify_fd >= 0)
        close(g_inotify_fd);
    free(g_ring);
    free(g_noise);
    free(g_mix);
    free(g_music);

    log_info("stopped");
    return 0;
}

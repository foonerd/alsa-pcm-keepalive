/*
 * libasound_module_pcm_keepalive.c - thin virtual PCM client
 *
 * Forwards player samples and timing to audio-keepalive-daemon.
 * Does not open hardware and has no silence thread.
 *
 * Copyright (C) 2026 Just a Nerd
 * License: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include "keepalive.h"

#include <alsa/pcm_external.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

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

struct keepalive_data {
    snd_pcm_ioplug_t io;
    char *socket_path;

    int sock;
    int opened;
    int timer_fd;
    int event_fd;
    int timer_active;
    int event_active;

    unsigned int frame_bytes;
    uint32_t daemon_avail;
};

static int set_timer(struct keepalive_data *kd, int on)
{
    struct itimerspec timer;
    long long wake_ns = 0;
    int err;

    if ((kd->timer_active && on) || (!kd->timer_active && !on))
        return 0;

    if (on) {
        if (kd->io.period_size && kd->io.rate)
            wake_ns = (long long)kd->io.period_size * 500000000LL
                    / (long long)kd->io.rate;
        else
            wake_ns = 10000000LL;
    }

    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_nsec = on ? 1 : 0;
    timer.it_interval.tv_sec = wake_ns / 1000000000LL;
    timer.it_interval.tv_nsec = wake_ns % 1000000000LL;

    err = timerfd_settime(kd->timer_fd, 0, &timer, NULL);
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

static void disconnect_daemon(struct keepalive_data *kd)
{
    if (kd->sock >= 0) {
        ka_send_msg(kd->sock, KA_MSG_CLOSE, NULL, 0);
        close(kd->sock);
        kd->sock = -1;
    }
    kd->daemon_avail = 0;
    kd->opened = 0;
}

static int connect_daemon(struct keepalive_data *kd)
{
    struct sockaddr_un addr;
    struct timeval tv;
    int fd;
    int attempt;
    int err;

    if (kd->sock >= 0)
        return 0;

    for (attempt = 0; attempt < 20; attempt++) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -errno;

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
                 kd->socket_path ? kd->socket_path : KA_SOCKET_PATH);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            kd->sock = fd;
            return 0;
        }

        err = -errno;
        close(fd);
        usleep(50000);
    }
    return err;
}

static int open_stream(struct keepalive_data *kd)
{
    struct ka_open req;
    struct ka_hdr hdr;
    struct ka_open_ok ok;
    struct ka_open_err er;
    int err;

    err = connect_daemon(kd);
    if (err < 0)
        return err;

    memset(&req, 0, sizeof(req));
    req.rate = kd->io.rate;
    req.channels = kd->io.channels;
    req.format = (uint32_t)kd->io.format;
    req.period_size = (uint32_t)kd->io.period_size;
    req.buffer_size = (uint32_t)kd->io.buffer_size;

    err = ka_send_msg(kd->sock, KA_MSG_OPEN, &req, sizeof(req));
    if (err < 0)
        goto fail;

    err = ka_recv_hdr(kd->sock, &hdr);
    if (err < 0)
        goto fail;

    if (hdr.type == KA_MSG_OPEN_OK && hdr.size >= sizeof(ok)) {
        err = ka_recv_payload(kd->sock, &ok, sizeof(ok));
        if (err < 0)
            goto fail;
        kd->frame_bytes = ok.frame_bytes
                        ? ok.frame_bytes
                        : ka_frame_bytes(kd->io.format, kd->io.channels);
        kd->daemon_avail = ok.buffer_size;
        kd->opened = 1;
        return 0;
    }

    if (hdr.type == KA_MSG_OPEN_ERR) {
        if (hdr.size >= sizeof(er))
            ka_recv_payload(kd->sock, &er, sizeof(er));
        else if (hdr.size)
            ka_recv_payload(kd->sock, &er, hdr.size);
        err = (hdr.size >= sizeof(er) && er.err) ? er.err : -EIO;
        goto fail;
    }

    err = -EPROTO;
fail:
    disconnect_daemon(kd);
    return err;
}

static int send_frames(struct keepalive_data *kd, const char *buf,
                       snd_pcm_uframes_t frames)
{
    struct ka_hdr hdr;
    struct ka_ack ack;
    uint32_t bytes;
    int err;

    if (kd->sock < 0 || !frames || !kd->frame_bytes)
        return -ENODEV;

    bytes = (uint32_t)frames * kd->frame_bytes;
    if (bytes > KA_MAX_PAYLOAD)
        return -EINVAL;

    err = ka_send_msg(kd->sock, KA_MSG_DATA, buf, bytes);
    if (err < 0)
        return err;

    err = ka_recv_hdr(kd->sock, &hdr);
    if (err < 0)
        return err;
    if (hdr.type != KA_MSG_ACK || hdr.size < sizeof(ack))
        return -EPROTO;

    err = ka_recv_payload(kd->sock, &ack, sizeof(ack));
    if (err < 0)
        return err;

    if (ack.frames < 0)
        return ack.frames;

    kd->daemon_avail = ack.avail;
    return ack.frames;
}

static snd_pcm_sframes_t write_to_daemon(snd_pcm_ioplug_t *io,
    snd_pcm_sframes_t ptr_offset, snd_pcm_uframes_t size)
{
    struct keepalive_data *kd = io->private_data;
    snd_pcm_sframes_t written = 0;
    snd_pcm_uframes_t offset = (io->hw_ptr + ptr_offset) % io->buffer_size;
    snd_pcm_uframes_t remaining = io->buffer_size - offset;
    const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);

    if (!areas)
        return -ENODEV;

    while (written < (snd_pcm_sframes_t)size) {
        snd_pcm_uframes_t target = size - (snd_pcm_uframes_t)written;
        char *buf;
        int got;

        if (target > remaining)
            target = remaining;
        if (kd->daemon_avail && target > kd->daemon_avail)
            target = kd->daemon_avail;
        if (!target)
            break;

        buf = (char *)areas[0].addr
            + (areas[0].first + areas[0].step * offset) / 8;

        got = send_frames(kd, buf, target);
        if (got == 0)
            break;
        if (got < 0)
            return written > 0 ? written : got;

        written += got;
        offset = (offset + (snd_pcm_uframes_t)got) % io->buffer_size;
        remaining = io->buffer_size - offset;
    }

    return written;
}

static int keepalive_hw_params(snd_pcm_ioplug_t *io,
                               snd_pcm_hw_params_t *params)
{
    struct keepalive_data *kd = io->private_data;
    (void)params;
    return open_stream(kd);
}

static int keepalive_prepare(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;
    if (!kd->opened)
        return -ENODEV;
    return 0;
}

static int keepalive_start(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;
    if (!kd->opened)
        return -ENODEV;
    int err = set_timer(kd, 1);
    if (err == 0)
        err = set_event(kd, 1);
    return err;
}

static int keepalive_stop(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;
    set_timer(kd, 0);
    set_event(kd, 1);
    return 0;
}

static snd_pcm_sframes_t keepalive_transfer(snd_pcm_ioplug_t *io,
    const snd_pcm_channel_area_t *areas,
    snd_pcm_uframes_t offset,
    snd_pcm_uframes_t size)
{
    struct keepalive_data *kd = io->private_data;
    (void)areas;
    (void)offset;
    set_event(kd, 0);
    return (snd_pcm_sframes_t)size;
}

static snd_pcm_sframes_t keepalive_pointer(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;
    snd_pcm_sframes_t moved = 0;
    snd_pcm_sframes_t buffered;
    uint64_t timer_val = 0;
    ssize_t s;

    if (!kd->opened || kd->sock < 0)
        return -EPIPE;

    if (io->state == SND_PCM_STATE_XRUN)
        return -EPIPE;

    if (io->state != SND_PCM_STATE_RUNNING &&
        io->state != SND_PCM_STATE_DRAINING)
        return io->hw_ptr;

    s = read(kd->timer_fd, &timer_val, sizeof(timer_val));
    if (s != sizeof(timer_val) || timer_val == 0)
        return io->hw_ptr;

    buffered = snd_pcm_ioplug_hw_avail(io, io->hw_ptr, io->appl_ptr);
    if (buffered > 0 && kd->sock >= 0) {
        snd_pcm_sframes_t to_copy = buffered;
        if (kd->daemon_avail && to_copy > (snd_pcm_sframes_t)kd->daemon_avail)
            to_copy = (snd_pcm_sframes_t)kd->daemon_avail;
        if (to_copy > 0) {
            moved = write_to_daemon(io, 0, (snd_pcm_uframes_t)to_copy);
            if (moved < 0) {
                disconnect_daemon(kd);
                return -EPIPE;
            }
        }
    }

    set_event(kd, 0);
    return io->hw_ptr + moved;
}

static int keepalive_close(snd_pcm_ioplug_t *io)
{
    struct keepalive_data *kd = io->private_data;

    disconnect_daemon(kd);

    if (kd->timer_fd >= 0)
        close(kd->timer_fd);
    if (kd->event_fd >= 0)
        close(kd->event_fd);
    free(kd->socket_path);
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

    pfd[0].fd = kd->timer_fd;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    pfd[1].fd = kd->event_fd;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0;
    return 2;
}

static int keepalive_poll_revents(snd_pcm_ioplug_t *io,
    struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
    struct keepalive_data *kd = io->private_data;
    int timer = 0, event = 0;
    unsigned int i;

    *revents = 0;

    for (i = 0; i < nfds; i++) {
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

SND_PCM_PLUGIN_DEFINE_FUNC(keepalive)
{
    snd_config_iterator_t i, next;
    const char *socket_path = KA_SOCKET_PATH;
    struct keepalive_data *kd;
    int err;

    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;

        if (snd_config_get_id(n, &id) < 0)
            continue;
        if (strcmp(id, "type") == 0 || strcmp(id, "comment") == 0)
            continue;
        if (strcmp(id, "slave") == 0)
            continue;
        if (strcmp(id, "socket") == 0) {
            if (snd_config_get_string(n, &socket_path) < 0) {
                SNDERR("keepalive: socket must be a string");
                return -EINVAL;
            }
            continue;
        }
        SNDERR("keepalive: unknown config field '%s'", id);
        return -EINVAL;
    }

    kd = calloc(1, sizeof(*kd));
    if (!kd)
        return -ENOMEM;

    kd->socket_path = strdup(socket_path);
    if (!kd->socket_path) {
        free(kd);
        return -ENOMEM;
    }

    kd->sock = -1;
    kd->opened = 0;
    kd->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (kd->timer_fd < 0) {
        err = -errno;
        free(kd->socket_path);
        free(kd);
        return err;
    }

    kd->event_fd = eventfd(0, EFD_NONBLOCK);
    if (kd->event_fd < 0) {
        err = -errno;
        close(kd->timer_fd);
        free(kd->socket_path);
        free(kd);
        return err;
    }

    kd->io.version = SND_PCM_IOPLUG_VERSION;
    kd->io.name = "ALSA keepalive virtual input";
    kd->io.callback = &keepalive_callbacks;
    kd->io.private_data = kd;
    kd->io.poll_fd = kd->event_fd;
    kd->io.poll_events = POLLIN;
    kd->io.mmap_rw = 1;

    err = snd_pcm_ioplug_create(&kd->io, name, stream, mode);
    if (err < 0) {
        close(kd->timer_fd);
        close(kd->event_fd);
        free(kd->socket_path);
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

SND_PCM_PLUGIN_SYMBOL(keepalive);

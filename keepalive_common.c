/*
 * Shared keepalive helpers: idle noise and framed socket I/O.
 *
 * Copyright (C) 2026 Just a Nerd
 * License: GPL-2.0-or-later
 */

#include "keepalive.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

static uint32_t noise_random(void)
{
    static uint32_t state = 0xDEADBEEFu;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static double gaussian_sample(void)
{
    double u1 = ((double)noise_random() + 1.0) / 4294967297.0;
    double u2 = (double)noise_random() / 4294967296.0;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

unsigned int ka_frame_bytes(snd_pcm_format_t format, unsigned int channels)
{
    int phys = snd_pcm_format_physical_width(format);
    if (phys <= 0 || channels == 0)
        return 0;
    return ((unsigned int)phys / 8u) * channels;
}

void ka_fill_noise(char *buf, snd_pcm_uframes_t frames,
                   unsigned int channels, snd_pcm_format_t format,
                   double db)
{
    unsigned int sample_bits = (unsigned int)snd_pcm_format_physical_width(format);
    unsigned int sample_bytes = sample_bits / 8u;
    size_t total_samples = (size_t)frames * channels;
    size_t i;
    int width = snd_pcm_format_width(format);
    int big_endian = snd_pcm_format_big_endian(format) > 0;
    double linear;

    if (!buf || !total_samples || !sample_bytes || width <= 0)
        return;

    if (!isfinite(db))
        db = KA_NOISE_DB_IDLE;
    if (db > 0.0)
        db = -db;
    linear = pow(10.0, db / 20.0);
    if (linear < 0.0)
        linear = 0.0;
    if (linear > 1.0)
        linear = 1.0;

    if (format == SND_PCM_FORMAT_FLOAT_LE) {
        float *fbuf = (float *)buf;
        for (i = 0; i < total_samples; i++) {
            double sample = gaussian_sample() * linear;
            if (sample > 1.0)
                sample = 1.0;
            if (sample < -1.0)
                sample = -1.0;
            fbuf[i] = (float)sample;
        }
        return;
    }

    int64_t max_value = width >= 32 ? INT32_MAX : (1LL << (width - 1)) - 1;
    int64_t min_value = width >= 32 ? INT32_MIN : -(1LL << (width - 1));
    double amplitude = (double)(1LL << (width - 1)) * linear;

    for (i = 0; i < total_samples; i++) {
        int64_t value = (int64_t)llround(gaussian_sample() * amplitude);
        unsigned int byte;

        if (value > max_value)
            value = max_value;
        if (value < min_value)
            value = min_value;

        for (byte = 0; byte < sample_bytes; byte++) {
            unsigned int shift = 8u * (big_endian
                ? sample_bytes - byte - 1u : byte);
            buf[byte] = (char)(((uint64_t)value >> shift) & 0xFFu);
        }
        buf += sample_bytes;
    }
}

int ka_read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (r == 0)
            return -EPIPE;
        got += (size_t)r;
    }
    return 0;
}

int ka_write_full(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t put = 0;

    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (w == 0)
            return -EPIPE;
        put += (size_t)w;
    }
    return 0;
}

int ka_send_msg(int fd, uint32_t type, const void *payload, uint32_t size)
{
    struct ka_hdr hdr;
    int err;

    if (size > KA_MAX_PAYLOAD)
        return -EINVAL;

    hdr.magic = KA_MAGIC;
    hdr.type = type;
    hdr.size = size;

    err = ka_write_full(fd, &hdr, sizeof(hdr));
    if (err < 0)
        return err;
    if (size && payload)
        return ka_write_full(fd, payload, size);
    return 0;
}

int ka_recv_hdr(int fd, struct ka_hdr *hdr)
{
    int err = ka_read_full(fd, hdr, sizeof(*hdr));
    if (err < 0)
        return err;
    if (hdr->magic != KA_MAGIC)
        return -EINVAL;
    if (hdr->size > KA_MAX_PAYLOAD)
        return -EINVAL;
    return 0;
}

int ka_recv_payload(int fd, void *buf, uint32_t size)
{
    if (!size)
        return 0;
    return ka_read_full(fd, buf, size);
}

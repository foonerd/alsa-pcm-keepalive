/*
 * Shared keepalive helpers: noise, mix, framed socket I/O.
 *
 * Copyright (C) 2026 Just a Nerd
 * License: GPL-2.0-or-later
 */

#include "keepalive.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

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
    unsigned int total_samples = (unsigned int)frames * channels;
    unsigned int i;
    int width = snd_pcm_format_width(format);
    double linear;

    if (!buf || !total_samples || !sample_bytes)
        return;

    if (db > 0.0)
        db = -db;
    linear = pow(10.0, db / 20.0);
    if (linear < 0.0)
        linear = 0.0;
    if (linear > 1.0)
        linear = 1.0;

    if (format == SND_PCM_FORMAT_FLOAT_LE) {
        float *fbuf = (float *)buf;
        float amp = (float)linear;
        unsigned int seed = 0xDEADBEEF;
        for (i = 0; i < total_samples; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            fbuf[i] = ((float)(seed % 20001u) - 10000.0f) / 10000.0f * amp;
        }
        return;
    }

    long long full_scale = 1LL << (width > 1 ? (width - 1) : 0);
    long long amplitude = (long long)(full_scale * linear);
    if (amplitude < 1)
        amplitude = 1;

    unsigned int seed = 0xDEADBEEF;

    for (i = 0; i < total_samples; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;

        long long val = ((long long)(seed % (unsigned int)(2 * amplitude + 1)))
                        - amplitude;

        switch (sample_bytes) {
        case 2:
            buf[0] = (char)(val & 0xFF);
            buf[1] = (char)((val >> 8) & 0xFF);
            break;
        case 3:
            buf[0] = (char)(val & 0xFF);
            buf[1] = (char)((val >> 8) & 0xFF);
            buf[2] = (char)((val >> 16) & 0xFF);
            break;
        case 4:
            buf[0] = (char)(val & 0xFF);
            buf[1] = (char)((val >> 8) & 0xFF);
            buf[2] = (char)((val >> 16) & 0xFF);
            buf[3] = (char)((val >> 24) & 0xFF);
            break;
        default:
            buf[0] = (char)(val & 0xFF);
            break;
        }
        buf += sample_bytes;
    }
}

static int32_t unpack_sample(const unsigned char *p, unsigned int bytes, int width)
{
    int32_t v = 0;

    if (bytes == 1) {
        v = (int8_t)p[0];
        return v;
    }

    v = (int32_t)p[0] | ((int32_t)p[1] << 8);
    if (bytes >= 3)
        v |= (int32_t)p[2] << 16;
    if (bytes >= 4)
        v |= (int32_t)p[3] << 24;

    if (bytes < 4 && width > 0 && width < 32) {
        unsigned shift = (unsigned)(32 - width);
        v = (v << shift) >> shift;
    }
    return v;
}

static void pack_sample(unsigned char *p, unsigned int bytes, int32_t v)
{
    unsigned int i;
    for (i = 0; i < bytes; i++)
        p[i] = (unsigned char)((v >> (8 * (int)i)) & 0xFF);
}

void ka_mix(char *dst, const char *music, const char *noise,
            snd_pcm_uframes_t frames, unsigned int channels,
            snd_pcm_format_t format)
{
    unsigned int n = (unsigned int)frames * channels;
    unsigned int i;

    if (!dst || !music || !noise || !n)
        return;

    if (format == SND_PCM_FORMAT_FLOAT_LE) {
        float *d = (float *)dst;
        const float *m = (const float *)music;
        const float *nz = (const float *)noise;
        for (i = 0; i < n; i++) {
            float v = m[i] + nz[i];
            if (v > 1.0f)
                v = 1.0f;
            if (v < -1.0f)
                v = -1.0f;
            d[i] = v;
        }
        return;
    }

    int width = snd_pcm_format_width(format);
    int phys = snd_pcm_format_physical_width(format);
    unsigned int bytes = phys > 0 ? (unsigned int)phys / 8u : 0;
    if (!bytes)
        return;

    int32_t maxv = (width >= 32) ? 0x7fffffff : ((1 << (width - 1)) - 1);
    int32_t minv = (width >= 32) ? (int32_t)0x80000000 : -(1 << (width - 1));

    for (i = 0; i < n; i++) {
        const unsigned char *mp = (const unsigned char *)music + (size_t)i * bytes;
        const unsigned char *np = (const unsigned char *)noise + (size_t)i * bytes;
        unsigned char *dp = (unsigned char *)dst + (size_t)i * bytes;
        int64_t s = (int64_t)unpack_sample(mp, bytes, width)
                  + (int64_t)unpack_sample(np, bytes, width);
        if (s > maxv)
            s = maxv;
        if (s < minv)
            s = minv;
        pack_sample(dp, bytes, (int32_t)s);
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

/*
 * Shared protocol and helpers for the keepalive daemon and ALSA PCM client.
 *
 * Copyright (C) 2026 Just a Nerd
 * License: GPL-2.0-or-later
 */

#ifndef AUDIO_KEEPALIVE_H
#define AUDIO_KEEPALIVE_H

#include <alsa/asoundlib.h>
#include <stdint.h>

#define KA_MAGIC            0x4B414C56u  /* 'KALV' */
#define KA_SOCKET_PATH      "/run/audio-keepalive/ctl.sock"
#define KA_PCM_NAME         "keepaliveProxyOut"
#define KA_FALLBACK_PCM     "volumioOutput"
#define KA_MAX_PAYLOAD      (256u * 1024u)

#define KA_DEFAULT_RATE     48000u
#define KA_DEFAULT_CHANNELS 2u
#define KA_DEFAULT_FORMAT   SND_PCM_FORMAT_S16_LE

#define KA_SENTINEL_PATH    "/data/keepalive"
#define KA_NOISE_DB_IDLE    (-100.0)
#define KA_NOISE_DB_TEST    (-30.0)
#define KA_NOISE_DB_MIN     (-80.0)
#define KA_NOISE_DB_MAX     (-12.0)

enum ka_msg_type {
    KA_MSG_OPEN     = 1,
    KA_MSG_OPEN_OK  = 2,
    KA_MSG_OPEN_ERR = 3,
    KA_MSG_DATA     = 4,
    KA_MSG_ACK      = 5,
    KA_MSG_CLOSE    = 6,
};

struct ka_hdr {
    uint32_t magic;
    uint32_t type;
    uint32_t size;
};

struct ka_open {
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
    uint32_t period_size;
    uint32_t buffer_size;
};

struct ka_open_ok {
    uint32_t period_size;
    uint32_t buffer_size;
    uint32_t frame_bytes;
};

struct ka_open_err {
    int32_t err;
};

struct ka_ack {
    int32_t  frames;
    uint32_t avail;
};

unsigned int ka_frame_bytes(snd_pcm_format_t format, unsigned int channels);

void ka_fill_noise(char *buf, snd_pcm_uframes_t frames,
                   unsigned int channels, snd_pcm_format_t format,
                   double db);

void ka_mix(char *dst, const char *music, const char *noise,
            snd_pcm_uframes_t frames, unsigned int channels,
            snd_pcm_format_t format);

int ka_read_full(int fd, void *buf, size_t n);
int ka_write_full(int fd, const void *buf, size_t n);

int ka_send_msg(int fd, uint32_t type, const void *payload, uint32_t size);
int ka_recv_hdr(int fd, struct ka_hdr *hdr);
int ka_recv_payload(int fd, void *buf, uint32_t size);

#endif

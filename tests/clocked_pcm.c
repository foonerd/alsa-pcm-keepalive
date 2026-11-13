/*
 * LD_PRELOAD shim that turns a "file + null" sink into a device with a
 * real playback clock: the queue drains at the sample rate, writes only
 * fit into free buffer space, and avail/delay are reported from that
 * model. A plain null sink accepts unlimited data and never has delay.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static void *g_top;
static unsigned int g_rate = 48000;
static unsigned long g_buffer = 4096;
static uint64_t g_written;
static uint64_t g_played_base;
static uint64_t g_clock_start_ns;
static int g_clock_running;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t queued(void)
{
    uint64_t played;

    if (!g_clock_running)
        return g_written - g_played_base;
    played = g_played_base
           + (now_ns() - g_clock_start_ns) * g_rate / 1000000000ull;
    if (played >= g_written) {
        /* Underrun: the device stops until the next write. */
        g_played_base = g_written;
        g_clock_running = 0;
        return 0;
    }
    return g_written - played;
}

static long free_frames(void)
{
    uint64_t q = queued();

    return q >= g_buffer ? 0 : (long)(g_buffer - q);
}

int snd_pcm_hw_params_set_rate(void *pcm, void *params, unsigned int rate,
                               int direction)
{
    int (*next)(void *, void *, unsigned int, int) =
        dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_rate");

    g_rate = rate;
    return next(pcm, params, rate, direction);
}

int snd_pcm_hw_params_get_buffer_size(const void *params, unsigned long *val)
{
    int (*next)(const void *, unsigned long *) =
        dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_buffer_size");
    int err = next(params, val);

    if (err == 0 && *val)
        g_buffer = *val;
    return err;
}

int snd_pcm_poll_descriptors_count(void *pcm)
{
    (void)pcm;
    /* Null sinks are always writable; let the daemon use its timeout. */
    return 0;
}

long snd_pcm_avail_update(void *pcm)
{
    long (*next)(void *) = dlsym(RTLD_NEXT, "snd_pcm_avail_update");
    long real = next(pcm);

    if (pcm != g_top || real < 0)
        return real;
    return free_frames();
}

int snd_pcm_delay(void *pcm, long *delay)
{
    int (*next)(void *, long *) = dlsym(RTLD_NEXT, "snd_pcm_delay");
    int err = next(pcm, delay);

    if (pcm == g_top && err == 0)
        *delay = (long)queued();
    return err;
}

long snd_pcm_writei(void *pcm, const void *buffer, unsigned long frames)
{
    long (*next)(void *, const void *, unsigned long) =
        dlsym(RTLD_NEXT, "snd_pcm_writei");
    const char *partial = getenv("KEEPALIVE_TEST_PARTIAL_WRITES");
    unsigned long requested = frames;
    long space;
    long written;

    g_top = pcm;
    space = free_frames();
    if (space <= 0)
        return -EAGAIN;
    if (requested > (unsigned long)space)
        requested = (unsigned long)space;
    if (partial && partial[0] == '1' && requested > 1)
        requested /= 2;

    written = next(pcm, buffer, requested);
    if (written > 0) {
        if (!g_clock_running) {
            g_clock_start_ns = now_ns();
            g_clock_running = 1;
        }
        g_written += (uint64_t)written;
    }
    return written;
}

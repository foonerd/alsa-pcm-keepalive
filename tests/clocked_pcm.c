#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

static unsigned int g_rate = 48000;

int snd_pcm_hw_params_set_rate(void *pcm, void *params, unsigned int rate,
                               int direction)
{
    int (*next)(void *, void *, unsigned int, int) =
        dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_rate");
    g_rate = rate;
    return next(pcm, params, rate, direction);
}

long snd_pcm_writei(void *pcm, const void *buffer, unsigned long frames)
{
    long (*next)(void *, const void *, unsigned long) =
        dlsym(RTLD_NEXT, "snd_pcm_writei");
    const char *partial = getenv("KEEPALIVE_TEST_PARTIAL_WRITES");
    unsigned long requested = frames;
    long written;

    if (partial && partial[0] == '1' && frames > 1)
        requested = frames / 2;

    written = next(pcm, buffer, requested);
    if (written > 0) {
        long long ns = (long long)written * 1000000000LL / g_rate;
        struct timespec delay = {ns / 1000000000LL, ns % 1000000000LL};

        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
        }
    }
    return written;
}

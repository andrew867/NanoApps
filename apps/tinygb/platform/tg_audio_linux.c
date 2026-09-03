/*
 * tg_audio_linux.c — tinyalsa, blocking, as the clock. See tg_audio.h.
 */

#include "tg_audio.h"
#include "../core/tg_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tinyalsa/asoundlib.h>

/* Card 0, device 0 - "nano7gaudio", playback cs42l81-hifi-0. */
#define TG_CARD   0
#define TG_DEVICE 0

/*
 * 1024 frames a period, eight of them.
 *
 * Both numbers are Entrain's, arrived at on this hardware: the codec takes
 * about 60 ms to settle a rate change inside its play-start, so a four-period
 * buffer leaves under half a period of slack and one late wake-up becomes an
 * underrun. Eight periods at 48 kHz is 170 ms, which is enough that a stall
 * has to be real to be heard. Four is still tried if the driver refuses eight,
 * because a short buffer beats silence.
 */
#define TG_PERIOD_FRAMES 1024
#define TG_PERIOD_COUNT  8

struct tg_audio {
    struct pcm *pcm;
    unsigned    rate;
    unsigned    periods;
    bool        silent;
    unsigned long restarts;

    /* Null sink only: when the next block is notionally due. */
    struct timespec next;
    bool            timed;
};

/*
 * Rates to try, best first.
 *
 * 48000 leads because the codec's 12 MHz master clock divides into the 48 kHz
 * family and not into 44.1: this device will ACCEPT 44100 through hw_params
 * and then never actually clock it, which presents as a stream stuck in XRUN
 * with hw_ptr pinned at zero. That cost most of a day once. The rest are here
 * so a differently configured card still makes a noise.
 */
static const unsigned k_rates[] = { 48000, 96000, 32000, 24000, 16000, 8000 };

static int try_open(tg_audio *a, unsigned rate)
{
    static const unsigned counts[] = { TG_PERIOD_COUNT, 4 };

    for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        struct pcm_config cfg;
        struct pcm *p;

        if (i && counts[i] == counts[i - 1]) continue;

        memset(&cfg, 0, sizeof cfg);
        cfg.channels     = 2;
        cfg.rate         = rate;
        cfg.period_size  = TG_PERIOD_FRAMES;
        cfg.period_count = counts[i];
        cfg.format       = PCM_FORMAT_S16_LE;
        /* Zero is tinyalsa's default - start at half the buffer, stop at the
           whole of it - which is what we want, and stating it by hand only
           risks disagreeing with the buffer actually granted. */
        cfg.start_threshold   = 0;
        cfg.stop_threshold    = 0;
        cfg.silence_threshold = 0;

        /* Blocking, which is the entire point: the write is the pacer. */
        p = pcm_open(TG_CARD, TG_DEVICE, PCM_OUT, &cfg);
        if (p && pcm_is_ready(p)) {
            a->pcm = p;
            a->rate = rate;
            a->periods = counts[i];
            return 1;
        }

        /* Say what the driver actually objected to. "No audio device would
           open" is not a diagnosis - the card, the rate and the buffer shape
           are three different failures and they need three different fixes. */
        fprintf(stderr, "tinygb: hw:%d,%d %u Hz %u x %u frames: %s\n",
                TG_CARD, TG_DEVICE, rate, counts[i], TG_PERIOD_FRAMES,
                p ? pcm_get_error(p) : "pcm_open returned nothing");
        if (p) pcm_close(p);
    }
    return 0;
}

tg_audio *tg_audio_open(unsigned want, bool silent)
{
    tg_audio *a = calloc(1, sizeof *a);

    if (!a) return NULL;

    if (silent) {
        a->silent = true;
        a->rate = want ? want : 48000;
        return a;
    }

    if (want && try_open(a, want)) goto opened;

    for (unsigned i = 0; i < sizeof k_rates / sizeof k_rates[0]; i++) {
        if (k_rates[i] == want) continue;      /* already tried */
        if (try_open(a, k_rates[i])) {
            fprintf(stderr, "tinygb: wanted %u Hz, got %u\n", want, a->rate);
            goto opened;
        }
    }

    /*
     * Nothing opened. A null sink rather than a failure: the picture is worth
     * having on a device whose codec is being worked on, and the pacing stays
     * identical so what is on screen is still honest.
     */
    fprintf(stderr, "tinygb: no audio device would open; running silent\n");
    a->silent = true;
    a->rate = want ? want : 48000;
    return a;

opened:
    return a;
}

void tg_audio_close(tg_audio *a)
{
    if (!a) return;
    if (a->pcm) {
        /* Let what has been queued actually play. Closing outright discards
           it, which clips the last fifth of a second off every session. */
        pcm_stop(a->pcm);
        pcm_close(a->pcm);
    }
    free(a);
}

unsigned tg_audio_rate(const tg_audio *a)      { return a ? a->rate : 0; }
bool     tg_audio_is_silent(const tg_audio *a) { return !a || a->silent; }
unsigned long tg_audio_restarts(const tg_audio *a) { return a ? a->restarts : 0; }

/* The null sink still has to take the right amount of time, or the emulator
   free-runs and the picture is wrong in a way that looks like a fast game. */
static void sleep_for(tg_audio *a, unsigned frames)
{
    long long ns = (long long)frames * 1000000000LL / (a->rate ? a->rate : 48000);

    if (!a->timed) {
        clock_gettime(CLOCK_MONOTONIC, &a->next);
        a->timed = true;
    }

    a->next.tv_nsec += (long)(ns % 1000000000LL);
    a->next.tv_sec  += (time_t)(ns / 1000000000LL);
    if (a->next.tv_nsec >= 1000000000L) {
        a->next.tv_nsec -= 1000000000L;
        a->next.tv_sec++;
    }

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &a->next, NULL) == EINTR)
        ;
}

bool tg_audio_write(tg_audio *a, const int16_t *pcm, unsigned frames)
{
    if (!a) return false;

    if (a->silent) {
        sleep_for(a, frames);
        return true;
    }

    while (frames) {
        int rc = pcm_writei(a->pcm, (void *)pcm, frames);

        if (rc > 0) {
            /* Short writes happen and are not errors. Advancing by what was
               actually taken is the difference between a gap and a glitch. */
            pcm = pcm + (unsigned)rc * 2;
            frames -= (unsigned)rc;
            continue;
        }

        if (rc == 0) continue;

        /*
         * errno, not rc.
         *
         * tinyalsa's pcm_writei returns -1 and sets errno; it does not return
         * -EAGAIN. Testing the return value against -EAGAIN therefore treats
         * every recoverable stall as fatal, tears the stream down and prepares
         * it again - which is a restart loop at period rate that sounds like a
         * broken toy. Exactly this bug was in Entrain's backend.
         */
        {
            int e = errno;

            if (e == EAGAIN || e == EINTR) continue;

            a->restarts++;
            if (pcm_prepare(a->pcm) < 0) {
                fprintf(stderr, "tinygb: audio stream lost (%s)\n", strerror(e));
                return false;
            }
        }
    }
    return true;
}

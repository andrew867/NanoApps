/*
 * tg_audio_alsalib.c — the same sink, through alsa-lib. See tg_audio.h.
 *
 * There are two backends for one interface, and the reason is specific rather
 * than a matter of taste.
 *
 * tinyalsa is five files with no dependencies, which is exactly why the first
 * one used it. But on this device its view of the stream is wrong: the status
 * page it mmaps at open never changes, so snd state reads SETUP forever,
 * pcm_writei calls pcm_prepare before every transfer, and /proc shows the
 * application pointer resetting to a single frame's worth over and over with
 * hw_ptr pinned at zero. The driver reports the opposite - periods played,
 * zero underruns, the codec resolved at 48000 - and mpg123 and ffplay, both
 * of which go through alsa-lib, play correctly here.
 *
 * So whatever the disagreement is, alsa-lib is on the right side of it.
 *
 * The cost is a megabyte of library and a config tree that must exist at
 * /usr/share/alsa on the device, because alsa-lib bakes that path in at build
 * time and cannot resolve even "hw:0,0" without it. mk-initramfs.sh installs
 * it. tinyalsa remains selectable for a device where it works.
 */

#include "tg_audio.h"
#include "../core/tg_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>

/* Card 0, device 0 - "nano7gaudio", playback cs42l81-hifi-0. */
#define TG_PCM_HW "hw:0,0"

/*
 * Somewhere else, when asked.
 *
 * TINYGB_ALSA_DEVICE takes an alsa-lib device name and is tried before
 * anything below. The reason it exists is snd-aloop: writing "hw:1,0" puts the
 * emulator where tinybtd's SBC encoder reads it from hw:1,1, and the Game Boy
 * comes out of a pair of Bluetooth headphones. Radio+, TinyPod and fbdoom all
 * already had a way to say this and this was the one that did not.
 *
 * A name rather than a card and device number, unlike TinyPod's pair of
 * integers, because this backend is alsa-lib and its whole advantage is that
 * it understands names - "plug:hw:1,0" is a perfectly reasonable thing to want
 * here and cannot be spelled as two integers.
 *
 * No fallback when it is set. An override that silently plays somewhere else
 * because the place you asked for was busy is worse than a failure: the
 * loopback's commonest fault is exactly that, and falling back to the
 * headphones would look like Bluetooth working badly rather than not at all.
 */
#define TG_PCM_ENV "TINYGB_ALSA_DEVICE"

struct tg_audio {
    snd_pcm_t *pcm;
    unsigned   rate;
    bool       silent;
    unsigned long restarts;

    /* Null sink only: when the next block is notionally due. */
    struct timespec next;
    bool            timed;
};

/*
 * Rates to try, best first.
 *
 * 48000 leads because the codec's 12 MHz master clock divides into the 48 kHz
 * family and not into 44.1: this device will ACCEPT 44100 and then never
 * actually clock it, which presents as a stream stuck with its hardware
 * pointer at zero. That cost most of a day once.
 */
static const unsigned k_rates[] = { 48000, 96000, 32000, 24000, 16000, 8000 };

/*
 * How much to keep queued.
 *
 * The driver pins its buffer at 65536 bytes, which at 48 kHz is 341 ms, so
 * asking for less than that changes nothing - alsa-lib will land on what the
 * hardware allows. Asking is still worth doing: on a device that permits a
 * smaller buffer this is the latency between a button and the sound it causes,
 * and 200 ms is about the most a game can carry without feeling detached.
 */
#define TG_LATENCY_US 200000

static int try_open(tg_audio *a, unsigned rate)
{
    /*
     * hw first, then plug.
     *
     * "hw" is the device itself: no conversion, no resampling, and the frames
     * handed over are the frames clocked out. That is what we want, because
     * the APU is compiled to generate at exactly this rate and anything in the
     * middle is work the device does not need to do. "plug" is the fallback
     * for a card that will not take S16 stereo directly.
     */
    const char *names[3] = { TG_PCM_HW, "plug:" TG_PCM_HW, "default" };
    unsigned n_names = 3;

    /* Asked for by name: that one, and only that one. */
    {
        const char *env = getenv(TG_PCM_ENV);
        if (env && *env) {
            names[0] = env;
            n_names = 1;
        }
    }

    for (unsigned i = 0; i < n_names; i++) {
        snd_pcm_t *h = NULL;
        int rc;

        if ((rc = snd_pcm_open(&h, names[i], SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
            fprintf(stderr, "tinygb: %s: %s\n", names[i], snd_strerror(rc));
            continue;
        }

        /*
         * set_params rather than hw_params and sw_params by hand.
         *
         * It picks a period and buffer inside what the device allows and sets
         * the start and stop thresholds to match - which is precisely the part
         * that had to be reverse-engineered per-device with tinyalsa, and got
         * it wrong. The 0 is "do not resample": if this rate is not available
         * the call fails and the ladder moves on, rather than silently
         * inserting a converter and playing at the wrong pitch.
         */
        rc = snd_pcm_set_params(h, SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                2, rate, 0, TG_LATENCY_US);
        if (rc < 0) {
            fprintf(stderr, "tinygb: %s at %u Hz: %s\n",
                    names[i], rate, snd_strerror(rc));
            snd_pcm_close(h);
            continue;
        }

        a->pcm = h;
        a->rate = rate;

        {
            snd_pcm_uframes_t period = 0, buffer = 0;

            if (snd_pcm_get_params(h, &buffer, &period) == 0)
                printf("tinygb: audio %s %u Hz, period %lu, buffer %lu"
                       " = %lu ms\n", names[i], rate,
                       (unsigned long)period, (unsigned long)buffer,
                       rate ? (unsigned long)buffer * 1000u / rate : 0);
        }
        return 1;
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

    if (want && try_open(a, want)) return a;

    for (unsigned i = 0; i < sizeof k_rates / sizeof k_rates[0]; i++) {
        if (k_rates[i] == want) continue;       /* already tried */
        if (try_open(a, k_rates[i])) {
            fprintf(stderr, "tinygb: wanted %u Hz, got %u - the APU is built "
                            "for %u and will be off pitch\n",
                    want, a->rate, want);
            return a;
        }
    }

    /* A null sink rather than a failure: the picture is worth having, and the
       pacing stays identical so what is on screen is still honest. */
    fprintf(stderr, "tinygb: no audio device would open; running silent\n");
    a->silent = true;
    a->rate = want ? want : 48000;
    return a;
}

void tg_audio_close(tg_audio *a)
{
    if (!a) return;
    if (a->pcm) {
        /* Let what is queued actually play. Closing outright discards it,
           which clips the last fifth of a second off every session. */
        snd_pcm_drain(a->pcm);
        snd_pcm_close(a->pcm);
    }
    /* alsa-lib caches its parsed configuration; a process that is about to
       exit does not care, but a leak checker does and so does anything that
       opens and closes the sink repeatedly. */
    snd_config_update_free_global();
    free(a);
}

unsigned tg_audio_rate(const tg_audio *a)          { return a ? a->rate : 0; }
bool     tg_audio_is_silent(const tg_audio *a)     { return !a || a->silent; }
unsigned long tg_audio_restarts(const tg_audio *a) { return a ? a->restarts : 0; }

void tg_audio_probe(void)
{
    snd_pcm_t *h = NULL;
    snd_pcm_hw_params_t *hw;
    unsigned min = 0, max = 0;
    int dir = 0, rc;

    if ((rc = snd_pcm_open(&h, TG_PCM_HW, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "tinygb: %s: %s\n", TG_PCM_HW, snd_strerror(rc));
        return;
    }

    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(h, hw) < 0) {
        fprintf(stderr, "tinygb: %s will not describe itself\n", TG_PCM_HW);
        snd_pcm_close(h);
        return;
    }

    printf("%s playback limits\n", TG_PCM_HW);
    snd_pcm_hw_params_get_rate_min(hw, &min, &dir);
    snd_pcm_hw_params_get_rate_max(hw, &max, &dir);
    printf("  rate         %u .. %u\n", min, max);
    snd_pcm_hw_params_get_channels_min(hw, &min);
    snd_pcm_hw_params_get_channels_max(hw, &max);
    printf("  channels     %u .. %u\n", min, max);
    {
        snd_pcm_uframes_t f0 = 0, f1 = 0;

        snd_pcm_hw_params_get_period_size_min(hw, &f0, &dir);
        snd_pcm_hw_params_get_period_size_max(hw, &f1, &dir);
        printf("  period size  %lu .. %lu frames\n",
               (unsigned long)f0, (unsigned long)f1);
        snd_pcm_hw_params_get_buffer_size_min(hw, &f0);
        snd_pcm_hw_params_get_buffer_size_max(hw, &f1);
        printf("  buffer size  %lu .. %lu frames\n",
               (unsigned long)f0, (unsigned long)f1);
    }
    snd_pcm_hw_params_get_periods_min(hw, &min, &dir);
    snd_pcm_hw_params_get_periods_max(hw, &max, &dir);
    printf("  periods      %u .. %u\n", min, max);

    printf("  rate 48000   %s\n",
           snd_pcm_hw_params_test_rate(h, hw, 48000, 0) == 0 ? "yes" : "no");

    snd_pcm_close(h);
    snd_config_update_free_global();
}

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
        snd_pcm_sframes_t got = snd_pcm_writei(a->pcm, pcm, frames);

        if (got >= 0) {
            /* Short writes happen and are not errors. Advancing by what was
               actually taken is the difference between a gap and a glitch. */
            pcm += (size_t)got * 2;
            frames -= (unsigned)got;
            continue;
        }

        /*
         * snd_pcm_recover handles the three that are survivable - EPIPE for an
         * underrun, ESTRPIPE for a suspended stream, EINTR - and returns the
         * error untouched for anything else. Unlike tinyalsa it does NOT do
         * this silently inside the write, so an underrun is visible here and
         * gets counted, which is the whole reason the counter can be trusted
         * on this backend and could not be on the other one.
         */
        if (got == -EPIPE) a->restarts++;

        if (snd_pcm_recover(a->pcm, (int)got, 1 /* silent */) < 0) {
            fprintf(stderr, "tinygb: audio stream lost (%s)\n",
                    snd_strerror((int)got));
            return false;
        }
    }
    return true;
}

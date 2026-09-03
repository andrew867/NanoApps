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

/*
 * tinyalsa 2.0.0 exports pcm_state and forgets to declare it. Declaring it
 * here is a matching prototype, so a version that does declare it agrees
 * rather than conflicts. TinyPod's sink does the same, for the same reason:
 * pcm_writei recovers from an underrun by itself - it re-prepares and retries
 * - and returns success, so a stream restarting on every write is
 * indistinguishable from one playing perfectly unless the state is read on the
 * way in.
 *
 * KNOWN UNRELIABLE ON THIS DRIVER, and worth writing down.
 *
 * pcm_state reads the status page tinyalsa mmaps at open. On the N31 i2s
 * driver that page does not appear to be updated: the state reads back
 * unchanged forever, /proc shows hw_ptr pinned at 0 and appl_ptr resetting to
 * a single frame's worth on every write, and tinyalsa - seeing SETUP - calls
 * pcm_prepare before each transfer, which is what resets it.
 *
 * Meanwhile the driver itself reports the opposite: "rearm stop: 59 periods,
 * 0 underruns", and the codec resolves 48000. mpg123 and ffplay, which go
 * through alsa-lib rather than tinyalsa, play correctly on this device. So
 * the restart count below may be entirely phantom, and is reported as a
 * number to compare rather than as a fact about the audio.
 */
int pcm_state(struct pcm *pcm);

/* Card 0, device 0 - "nano7gaudio", playback cs42l81-hifi-0. */
#define TG_CARD   0
#define TG_DEVICE 0

/*
 * The buffer shape is asked for, not chosen.
 *
 * It started as 1024 frames in 8 periods, which is Entrain's shape and sound
 * reasoning on paper. The driver refused it with EINVAL at every rate, and the
 * reason was only visible by asking the device: it reports periods 16..32 and
 * a buffer pinned at exactly 65536 bytes, so the only legal shapes are 1024x16
 * and 512x32. Eight periods could never have worked.
 *
 * Worse, the driver source on the build host said periods_min 4 and
 * period_bytes_max 8188 - the module on the device was newer than the tree.
 * Any constant here is a constant that will be wrong again the next time the
 * audio driver is touched, and it fails as EINVAL with no explanation.
 *
 * So: read the constraints, pick the largest period the device allows (fewest
 * interrupts for a given buffer), and derive the count from the buffer size it
 * insists on. These remain only as the fallback for a device that will not
 * describe itself.
 */
#define TG_PERIOD_FRAMES 1024
#define TG_PERIOD_COUNT  16

struct tg_audio {
    struct pcm *pcm;
    unsigned    rate;
    unsigned    periods;
    unsigned    period_frames;
    bool        running;      /* the stream has actually been started */
    unsigned    queued;       /* frames handed over before it started */
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

/* A shape to try: frames per period, and how many. */
struct shape { unsigned size, count; };

/*
 * What this device will actually take, best first.
 *
 * "Best" is the largest period within the reported range, because the period
 * is how often the driver has to be woken, and the count that fills the buffer
 * the device demands. A second candidate at half the period covers a driver
 * whose maximum period is not a divisor of its buffer.
 */
static unsigned shapes_for_device(struct shape *out, unsigned cap)
{
    /* An override, for finding out on the device which shapes actually run. */
    const char *force = getenv("TINYGB_AUDIO_SHAPE");

    if (force && cap) {
        unsigned sz = 0, ct = 0;

        if (sscanf(force, "%u,%u", &sz, &ct) == 2 && sz && ct) {
            out[0].size = sz;
            out[0].count = ct;
            return 1;
        }
    }

    struct pcm_params *pp = pcm_params_get(TG_CARD, TG_DEVICE, PCM_OUT);
    unsigned n = 0;
    unsigned ps_min, ps_max, pc_min, pc_max, buf, frame_bytes = 2 * 2;

    if (!pp) return 0;

    ps_min = pcm_params_get_min(pp, PCM_PARAM_PERIOD_SIZE);
    ps_max = pcm_params_get_max(pp, PCM_PARAM_PERIOD_SIZE);
    pc_min = pcm_params_get_min(pp, PCM_PARAM_PERIODS);
    pc_max = pcm_params_get_max(pp, PCM_PARAM_PERIODS);
    buf    = pcm_params_get_min(pp, PCM_PARAM_BUFFER_BYTES);
    pcm_params_free(pp);

    if (!ps_max || !pc_max || !buf) return 0;

    for (unsigned size = ps_max; size >= ps_min && n < cap; size /= 2) {
        unsigned count = buf / (size * frame_bytes);

        if (count < pc_min) count = pc_min;
        if (count > pc_max) count = pc_max;
        if (count) {
            out[n].size = size;
            out[n].count = count;
            n++;
        }
        if (size == ps_min || size < 2) break;
    }
    return n;
}

static int try_open(tg_audio *a, unsigned rate)
{
    struct shape shapes[4];
    unsigned n = shapes_for_device(shapes, 4);

    /* A device that will not describe itself still gets one honest attempt. */
    if (!n) {
        shapes[0].size = TG_PERIOD_FRAMES;
        shapes[0].count = TG_PERIOD_COUNT;
        n = 1;
    }

    for (unsigned i = 0; i < n; i++) {
        struct pcm_config cfg;
        struct pcm *p;

        memset(&cfg, 0, sizeof cfg);
        cfg.channels     = 2;
        cfg.rate         = rate;
        cfg.period_size  = shapes[i].size;
        cfg.period_count = shapes[i].count;
        cfg.format       = PCM_FORMAT_S16_LE;
        /*
         * The software parameters, spelled out, and taken from TinyPod's sink
         * because those are the ones this codec is known to run with.
         *
         * avail_min is the one that matters and the one that was missing.
         * Left at zero it is not "the default", it is zero - the device never
         * reports itself writable, so a blocking write does not block and the
         * stream sits in PREPARED with its hardware pointer at zero while
         * every call returns success. That reads exactly like a driver that
         * accepts data and refuses to clock it, and it is not: it is this
         * field.
         *
         * Starting on a full buffer rather than a half-full one is TinyPod's
         * reasoning too - half a buffer is half the time to the first
         * underrun, and the work is heaviest at the start.
         */
        cfg.start_threshold   = shapes[i].size * shapes[i].count;
        /*
         * Never stop on an underrun.
         *
         * stop_threshold is the free space at which the core calls the stream
         * dead. The usual value is the buffer size - "stop when completely
         * empty" - which is right for a media player, where running dry means
         * the file ended. For a game a late frame is just a late frame, and
         * tearing the stream down and rebuilding it is a far worse artefact
         * than the gap that caused it. A threshold the buffer cannot reach
         * turns an underrun into a glitch instead of a restart.
         *
         * (Tried as a fix for the re-prepare described below. It was not one,
         * but it is still the right setting for this application.)
         */
        cfg.stop_threshold    = 0x7FFFFFFFu;
        cfg.silence_threshold = 0;
        cfg.avail_min         = shapes[i].size;

        /* Blocking, which is the entire point: the write is the pacer. */
        p = pcm_open(TG_CARD, TG_DEVICE, PCM_OUT, &cfg);
        if (p && pcm_is_ready(p)) {
            a->pcm = p;
            a->rate = rate;
            a->periods = shapes[i].count;
            a->period_frames = shapes[i].size;
            return 1;
        }

        /* Say what the driver actually objected to. "No audio device would
           open" is not a diagnosis - the card, the rate and the buffer shape
           are three different failures and they need three different fixes. */
/*
         * errno as well as the library's own message. tinyalsa returns a
         * shared static object for early failures and its error buffer can
         * come back empty, which says nothing at all - and "no audio device
         * would open" with no reason attached is how an afternoon goes.
         */
        {
            const char *msg = p ? pcm_get_error(p) : NULL;

            fprintf(stderr,
                    "tinygb: hw:%d,%d %u Hz %u x %u frames rejected: %s (%s)\n",
                    TG_CARD, TG_DEVICE, rate, shapes[i].count, shapes[i].size,
                    (msg && *msg) ? msg : "no reason given", strerror(errno));
        }
        if (p) pcm_close(p);
    }
    return 0;
}

/*
 * What the hardware says it can do, asked of the hardware.
 *
 * The driver publishes its own limits and they are not guesses: this one puts
 * its PCM buffer in SRAM and constrains a period to one DMA descriptor, so the
 * usable shapes are narrower than a desktop card's and a config that looks
 * ordinary can be refused. Printing them beats reading the driver source and
 * hoping the module on the device is the one that source built.
 */
void tg_audio_probe(void)
{
    struct pcm_params *pp = pcm_params_get(TG_CARD, TG_DEVICE, PCM_OUT);

    if (!pp) {
        fprintf(stderr, "tinygb: hw:%d,%d will not describe itself (%s)\n",
                TG_CARD, TG_DEVICE, strerror(errno));
        return;
    }

    printf("hw:%d,%d playback limits\n", TG_CARD, TG_DEVICE);
    printf("  rate         %u .. %u\n",
           pcm_params_get_min(pp, PCM_PARAM_RATE),
           pcm_params_get_max(pp, PCM_PARAM_RATE));
    printf("  channels     %u .. %u\n",
           pcm_params_get_min(pp, PCM_PARAM_CHANNELS),
           pcm_params_get_max(pp, PCM_PARAM_CHANNELS));
    printf("  period bytes %u .. %u\n",
           pcm_params_get_min(pp, PCM_PARAM_PERIOD_BYTES),
           pcm_params_get_max(pp, PCM_PARAM_PERIOD_BYTES));
    printf("  period size  %u .. %u frames\n",
           pcm_params_get_min(pp, PCM_PARAM_PERIOD_SIZE),
           pcm_params_get_max(pp, PCM_PARAM_PERIOD_SIZE));
    printf("  periods      %u .. %u\n",
           pcm_params_get_min(pp, PCM_PARAM_PERIODS),
           pcm_params_get_max(pp, PCM_PARAM_PERIODS));
    printf("  buffer bytes %u .. %u\n",
           pcm_params_get_min(pp, PCM_PARAM_BUFFER_BYTES),
           pcm_params_get_max(pp, PCM_PARAM_BUFFER_BYTES));

    pcm_params_free(pp);
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
    /*
     * The buffer is the latency, and here it is not negotiable: the device
     * pins it at 65536 bytes, which at 48 kHz is 341 ms. That is the delay
     * between a button and the sound it causes, and it is the driver's to
     * change, not ours - so say it rather than let it be a mystery.
     */
    {
        /* What it gave, not what was asked for. A driver that accepts a rate
           and clocks a different one plays everything at the wrong speed. */
        unsigned got = pcm_get_rate(a->pcm);

        if (got && got != a->rate) {
            fprintf(stderr, "tinygb: asked for %u Hz, the device is running "
                            "at %u\n", a->rate, got);
            a->rate = got;
        }
    }
    printf("tinygb: audio %u Hz, %u x %u frames = %u ms buffered\n",
           a->rate, a->periods, a->period_frames,
           a->rate ? (a->periods * a->period_frames * 1000u) / a->rate : 0);
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

    /*
     * Look before writing. Once the stream has started, anything other than
     * RUNNING or DRAINING means the last buffer ran dry and tinyalsa has
     * already quietly put it back together.
     */
    if (a->running) {
        int st = pcm_state(a->pcm);

        /* See the note on pcm_state: on this driver this counts something,
           but not reliably an underrun. */
        if (st != PCM_STATE_RUNNING && st != PCM_STATE_DRAINING) a->restarts++;
    }

    while (frames) {
        int rc = pcm_writei(a->pcm, (void *)pcm, frames);

        /*
         * Notice if it never starts.
         *
         * The stream begins on its own once start_threshold frames are queued,
         * which is one buffer - about twenty writes. Forcing it earlier would
         * start it nearly empty and underrun immediately, so this only
         * watches, and complains once if the threshold has been passed twice
         * over and the device is still not running. Silence with no
         * explanation is the thing worth preventing.
         */
        if (rc > 0 && !a->running) {
            a->queued += (unsigned)rc;
            if (pcm_state(a->pcm) == PCM_STATE_RUNNING) {
                a->running = true;
            } else if (a->queued > 2 * a->periods * a->period_frames) {
                fprintf(stderr, "tinygb: the stream will not start "
                                "(state %d after %u frames queued)\n",
                        (int)pcm_state(a->pcm), a->queued);
                a->running = true;      /* said once, not every frame */
            }
        }

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

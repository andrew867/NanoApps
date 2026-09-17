/*
 * tg_audio.h — where the sound goes, and what keeps time.
 *
 * Small on purpose, because the interesting decision is not the API but the
 * clock, and this interface exists to make that decision explicit:
 *
 *   The sink is the master. tg_audio_write blocks until the device has room,
 *   so calling it once per emulated frame paces the whole emulator off the
 *   codec's crystal - the only clock in this machine that is not approximate.
 *   Video follows audio, which is the right way round: a frame arriving a
 *   millisecond late is invisible, and a sample arriving a millisecond late is
 *   a click.
 *
 * Why this is not Entrain's platform/audio.h, which already streams PCM on
 * this device and which the plan said to share:
 *
 *   Entrain writes non-blocking from its own thread and paces against the
 *   monotonic clock, because its generator is cheap and its UI must never
 *   block. That is the correct design there and the wrong one here - it makes
 *   the monotonic clock the master, and an emulator whose video clock and
 *   audio clock are different clocks drifts apart forever. Entrain also ramps
 *   gain in over a second on every start, which is right for an entrainment
 *   tone and wrong for a game.
 *
 *   Sharing would have meant reshaping a working, tuned module - including a
 *   RetailOS backend built on reverse-engineered descriptor chaining that
 *   nothing here can test - to serve a second caller that wants the opposite
 *   threading model. What is worth carrying across is the knowledge, not the
 *   code: the card, the period shape, the fallback ladder, and the restart
 *   handling are all taken from it, and noted where they are used.
 */

#ifndef TINYGB_AUDIO_H
#define TINYGB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct tg_audio tg_audio;

/*
 * Open the sink at the best rate it will give.
 *
 * `want` is the rate the caller would prefer; the rate actually obtained is
 * reported by tg_audio_rate and may differ, so the caller must ask rather than
 * assume. Returns NULL if no rate worked at all.
 *
 * With `silent` the sink is a null one that still blocks for the right length
 * of time, so the emulator is paced identically and the picture can be checked
 * on a device whose codec is unhappy.
 */
tg_audio *tg_audio_open(unsigned want, bool silent);

void tg_audio_close(tg_audio *a);

unsigned tg_audio_rate(const tg_audio *a);

/* Whether this is a real device or the null sink. */
bool tg_audio_is_silent(const tg_audio *a);

/*
 * Hand over `frames` frames of interleaved 16-bit stereo, and block until the
 * device has taken them.
 *
 * This is the pacer. Returns false only if the stream could not be recovered,
 * which is fatal to playback but not to the app - the caller can carry on
 * silently rather than quit.
 */
bool tg_audio_write(tg_audio *a, const int16_t *pcm, unsigned frames);

/*
 * How many audio frames one emulated video frame is worth, without drift.
 *
 * The naive answer is rate / 59.7275 truncated, which at 48000 is 803 - and
 * 803 a frame is 47961 Hz, so the sink is starved by 39 frames every second
 * and underruns about every four seconds. The real figure is 803.6499..., so
 * this keeps the remainder and returns 804 whenever it has accumulated one.
 *
 * Exact rational arithmetic on integers: no float, no drift, ever.
 */
typedef struct {
    unsigned rate;
    uint64_t acc;
} tg_audio_clock;

void     tg_audio_clock_init(tg_audio_clock *c, unsigned rate);
unsigned tg_audio_clock_next(tg_audio_clock *c);

/* Print what the playback device says it can do, and return. For finding out
   on a real device why a config that looks reasonable was refused. */
void tg_audio_probe(void);

/* How many underruns the device has reported, for the log. */
unsigned long tg_audio_restarts(const tg_audio *a);

#endif /* TINYGB_AUDIO_H */

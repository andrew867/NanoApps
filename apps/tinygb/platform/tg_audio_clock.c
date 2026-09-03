/*
 * tg_audio_clock.c — how many audio frames one video frame is worth.
 *
 * Separate from any sink because it is pure integer arithmetic with no device
 * in it: the Linux backend needs it, RetailOS's will need it at 22050, and the
 * host tests need it without tinyalsa anywhere near them.
 *
 * The naive answer is rate / 59.7275 truncated, which at 48 kHz is 803. That
 * is not a rounding question - 803 frames per video frame is 47961 Hz, so the
 * sink is starved by 39 frames every second, an eight-period buffer drains in
 * about four seconds, and the result is audio that clicks every four seconds
 * for as long as you leave it running. The remainder has to be carried.
 */

#include "tg_audio.h"
#include "../core/tg_core.h"

void tg_audio_clock_init(tg_audio_clock *c, unsigned rate)
{
    c->rate = rate;
    c->acc = 0;
}

unsigned tg_audio_clock_next(tg_audio_clock *c)
{
    unsigned n;

    /*
     * frames = rate * TG_FPS_DEN / TG_FPS_NUM, keeping what is left over.
     *
     * At 48000 that is 48000 * 70224 / 4194304 = 803.6499..., so this hands
     * out 804 about two frames in three and 803 the rest, and the running
     * average is exact for as long as the machine is on.
     *
     * 64-bit because 48000 * 70224 is 3.4 billion and would wrap a 32-bit
     * accumulator on the very first frame.
     */
    c->acc += (uint64_t)c->rate * TG_FPS_DEN;
    n = (unsigned)(c->acc / TG_FPS_NUM);
    c->acc -= (uint64_t)n * TG_FPS_NUM;
    return n;
}

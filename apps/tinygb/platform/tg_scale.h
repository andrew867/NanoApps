/*
 * tg_scale.h — 160x144 onto 240x216, which is exactly one and a half.
 *
 * The nano 7's panel is 240x432. A Game Boy is 160x144. 160 * 1.5 is 240 and
 * 144 * 1.5 is 216, so the picture fills the width exactly and takes precisely
 * half the height, leaving the other half for the touch controls. No other
 * scale on this screen is exact in both axes, which is why this one is the
 * default rather than a compromise.
 *
 * One and a half is two source pixels becoming three, and the interesting
 * question is which three. Nearest neighbour gives (a, a, b): every second
 * source pixel is doubled and its neighbour is not, so a one-pixel line is
 * sometimes one pixel wide and sometimes two depending only on where it sits.
 * On Game Boy art, which is nearly all one-pixel detail, that is very visible
 * and it shimmers when anything scrolls.
 *
 * Taking the middle sample instead gives (a, mix, b), which is symmetric: every
 * source pixel appears once at full strength, and the pixel between them is the
 * average. Two shades average to a third value, so a four-shade image becomes a
 * seven-value one - still a tiny palette, and far steadier.
 *
 * Both are here. Smooth is the default; sharp is one field away for anyone who
 * wants the hard edges.
 *
 * This file is deliberately free of Linux, RetailOS, LVGL and the framebuffer:
 * it turns shade indices into pixels in a caller-provided buffer, so the same
 * code runs on the device under both operating systems and gets tested on a
 * desktop.
 */

#ifndef TINYGB_SCALE_H
#define TINYGB_SCALE_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/tg_core.h"

/* What TG_W x TG_H becomes. Both exact: 160*3/2 and 144*3/2. */
#define TG_SCALED_W ((TG_W * 3) / 2)   /* 240 */
#define TG_SCALED_H ((TG_H * 3) / 2)   /* 216 */

/*
 * The palette, pre-chewed. Building the sixteen possible blends once per
 * palette change turns the inner loop into two table lookups and a copy, which
 * matters: this runs 51840 times a frame, sixty times a second, on a
 * single-issue Cortex-A8 with no NEON.
 */
typedef struct {
    uint32_t solid[4];    /* the four shades, as 0x00RRGGBB */
    uint32_t pair[16];    /* pair[a * 4 + b] - what goes between shade a and b */

    /*
     * The last frame, so rows that did not change can skip the write.
     *
     * The framebuffer is uncached: measured on the device, putting 207 KB into
     * it costs 3.85 ms of a 16.74 ms frame, which is about 50 MB/s and is what
     * display memory costs everywhere. Comparing 320 bytes of cached source
     * per row pair is nearly free next to writing 2880 bytes of uncached
     * destination, and a Game Boy frame is usually mostly the previous one -
     * a static playfield, an open menu, a text box.
     *
     * 23 KB, which is a bargain for what it buys.
     */
    uint8_t prev[TG_W * TG_H];
    bool    have_prev;
} tg_scaler;

/* `pal` is four entries of 0x00RRGGBB, lightest first. When `smooth` is false
   the middle sample repeats the left pixel instead of averaging, which is
   nearest-neighbour and looks it. */
void tg_scaler_init(tg_scaler *s, const uint32_t pal[4], bool smooth);

/*
 * Scale one frame.
 *
 * `src` is TG_W*TG_H bytes of core output; only the shade bits are read, so a
 * core that also reports which palette a pixel came from does not need to mask
 * first. `dst` points at the top-left of where the picture goes, and
 * `dst_stride_px` is the destination's row pitch in PIXELS, not bytes - a
 * framebuffer's line length is in bytes and dividing it by four is the caller's
 * job, because only the caller knows the pixel size.
 *
 * Writes exactly TG_SCALED_H rows of TG_SCALED_W pixels and touches nothing
 * else.
 */
void tg_scale_15(tg_scaler *s, uint32_t *dst, unsigned dst_stride_px,
                 const uint8_t *src);

/*
 * Forget the last frame, so the next call writes every row.
 *
 * Required whenever anything else has drawn over the picture - a menu, a
 * message, a mode change - because the skip assumes the destination still
 * holds what this scaler last put there. Also called by tg_scaler_init, so a
 * palette change repaints on its own.
 */
void tg_scaler_invalidate(tg_scaler *s);

#endif /* TINYGB_SCALE_H */

/*
 * tg_scale.c — see tg_scale.h.
 */

#include "tg_scale.h"

#include <string.h>

/*
 * Average two packed pixels without unpacking them.
 *
 * The obvious form - shift both down and add - is wrong in a way that only
 * shows on flat colour: it truncates each channel before adding, so averaging
 * a pixel with ITSELF loses the low bit and comes back darker. Shade 2 of the
 * default palette is 0x306230, and that form returns 0x306230 for the solid
 * pixels and 0x306230 minus a level for every blended one, which is a faint
 * grid over every flat area of the picture.
 *
 * This form is exact when the two are equal, which is the common case here -
 * most of a Game Boy frame is flat - and still correct when they differ:
 * the shared bits pass through untouched, and only the bits that differ get
 * halved. Masking before the shift keeps a channel's low bit out of its
 * neighbour's top bit. Same three operations as the wrong version.
 */
static inline uint32_t mix(uint32_t x, uint32_t y)
{
    return (x & y) + (((x ^ y) & 0xFEFEFEu) >> 1);
}

void tg_scaler_invalidate(tg_scaler *s)
{
    s->have_prev = false;
}

void tg_scaler_init(tg_scaler *s, const uint32_t pal[4], bool smooth)
{
    tg_scaler_invalidate(s);

    for (unsigned i = 0; i < 4; i++)
        s->solid[i] = pal[i] & 0x00FFFFFFu;

    for (unsigned a = 0; a < 4; a++)
        for (unsigned b = 0; b < 4; b++)
            s->pair[a * 4 + b] = smooth ? mix(s->solid[a], s->solid[b])
                                        : s->solid[a];
}

/* One source row of 160 becomes one destination row of 240. */
static void expand_row(const tg_scaler *s, const uint8_t *src, uint32_t *out)
{
    for (unsigned k = 0; k < TG_W / 2; k++) {
        unsigned a = src[k * 2]     & TG_PX_SHADE;
        unsigned b = src[k * 2 + 1] & TG_PX_SHADE;

        out[k * 3]     = s->solid[a];
        out[k * 3 + 1] = s->pair[a * 4 + b];
        out[k * 3 + 2] = s->solid[b];
    }
}

void tg_scale_15(tg_scaler *s, uint32_t *dst, unsigned dst_stride_px,
                 const uint8_t *src)
{
    /*
     * Two expanded rows at a time. Vertically it is the same two-becomes-three
     * as horizontally, so a pair of source rows produces the top row, their
     * average, and the bottom row - and expanding each source row once rather
     * than per output row is what keeps this to one pass over the source.
     *
     * 1920 bytes of stack. The alternative, expanding into a full 240x216
     * scratch and then copying, would want 207 KiB on a device with 55 MiB and
     * an initramfs living in it.
     */
    uint32_t top[TG_SCALED_W], bot[TG_SCALED_W];

    for (unsigned p = 0; p < TG_H / 2; p++) {
        const uint8_t *sp = src + (size_t)(p * 2) * TG_W;
        uint32_t *d0 = dst + (size_t)(p * 3) * dst_stride_px;
        uint32_t *d1 = d0 + dst_stride_px;
        uint32_t *d2 = d1 + dst_stride_px;

        /* Both source rows identical to last time means all three destination
           rows already hold the right pixels. 320 bytes of cached compare
           against 2880 bytes of uncached write. */
        if (s->have_prev &&
            memcmp(sp, s->prev + (size_t)(p * 2) * TG_W, TG_W * 2) == 0)
            continue;

        expand_row(s, src + (size_t)(p * 2)     * TG_W, top);
        expand_row(s, src + (size_t)(p * 2 + 1) * TG_W, bot);

        memcpy(d0, top, sizeof top);
        for (unsigned x = 0; x < TG_SCALED_W; x++)
            d1[x] = mix(top[x], bot[x]);
        memcpy(d2, bot, sizeof bot);
    }

    /* One copy of the whole frame rather than a copy per row: it is 23 KB of
       cached memory either way, and doing it here keeps the comparison above
       reading the OLD frame for the whole pass. */
    memcpy(s->prev, src, sizeof s->prev);
    s->have_prev = true;
}

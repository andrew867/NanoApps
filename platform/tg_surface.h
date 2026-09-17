/*
 * tg_surface.h — a rectangle of 32-bit pixels, and nothing about where it is.
 *
 * Both device targets hand the front end a block of XRGB8888 with a row
 * pitch: N31 maps the framebuffer or a DRM dumb buffer, RetailOS hands over
 * the compositor's surface. The pad, the text and the menus draw into one
 * of these and never learn which, and the host tests draw into an array.
 *
 * `or_mask` is what the compositor wants in the byte nobody displays. RetailOS
 * blits the surface with alpha and wants 0xFF there; N31's XRGB ignores it.
 * Applying it here, once per write, is cheaper than remembering it at every
 * call site and cheaper to get right.
 */

#ifndef TINYGB_SURFACE_H
#define TINYGB_SURFACE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t *px;
    unsigned  w, h;
    unsigned  stride_px;   /* row pitch in PIXELS, which is not always w */
    uint32_t  or_mask;
} tg_surface;

/* Clipped to the surface; a rectangle hanging off any edge is fine. */
static inline void tg_surface_rect(const tg_surface *s, int x, int y,
                                   int w, int h, uint32_t c)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)s->w ? (int)s->w : x + w;
    int y1 = y + h > (int)s->h ? (int)s->h : y + h;

    if (!s->px || x0 >= x1 || y0 >= y1) return;
    c |= s->or_mask;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = s->px + (size_t)yy * s->stride_px;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static inline void tg_surface_px(const tg_surface *s, int x, int y, uint32_t c)
{
    if (!s->px || x < 0 || y < 0 || x >= (int)s->w || y >= (int)s->h) return;
    s->px[(size_t)y * s->stride_px + x] = c | s->or_mask;
}

#endif /* TINYGB_SURFACE_H */

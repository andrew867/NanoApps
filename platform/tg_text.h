/*
 * tg_text.h — letters, for a surface that has none of its own.
 *
 * The RetailOS raw surface comes with rectangles and discs and no way to draw
 * a word: the SDK's text routines write to the LCD over the compositor, not
 * into the surface. So TinyGB carries a bitmap font - two, one for lists and
 * one for small print - rendered from DejaVu Sans Mono by tools/gen-font.py
 * and committed as C tables, so a build needs nothing but this file.
 *
 * Fixed cells, one bit per pixel, bit 0 the leftmost. No kerning, no
 * proportional widths: a monospace cell makes "how wide is this string" a
 * multiplication, which is what a list that has to truncate names wants.
 */

#ifndef TINYGB_TEXT_H
#define TINYGB_TEXT_H

#include <stdint.h>

#include "tg_surface.h"

typedef struct {
    uint8_t  w, h;         /* the cell */
    uint8_t  first, count; /* ASCII range covered: 32..126 */
    const uint16_t *rows;  /* count * h rows, bit 0 = leftmost pixel */
} tg_font;

extern const tg_font tg_font_ui;      /* 9x18, for rows a thumb picks from */
extern const tg_font tg_font_small;   /* 7x13, for notes under them */

/* Width in pixels of `s` at `scale` (1 or 2). */
unsigned tg_text_width(const tg_font *f, const char *s, unsigned scale);

/* How many characters of `s` fit in `max_px` at `scale`. */
unsigned tg_text_fit(const tg_font *f, const char *s, unsigned max_px,
                     unsigned scale);

/*
 * Draw `s` with its cell's top-left at (x, y). Only the set bits are
 * written, so text sits on whatever is already there; clear the row first
 * for a background. Clipped at the surface's edges. Characters outside the
 * font's range draw as a space.
 */
void tg_text_draw(const tg_surface *dst, const tg_font *f, int x, int y,
                  const char *s, unsigned scale, uint32_t fg);

#endif /* TINYGB_TEXT_H */

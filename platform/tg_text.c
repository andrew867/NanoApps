/*
 * tg_text.c — see tg_text.h.
 */

#include "tg_text.h"

static const uint16_t *glyph_rows(const tg_font *f, char c)
{
    unsigned i = (unsigned char)c;

    if (i < f->first || i >= (unsigned)f->first + f->count) i = ' ';
    return f->rows + (size_t)(i - f->first) * f->h;
}

unsigned tg_text_width(const tg_font *f, const char *s, unsigned scale)
{
    unsigned n = 0;

    if (!s || !scale) return 0;
    while (*s++) n++;
    return n * f->w * scale;
}

unsigned tg_text_fit(const tg_font *f, const char *s, unsigned max_px,
                     unsigned scale)
{
    unsigned n = 0, cell = f->w * scale;

    if (!s || !cell) return 0;
    while (s[n] && (n + 1) * cell <= max_px) n++;
    return n;
}

void tg_text_draw(const tg_surface *dst, const tg_font *f, int x, int y,
                  const char *s, unsigned scale, uint32_t fg)
{
    if (!dst || !dst->px || !s || !scale) return;

    for (; *s; s++, x += (int)(f->w * scale)) {
        const uint16_t *g = glyph_rows(f, *s);

        /* Entirely off-screen glyphs cost nothing; a partly visible one is
           clipped per pixel below. */
        if (x >= (int)dst->w) break;
        if (x + (int)(f->w * scale) <= 0) continue;

        for (unsigned r = 0; r < f->h; r++) {
            uint16_t bits = g[r];

            if (!bits) continue;
            for (unsigned c = 0; c < f->w; c++) {
                if (!(bits & (1u << c))) continue;
                if (scale == 1)
                    tg_surface_px(dst, x + (int)c, y + (int)r, fg);
                else
                    tg_surface_rect(dst, x + (int)(c * scale),
                                    y + (int)(r * scale),
                                    (int)scale, (int)scale, fg);
            }
        }
    }
}

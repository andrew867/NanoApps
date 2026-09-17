/*
 * tg_pad.c — see tg_pad.h.
 */

#include "tg_pad.h"

#include "tg_scale.h"
#include "../core/tg_core.h"

/* ---- the geometry --------------------------------------------------------
 *
 * Everything below is in panel pixels on a 240x432 screen, with the game
 * occupying the top TG_SCALED_H of it. Written out rather than computed from
 * ratios: these were chosen by where a thumb falls, not by arithmetic, and a
 * formula would suggest they can be re-derived.
 */

#define PAD_TOP     TG_SCALED_H          /* 216: where the picture ends */

/* The d-pad, as a three-by-three of cells. A corner cell is two directions. */
#define DPAD_X      6
#define DPAD_Y      248
#define DPAD_CELL   40
#define DPAD_SPAN   (DPAD_CELL * 3)

/* A and B, diagonal and the same size, the way they are on the machine this
   is pretending to be. B sits low and left of A. */
#define BTN_R       26
#define A_CX        208
#define A_CY        274
#define B_CX        168
#define B_CY        326

/* Start and Select, as pills along the bottom. */
#define PILL_W      72
#define PILL_H      26
#define PILL_Y      386
#define SELECT_X    36
#define START_X     132

/* ---- colour --------------------------------------------------------------
 *
 * The same palette the rest of the apps use, so the pad does not look like it
 * came from somewhere else. Pressed is a lift rather than a hue: a colour
 * change reads as a different button, where a lighter one reads as the same
 * button being held.
 */
#define C_PAD_BG    0x000000u
#define C_FACE      0x171B25u
#define C_FACE_DOWN 0x3A4150u
#define C_EDGE      0x232937u
#define C_GLYPH     0x8B92A0u

static unsigned s_drawn = ~0u;           /* the mask last painted */

/* ---- drawing primitives --------------------------------------------------- */

static void rect(tg_fb *fb, int x, int y, int w, int h, uint32_t c)
{
    int yy;

    if (!fb || !fb->pixels) return;

    for (yy = y; yy < y + h; yy++) {
        uint32_t *row;
        int xx;

        if (yy < 0 || yy >= (int)fb->h) continue;
        row = fb->pixels + (size_t)yy * fb->stride_px;

        for (xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)fb->w) continue;
            row[xx] = c;
        }
    }
}

/* A filled disc, by the squared distance so there is no arithmetic on
   fractions anywhere in this file. */
static void disc(tg_fb *fb, int cx, int cy, int r, uint32_t c)
{
    int y;

    if (!fb || !fb->pixels) return;

    for (y = -r; y <= r; y++) {
        int py = cy + y;
        uint32_t *row;
        int x;

        if (py < 0 || py >= (int)fb->h) continue;
        row = fb->pixels + (size_t)py * fb->stride_px;

        for (x = -r; x <= r; x++) {
            int px = cx + x;

            if (px < 0 || px >= (int)fb->w) continue;
            if (x * x + y * y <= r * r) row[px] = c;
        }
    }
}

/*
 * Two glyphs, five by seven, one bit per pixel with the low bit on the left.
 *
 * Only A and B. The d-pad's shape says what it is to anybody who has held a
 * game console, and the two pills at the bottom are in the place Start and
 * Select have been since 1989 - but two identical circles are genuinely
 * ambiguous, and which one is A is the thing you need to know without
 * looking away from the game.
 */
static const uint8_t k_glyph_a[7] = { 0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11 };
static const uint8_t k_glyph_b[7] = { 0x0F, 0x11, 0x11, 0x0F, 0x11, 0x11, 0x0F };

static void glyph(tg_fb *fb, const uint8_t *g, int x, int y, int scale,
                  uint32_t c)
{
    int row;

    for (row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (g[row] & (1u << col))
                rect(fb, x + col * scale, y + row * scale, scale, scale, c);
}

/* ---- hit-testing ---------------------------------------------------------- */

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int in_disc(int x, int y, int cx, int cy, int r)
{
    int dx = x - cx, dy = y - cy;

    /* A little larger than it is drawn. A thumb's centre lands slightly below
       where its owner thinks it did, and a button that has to be hit exactly
       is a button that feels broken. */
    r += 6;
    return dx * dx + dy * dy <= r * r;
}

unsigned tg_pad_hit(int x, int y)
{
    /* The picture is not a control. Nothing above the pad does anything, which
       leaves the top half free for a future pause gesture without having to
       take it back from something. */
    if (y < PAD_TOP)
        return 0;

    if (in_disc(x, y, A_CX, A_CY, BTN_R)) return TG_A;
    if (in_disc(x, y, B_CX, B_CY, BTN_R)) return TG_B;

    if (in_rect(x, y, SELECT_X, PILL_Y, PILL_W, PILL_H)) return TG_SELECT;
    if (in_rect(x, y, START_X,  PILL_Y, PILL_W, PILL_H)) return TG_START;

    if (in_rect(x, y, DPAD_X, DPAD_Y, DPAD_SPAN, DPAD_SPAN)) {
        int col = (x - DPAD_X) / DPAD_CELL;
        int row = (y - DPAD_Y) / DPAD_CELL;
        unsigned bits = 0;

        /* The corners are what make diagonals work, and they are simply what
           falls out of testing the two axes independently. The centre cell is
           neither, which is the dead spot a real cross has. */
        if (col == 0) bits |= TG_LEFT;
        if (col == 2) bits |= TG_RIGHT;
        if (row == 0) bits |= TG_UP;
        if (row == 2) bits |= TG_DOWN;
        return bits;
    }

    return 0;
}

/* ---- drawing -------------------------------------------------------------- */

void tg_pad_invalidate(void) { s_drawn = ~0u; }

void tg_pad_draw(tg_fb *fb, unsigned held, bool force)
{
    uint32_t face;

    if (!fb || !fb->pixels)
        return;
    if (!force && held == s_drawn)
        return;
    s_drawn = held;

    /* The whole pad area, so a button that has just been released is cleared
       rather than left lit. */
    rect(fb, 0, PAD_TOP, (int)fb->w, (int)fb->h - PAD_TOP, C_PAD_BG);

    /* The cross. Drawn as three bars - the two arms and the middle - because
       that is the shape, and because the middle must not look like a button
       when it is not one. */
    face = (held & (TG_UP | TG_DOWN)) ? C_FACE_DOWN : C_FACE;
    rect(fb, DPAD_X + DPAD_CELL, DPAD_Y, DPAD_CELL, DPAD_SPAN, C_FACE);
    if (held & TG_UP)
        rect(fb, DPAD_X + DPAD_CELL, DPAD_Y, DPAD_CELL, DPAD_CELL, face);
    if (held & TG_DOWN)
        rect(fb, DPAD_X + DPAD_CELL, DPAD_Y + DPAD_CELL * 2,
             DPAD_CELL, DPAD_CELL, face);

    face = (held & (TG_LEFT | TG_RIGHT)) ? C_FACE_DOWN : C_FACE;
    rect(fb, DPAD_X, DPAD_Y + DPAD_CELL, DPAD_SPAN, DPAD_CELL, C_FACE);
    if (held & TG_LEFT)
        rect(fb, DPAD_X, DPAD_Y + DPAD_CELL, DPAD_CELL, DPAD_CELL, face);
    if (held & TG_RIGHT)
        rect(fb, DPAD_X + DPAD_CELL * 2, DPAD_Y + DPAD_CELL,
             DPAD_CELL, DPAD_CELL, face);

    /* A hairline around the middle cell, so the dead centre reads as part of
       the cross rather than as a fifth button. */
    rect(fb, DPAD_X + DPAD_CELL, DPAD_Y + DPAD_CELL, DPAD_CELL, 1, C_EDGE);
    rect(fb, DPAD_X + DPAD_CELL, DPAD_Y + DPAD_CELL * 2 - 1,
         DPAD_CELL, 1, C_EDGE);

    disc(fb, A_CX, A_CY, BTN_R, (held & TG_A) ? C_FACE_DOWN : C_FACE);
    disc(fb, B_CX, B_CY, BTN_R, (held & TG_B) ? C_FACE_DOWN : C_FACE);

    /* Centred by hand: five columns at two pixels is ten wide, seven rows is
       fourteen tall. */
    glyph(fb, k_glyph_a, A_CX - 5, A_CY - 7, 2, C_GLYPH);
    glyph(fb, k_glyph_b, B_CX - 5, B_CY - 7, 2, C_GLYPH);

    rect(fb, SELECT_X, PILL_Y, PILL_W, PILL_H,
         (held & TG_SELECT) ? C_FACE_DOWN : C_FACE);
    rect(fb, START_X, PILL_Y, PILL_W, PILL_H,
         (held & TG_START) ? C_FACE_DOWN : C_FACE);
}

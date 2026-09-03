/*
 * tg_tilt.c — see tg_tilt.h.
 */

#include "tg_tilt.h"
#include "../core/tg_core.h"

void tg_tilt_init(tg_tilt *t, int min, int max, int on_pct, int off_pct)
{
    t->min = min;
    t->max = max;
    t->cx = t->cy = 0;
    t->held = 0;

    /* Release must be under press, or there is no hysteresis at all and the
       direction chatters at exactly the angle being held. */
    if (off_pct >= on_pct) off_pct = on_pct > 1 ? on_pct - 1 : 0;
    t->on_pct = on_pct;
    t->off_pct = off_pct;
}

void tg_tilt_set_centre(tg_tilt *t, int cx, int cy)
{
    t->cx = cx;
    t->cy = cy;
}

static uint8_t axis(const tg_tilt *t, int raw, int centre,
                    uint8_t held, uint8_t neg, uint8_t pos)
{
    int span = (t->max - t->min) / 2;
    int off  = raw - centre;
    int on_t, off_t;

    if (span <= 0) return 0;

    on_t  = span * t->on_pct / 100;
    off_t = span * t->off_pct / 100;

    /* Past the press angle: assert, whatever was happening before. */
    if (off <= -on_t) return neg;
    if (off >=  on_t) return pos;

    /* Between the two angles, whatever was held stays held. This is the
       hysteresis, and it is why the two thresholds exist. */
    if ((held & neg) && off <= -off_t) return neg;
    if ((held & pos) && off >=  off_t) return pos;

    return 0;
}

uint8_t tg_tilt_feed(tg_tilt *t, int x, int y)
{
    uint8_t out = 0;

    out |= axis(t, x, t->cx, t->held, TG_LEFT, TG_RIGHT);
    out |= axis(t, y, t->cy, t->held, TG_UP,   TG_DOWN);

    t->held = out;
    return out;
}

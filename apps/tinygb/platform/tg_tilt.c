/*
 * tg_tilt.c — see tg_tilt.h.
 */

#include "tg_tilt.h"

#include <stdio.h>
#include "../core/tg_core.h"

void tg_tilt_init(tg_tilt *t, int min, int max, int on_pct, int off_pct)
{
    t->min = min;
    t->max = max;
    t->cx = t->cy = t->cz = 0;
    t->held = 0;

    /*
     * Upright is the assumption until a calibration says otherwise: gravity
     * down Y, so X leans and Z tips. set_centre replaces this the moment it
     * has readings to work from.
     */
    t->roll_axis   = TG_AX_X;
    t->pitch_axis  = TG_AX_Z;
    t->roll_sign   = -1;
    t->pitch_sign  = -1;

    /* Release must be under press, or there is no hysteresis at all and the
       direction chatters at exactly the angle being held. */
    if (off_pct >= on_pct) off_pct = on_pct > 1 ? on_pct - 1 : 0;
    t->on_pct = on_pct;
    t->off_pct = off_pct;
}

/*
 * Which way up is this being held, and therefore which axes can answer.
 *
 * An accelerometer at rest reads gravity and nothing else, so the axis
 * closest to a full g is the one pointing at the floor - and that axis cannot
 * report a tilt, because it is already at the end of the only range gravity
 * has. The other two are free to swing through the whole of it.
 *
 * So the largest reading names the posture, and the remaining two axes become
 * lean and tip. Held upright, gravity is down Y and the pair is X and Z; laid
 * flat on a table it is down Z and the pair is X and Y. Both are postures
 * somebody will actually play in.
 */
void tg_tilt_set_centre(tg_tilt *t, int cx, int cy, int cz)
{
    int ax = cx < 0 ? -cx : cx;
    int ay = cy < 0 ? -cy : cy;
    int az = cz < 0 ? -cz : cz;

    t->cx = cx;
    t->cy = cy;
    t->cz = cz;

    /*
     * The signs below are the ones this device wants, and left and right were
     * the wrong way round until somebody played it: leaning the device left
     * RAISES X here, so left is the positive direction and the axis helper is
     * handed its directions in that order.
     *
     * Tipping is the convention that reads naturally in a game: tipping the
     * top of the device away from you presses Up.
     */
    if (ay >= ax && ay >= az) {
        /* Upright, gravity down the long axis. */
        t->roll_axis  = TG_AX_X;  t->roll_sign  = -1;
        t->pitch_axis = TG_AX_Z;  t->pitch_sign = -1;
    } else if (az >= ax && az >= ay) {
        /* Flat, screen up. Z is pinned; X and Y both swing. */
        t->roll_axis  = TG_AX_X;  t->roll_sign  = -1;
        t->pitch_axis = TG_AX_Y;  t->pitch_sign = -1;
    } else {
        /* On its side, which is nobody's idea of a way to hold this - but a
           d-pad that goes dead rather than choosing is worse. */
        t->roll_axis  = TG_AX_Y;  t->roll_sign  = -1;
        t->pitch_axis = TG_AX_Z;  t->pitch_sign = -1;
    }
}

static const char *axis_name(int a)
{
    return a == TG_AX_X ? "X" : a == TG_AX_Y ? "Y" : "Z";
}

const char *tg_tilt_describe(const tg_tilt *t)
{
    static char buf[96];

    snprintf(buf, sizeof buf,
             "lean %s%s  tip %s%s  (centre %d,%d,%d)",
             t->roll_sign < 0 ? "-" : "+", axis_name(t->roll_axis),
             t->pitch_sign < 0 ? "-" : "+", axis_name(t->pitch_axis),
             t->cx, t->cy, t->cz);
    return buf;
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

/* One of the three readings, and where its level was measured. */
static void pick(const tg_tilt *t, int which, int x, int y, int z,
                 int *raw, int *centre)
{
    switch (which) {
    case TG_AX_X: *raw = x; *centre = t->cx; break;
    case TG_AX_Y: *raw = y; *centre = t->cy; break;
    default:      *raw = z; *centre = t->cz; break;
    }
}

uint8_t tg_tilt_feed(tg_tilt *t, int x, int y, int z)
{
    uint8_t out = 0;
    int raw, centre;

    /*
     * The sign decides which end of the axis is which direction, by swapping
     * the pair handed to the helper rather than negating the reading - the
     * centre is measured in the chip's own frame and negating around it would
     * be one more place to get a sign wrong.
     */
    pick(t, t->roll_axis, x, y, z, &raw, &centre);
    out |= (t->roll_sign < 0)
             ? axis(t, raw, centre, t->held, TG_RIGHT, TG_LEFT)
             : axis(t, raw, centre, t->held, TG_LEFT,  TG_RIGHT);

    pick(t, t->pitch_axis, x, y, z, &raw, &centre);
    out |= (t->pitch_sign < 0)
             ? axis(t, raw, centre, t->held, TG_DOWN, TG_UP)
             : axis(t, raw, centre, t->held, TG_UP,   TG_DOWN);

    t->held = out;
    return out;
}

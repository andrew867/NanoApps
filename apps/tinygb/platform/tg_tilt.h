/*
 * tg_tilt.h — an accelerometer as a d-pad.
 *
 * Portable and device-free, because two things need it. On Linux the readings
 * come from evdev; under RetailOS they come from the SDK's hb_accel, and the
 * decision - which way is this being leaned, and is it far enough to count -
 * is identical and worth getting right once.
 *
 * Two thresholds, not one. A single angle makes a direction chatter on and off
 * many times a second at exactly the tilt someone happens to be holding, which
 * in a falling-block game is unplayable. Pressing at a wider angle than it
 * releases at is hysteresis, and it is the whole difference between tilt
 * controls that work and tilt controls that are a demo.
 *
 * And the neutral is measured, not assumed. Nobody holds a device at zero: on
 * the bench this one rests near a full g down its long axis, which is past any
 * useful threshold, so an uncalibrated d-pad holds a direction down from the
 * moment it starts.
 */

#ifndef TINYGB_TILT_H
#define TINYGB_TILT_H

#include <stdbool.h>
#include <stdint.h>

/* Which of the chip's three axes an idea like "lean left" is read from. */
enum { TG_AX_X = 0, TG_AX_Y = 1, TG_AX_Z = 2 };

typedef struct {
    int     min, max;      /* the chip's full-scale range */
    int     cx, cy, cz;    /* where level is, on all three */
    int     on_pct, off_pct;
    uint8_t held;          /* what is currently asserted */

    /*
     * Which axis answers which question, and which way round.
     *
     * Not fixed, because it depends on how the device is being held. An
     * accelerometer at rest reads gravity, and the axis most closely aligned
     * with it is the one that CANNOT report a tilt: it is already at 1 g, and
     * 1 g is as far as gravity goes. Tipping that way moves it a few counts
     * into the rail and no further.
     *
     * Measured on this device held upright, Y rests at -1086 of +-2304. Full
     * scale is +-2 g, so 1 g is 1152 counts and Y has 66 counts of travel
     * left before it saturates - against a press threshold of 507. Leaning
     * that way could never fire, which is exactly what it did.
     *
     * So the gravity axis is found at calibration and the other two are used.
     */
    int     roll_axis,  roll_sign;    /* left and right */
    int     pitch_axis, pitch_sign;   /* towards and away */
} tg_tilt;

/*
 * `min`/`max` are the accelerometer's reported range. The thresholds are a
 * percentage of half that range, which is a sine of the angle: on this chip
 * full scale is 2 g, so 14% is about 16 degrees.
 */
void tg_tilt_init(tg_tilt *t, int min, int max, int on_pct, int off_pct);

/*
 * Where level is, and - from that - which axes are worth reading.
 *
 * Feed it the average of a moment's readings taken while the device is held
 * the way it will be played. Whichever axis is closest to a full g is taken
 * to be pointing at the floor and is left out of the d-pad.
 */
void tg_tilt_set_centre(tg_tilt *t, int cx, int cy, int cz);

/* Which axes it settled on, for the front end to print. A wrong sign is one
   line in a log rather than an afternoon. */
const char *tg_tilt_describe(const tg_tilt *t);

/*
 * The d-pad bits for this reading, as TG_UP/DOWN/LEFT/RIGHT from tg_core.h.
 *
 * X leans left and right; Y tips towards and away. Both axes are evaluated
 * every call, so a diagonal is two bits and not a fight between them.
 */
uint8_t tg_tilt_feed(tg_tilt *t, int x, int y, int z);

#endif /* TINYGB_TILT_H */

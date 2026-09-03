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

typedef struct {
    int     min, max;      /* the chip's full-scale range */
    int     cx, cy;        /* where level is */
    int     on_pct, off_pct;
    uint8_t held;          /* what is currently asserted */
} tg_tilt;

/*
 * `min`/`max` are the accelerometer's reported range. The thresholds are a
 * percentage of half that range, which is a sine of the angle: on this chip
 * full scale is 2 g, so 14% is about 16 degrees.
 */
void tg_tilt_init(tg_tilt *t, int min, int max, int on_pct, int off_pct);

/* Where level is. Feed it the average of a moment's readings taken while the
   device is held the way it will be played. */
void tg_tilt_set_centre(tg_tilt *t, int cx, int cy);

/*
 * The d-pad bits for this reading, as TG_UP/DOWN/LEFT/RIGHT from tg_core.h.
 *
 * X leans left and right; Y tips towards and away. Both axes are evaluated
 * every call, so a diagonal is two bits and not a fight between them.
 */
uint8_t tg_tilt_feed(tg_tilt *t, int x, int y);

#endif /* TINYGB_TILT_H */

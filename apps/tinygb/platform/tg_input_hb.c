/*
 * tg_input_hb.c — see tg_input_hb.h.
 */

#include "tg_input_hb.h"
#include "tg_pad.h"
#include "../core/tg_core.h"

#include "hb_sdk.h"

#include <string.h>

/* The SDK reports milli-g; +-1000 is the range tg_tilt's percentages are of.
   Same press and release angles as the Linux front end's defaults. */
#define TILT_RANGE   1000
#define TILT_ON_PCT  22
#define TILT_OFF_PCT 12
#define CALIB_MS     250

void tg_input_hb_init(tg_input_hb *in, bool tilt)
{
    memset(in, 0, sizeof *in);
    in->tilt_on = tilt;
    tg_tilt_init(&in->tilt, -TILT_RANGE, TILT_RANGE, TILT_ON_PCT, TILT_OFF_PCT);
    in->calib_until_ms = hb_time_uptime_ms() + CALIB_MS;
}

static unsigned tilt_bits(tg_input_hb *in)
{
    int32_t a[3];

    hb_accel_read_milli_g(a);

    if (!in->calibrated) {
        in->sx += a[0]; in->sy += a[1]; in->sz += a[2];
        in->samples++;
        if ((int32_t)(hb_time_uptime_ms() - in->calib_until_ms) < 0)
            return 0;
        tg_tilt_set_centre(&in->tilt,
                           (int)(in->sx / (long)in->samples),
                           (int)(in->sy / (long)in->samples),
                           (int)(in->sz / (long)in->samples));
        in->calibrated = true;
    }
    return tg_tilt_feed(&in->tilt, a[0], a[1], a[2]);
}

unsigned tg_input_hb_poll(tg_input_hb *in)
{
    unsigned held = 0;
    hb_touch_t fingers[HB_MAX_FINGERS];
    int n;

    /* The two volume keys are GPIO lines read directly, so both can be down
       at once - which the OS's own button cache does not allow. */
    if (hb_button_pressed(HB_BTN_VOL_UP))   held |= TG_A;
    if (hb_button_pressed(HB_BTN_VOL_DOWN)) held |= TG_B;

    /*
     * Once per tick, in this order: drain every finger's queue down to its
     * newest sample, then read them all. A slot whose last sample says "up"
     * is a finger that has left; the coordinates are where it lifted, not a
     * press.
     */
    hb_touch_drain_all();
    n = hb_touch_poll_multi(fingers);
    for (int i = 0; i < n; i++) {
        if (fingers[i].status == 1) continue;   /* up */
        held |= tg_pad_hit(fingers[i].x, fingers[i].y);
    }

    if (in->tilt_on) held |= tilt_bits(in);

    return held;
}

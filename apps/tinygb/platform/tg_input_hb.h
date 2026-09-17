/*
 * tg_input_hb.h — eight Game Boy buttons out of the nano's SDK, on RetailOS.
 *
 * Three sources, OR-ed into one held mask, the same shape as the Linux
 * front end's tg_input:
 *
 *   the touch pad     every active finger through tg_pad_hit - a direction
 *                     and A at once needs the multitouch slots, which the
 *                     single point hb_raw_frame is handed cannot carry
 *   Vol Up / Down     A and B, read as GPIO levels; they work together
 *   the accelerometer as a d-pad through tg_tilt, when the player asks
 *
 * Play/Pause is deliberately not here. RetailOS acts on it before the app
 * sees it and starts the Music player underneath, so Start and Select live
 * on the pad. Home is not here either: it is how you leave.
 */

#ifndef TINYGB_INPUT_HB_H
#define TINYGB_INPUT_HB_H

#include <stdbool.h>
#include <stdint.h>

#include "tg_tilt.h"

typedef struct {
    bool     tilt_on;
    tg_tilt  tilt;

    /* Calibration: the first quarter second of readings after a cartridge
       starts says where "level" is for the way this one is being held. */
    bool     calibrated;
    uint32_t calib_until_ms;
    long     sx, sy, sz;
    unsigned samples;
} tg_input_hb;

void tg_input_hb_init(tg_input_hb *in, bool tilt);

/* Everything held right now: TG_* bits, plus TG_PAD_MENU from the pad.
   Never blocks. Call once per tick, and only from the tick - it drains the
   OS touch queues. */
unsigned tg_input_hb_poll(tg_input_hb *in);

#endif /* TINYGB_INPUT_HB_H */

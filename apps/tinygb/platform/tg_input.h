/*
 * tg_input.h — eight Game Boy buttons out of whatever this device has.
 *
 * The nano 7 has no gamepad and, for now, no working touchscreen: the Apple
 * Grape controller fails its firmware handshake on this build and parks itself
 * without registering. So the buttons have to come from several places at
 * once, and which places are available changes as the port progresses.
 *
 * That is why this is a set of SOURCES feeding one button mask rather than a
 * function that reads the touchscreen. Each source contributes bits; they are
 * OR-ed. Adding the touchscreen when Grape boots, or a Bluetooth pad in Phase
 * 06, is a new source and not a rewrite of the front end.
 *
 * What exists today, read off the device:
 *
 *   event0  n31-buttons     Vol Up, Vol Down          (GPIO, works)
 *   event1  MikeyBus Remote headphone remote keys      (works)
 *   event2  LIS3LV02DL      three-axis accelerometer   (works)
 *   event3  n31-pmic        Power, Home                (works, and reserved)
 *   event4  Apple Grape     multitouch                 (parked, no events)
 *
 * Home and Power are deliberately never mapped to a game button. Home is how
 * you get out, and Power is how the launcher sleeps the screen; a game that
 * swallowed either would be a game you cannot leave.
 */

#ifndef TINYGB_INPUT_H
#define TINYGB_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Which sources are wired up. A bitmask so the front end can say what it
   found, and so a scheme can be forced for testing. */
enum {
    TG_SRC_KEYS  = 1u << 0,   /* the physical buttons */
    TG_SRC_TILT  = 1u << 1,   /* accelerometer as a d-pad */
    TG_SRC_TOUCH = 1u << 2,   /* the on-screen pad, when Grape lives */
};

typedef struct tg_input tg_input;

/*
 * Open every input device that offers something usable.
 *
 * `sources` is the set to enable; TG_SRC_* OR-ed together. Sources whose
 * device is missing are dropped silently and reported by tg_input_sources.
 */
tg_input *tg_input_open(unsigned sources);
void      tg_input_close(tg_input *in);

/* What was actually found, which may be less than was asked for. */
unsigned tg_input_sources(const tg_input *in);

/*
 * Drain every pending event and return the buttons currently held, as TG_*
 * bits from tg_core.h. Never blocks.
 *
 * Held rather than pressed: the Game Boy's joypad register is a level, not an
 * edge, and a game samples it whenever it likes.
 */
uint8_t tg_input_poll(tg_input *in);

/*
 * Has the player asked to leave?
 *
 * Home is the way out of a running game and is checked separately from the
 * button mask, because it is not a Game Boy button and must not reach the
 * cartridge. Cleared by reading it.
 */
bool tg_input_take_quit(tg_input *in);

/* ---- tilt ---------------------------------------------------------------- */

/*
 * How far the device has to lean before an axis counts as pressed, and how far
 * back before it releases.
 *
 * Two thresholds rather than one: a single threshold at the exact tilt where a
 * game is being held makes the direction chatter on and off many times a
 * second, which in a falling-block game is unplayable. The gap is hysteresis
 * and it is the difference between tilt controls that work and tilt controls
 * that are a novelty.
 *
 * In units of the accelerometer's own reported range, as a percentage of full
 * scale, so it does not depend on what the chip reports for 1 g.
 */
void tg_input_set_tilt(tg_input *in, int on_pct, int off_pct);

/*
 * Override the neutral position measured at startup.
 *
 * tg_input_open spends its first quarter second deciding where "level" is,
 * because nobody holds a device at zero - it rests near a full g down the long
 * axis, which is past any threshold worth having. This is for a caller that
 * knows better, or a test.
 */
void tg_input_set_centre(tg_input *in, int x, int y);

/* Print what every device reports, and keep printing events until `secs` have
   passed. For working out on a real device what is actually connected. */
void tg_input_probe(unsigned secs);

#endif /* TINYGB_INPUT_H */

/*
 * tinygb.c — the RetailOS front end. Phase 04.
 *
 * Nothing here yet but the shape. The emulator itself is done and tested:
 * core/tg_core.h is the interface, core/tg_peanut.c is the Game Boy, and
 * host/ builds a headless runner that passes Blargg's cpu_instrs and
 * instr_timing and matches dmg-acid2's reference frame exactly. All of that is
 * portable C with no device in it.
 *
 * What goes here is the part that is specific to running under RetailOS:
 *
 *   - a raw surface, with hb_raw_frame() on the ~60 Hz heartbeat running one
 *     emulated frame and blitting it scaled 1.5x into 240x216
 *   - the touch pad in the 240x216 below it, read through
 *     hb_touch_drain_all() + hb_touch_poll_multi() so chords work
 *   - Vol Up / Vol Down / Play-Pause as A / B / Start via hb_button_pressed()
 *   - ROMs off the main volume through hb_fs
 *   - sound through the chained SFX descriptors Entrain already worked out,
 *     generated at 22050 to match the mixer
 *
 * Built as a raw-surface app (see Makefile, RAW_SURFACE := 1). Left as a stub
 * rather than as the copy of Paint the scaffolder makes, because a Paint clone
 * wearing TinyGB's name and icon is worse than nothing on the home screen.
 */

#include "../../sdk/hb_sdk.h"

void hb_app_main(void)
{
    /* Deliberately empty. Phase 04 fills this in; until then the app does not
       claim to do anything it cannot. */
}

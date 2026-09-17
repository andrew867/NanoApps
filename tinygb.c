/*
 * tinygb.c — the RetailOS front end. Phase 04.
 *
 * The emulator itself is done and tested: core/tg_core.h is the interface,
 * core/tg_peanut.c is the Game Boy, and host/ builds a headless runner that
 * passes Blargg's cpu_instrs and instr_timing and matches dmg-acid2's
 * reference frame exactly. All of that is portable C with no device in it.
 *
 * What goes here is the part that is specific to running under RetailOS as a
 * NanoApps raw-surface app - see docs/SPEC-retailos-frontend.md for the whole
 * of it. In one paragraph:
 *
 *   - hb_raw_frame() on the ~60 Hz heartbeat runs 0, 1 or 2 emulated frames,
 *     as many as the audio queue is short by, and blits the latest one scaled
 *     1.5x into the top 240x216 of the surface
 *   - the touch pad in the 240x216 below it, read through hb_touch_drain_all()
 *     + hb_touch_poll_multi() so a direction and A can be held together
 *   - Vol Up / Vol Down as A / B through hb_button_pressed(); Play/Pause is
 *     left alone, because RetailOS acts on it first and starts the Music app
 *   - cartridges from /Apps/Data/TinyGB/roms through hb_fs, read whole
 *   - sound through chained SoundEffect descriptors at 22050, the mechanism
 *     Entrain worked out (docs/SPEC-retailos-audio.md)
 *
 * Until Phase 3 fills that in, this is the smallest raw-surface app that
 * links: it paints the surface once and does nothing on the heartbeat. That
 * is not decoration. NanoApps builds every app in apps/ with one command, and
 * an app whose entry points do not exist fails the whole run at the packer -
 * which is what the previous stub, with a function no SDK ever called, did.
 */

#include "hb_sdk.h"
#include "hb_raw_surface.h"

/* The green-black of the N31 menu, so the two front ends read as one app. */
#define TG_ROS_BG 0x0B0F0Au

void hb_raw_init(int w, int h)
{
    (void)w;
    (void)h;

    /* The framebuffer is shared between apps and holds whatever the last one
       left there. Paint all of it before the first tick returns. */
    hb_raw_fill(TG_ROS_BG);
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    (void)touch;
}

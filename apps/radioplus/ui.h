/*
 * ui.h — the Radio+ screens.
 *
 * 240 x 432, which is a tall narrow portrait and shapes everything: the
 * frequency gets the top third because it is what you look at, the station and
 * its radio text get the middle because that is what you read, and the
 * transport gets the bottom because that is what your thumb reaches.
 */

#ifndef RADIOPLUS_UI_H
#define RADIOPLUS_UI_H

#include <stdbool.h>
#include <stdint.h>

#define RP_SCREEN_W 240
#define RP_SCREEN_H 432

/*
 * The first several are the swipe order and get a page dot each. The last two
 * are reached from settings and from the register list, and deliberately are
 * not: swiping into a register editor by accident would be a poor surprise.
 *
 * Two of the swipe screens are optional and the order is therefore built at
 * run time rather than being this enum's order - see rp_ui_swipe_count(). An
 * enum that lied about the sequence would be worse than no enum.
 */
typedef enum {
    RP_SCREEN_SIMPLE = 0,   /* optional: a handful of big preset buttons */
    RP_SCREEN_NOW,
    RP_SCREEN_WIDE,         /* optional: the landscape readout */
    RP_SCREEN_DIAL,
    RP_SCREEN_PRESETS,
    RP_SCREEN_LIBRARY,
    RP_SCREEN_SETTINGS,

    RP_SWIPE_MAX,
    RP_SCREEN_ADVANCED = RP_SWIPE_MAX,     /* the register list */
    RP_SCREEN_REGISTER,                    /* one register, field by field */
    RP_SCREEN_COUNT
} rp_screen_t;

/* How many screens are currently in the swipe sequence, and what is at
   position `i`. Both change when the optional screens are turned on or off. */
int         rp_ui_swipe_count(void);
rp_screen_t rp_ui_swipe_at(int i);

/*
 * The boot screen, and the only thing on the display until the rest is built.
 *
 * Bringing the tuner and both halves of the audio path up takes several
 * seconds, and none of the real screens exist until it finishes - so the panel
 * held whatever was on it, which reads as a device that did not start rather
 * than one that is starting.
 *
 * Call rp_ui_boot() before any of that with each step as it begins. It builds
 * its screen on the first call and refreshes synchronously on every one, so a
 * step that blocks for four seconds still leaves its own name on the screen
 * while it does.
 */
void rp_ui_boot(const char *step);

/* What the boot screen should say when a step failed rather than finished.
   Shown in the warning colour and kept on screen under the following steps,
   because the first thing that went wrong is the useful one. */
void rp_ui_boot_failed(const char *what);

void rp_ui_init(void);
void rp_ui_tick(void);           /* call from the frame callback */
void rp_ui_show(rp_screen_t s);

/* Open the editor for one register. */
void rp_ui_open_register(uint8_t addr);
rp_screen_t rp_ui_current(void);

/* Build every screen without showing any, for the headless renderer. */
void rp_ui_build_all(void);

#endif /* RADIOPLUS_UI_H */

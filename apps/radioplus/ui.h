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
 * The first five are the swipe order and get a page dot each. The last two are
 * reached from settings and from the register list, and deliberately are not:
 * swiping into a register editor by accident would be a poor surprise.
 */
typedef enum {
    RP_SCREEN_NOW = 0,
    RP_SCREEN_DIAL,
    RP_SCREEN_PRESETS,
    RP_SCREEN_LIBRARY,
    RP_SCREEN_SETTINGS,

    RP_SWIPE_COUNT,
    RP_SCREEN_ADVANCED = RP_SWIPE_COUNT,   /* the register list */
    RP_SCREEN_REGISTER,                    /* one register, field by field */
    RP_SCREEN_COUNT
} rp_screen_t;

void rp_ui_init(void);
void rp_ui_tick(void);           /* call from the frame callback */
void rp_ui_show(rp_screen_t s);

/* Open the editor for one register. */
void rp_ui_open_register(uint8_t addr);
rp_screen_t rp_ui_current(void);

/* Build every screen without showing any, for the headless renderer. */
void rp_ui_build_all(void);

#endif /* RADIOPLUS_UI_H */

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

typedef enum {
    RP_SCREEN_NOW = 0,
    RP_SCREEN_DIAL,
    RP_SCREEN_PRESETS,
    RP_SCREEN_LIBRARY,
    RP_SCREEN_SETTINGS,
    RP_SCREEN_ADVANCED,      /* the register explorer; reached from settings */
    RP_SCREEN_COUNT
} rp_screen_t;

void rp_ui_init(void);
void rp_ui_tick(void);           /* call from the frame callback */
void rp_ui_show(rp_screen_t s);
rp_screen_t rp_ui_current(void);

/* Build every screen without showing any, for the headless renderer. */
void rp_ui_build_all(void);

#endif /* RADIOPLUS_UI_H */

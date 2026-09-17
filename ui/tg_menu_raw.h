/*
 * tg_menu_raw.h — the menu, drawn by hand, for a surface with no toolkit.
 *
 * The same pages and rows as the LVGL menu in tg_menu.c, in the shape the
 * nano's own screens have: a title bar with a back chevron, full-width rows
 * with a chevron on the ones that open a page, a check on the chosen entry
 * of a list, a pill on a toggle. Tap a row; drag to scroll; swipe in from
 * the left edge, or tap the chevron, to go back. Vol Up and Vol Down move a
 * highlight too, so the thing is usable with a thumb on the buttons.
 *
 * Not a loop. RetailOS draws only when the tick returns, so this is a mode:
 * the front end calls tg_menu_raw_tick once a heartbeat with where the
 * finger is and which keys are down, and gets back what the player decided,
 * if anything. Nothing in here touches the OS; the surface, the theme and the
 * clock come in as arguments, which is what lets the host tests tap on it.
 */

#ifndef TINYGB_MENU_RAW_H
#define TINYGB_MENU_RAW_H

#include <stdbool.h>
#include <stdint.h>

#include "tg_menu.h"
#include "../platform/tg_roms.h"
#include "../platform/tg_surface.h"

/* What tg_menu_raw_tick returns: a tg_menu_action, or one of these. */
enum {
    TG_MENU_RAW_NONE             = -1,
    TG_MENU_RAW_SETTINGS_CHANGED = 100,  /* palette, scaling, tilt or overlay */
    TG_MENU_RAW_RESUME_STATE     = 101,  /* start st->rom_path and load its state */
    TG_MENU_RAW_CHOOSE           = 102,  /* leave the game for the library */
};

enum {
    TG_MR_PAGE_ROMS = 0,
    TG_MR_PAGE_PAUSE,
    TG_MR_PAGE_SETTINGS,
    TG_MR_PAGE_PALETTE,
    TG_MR_PAGE_ABOUT,
    TG_MR_PAGES
};

typedef struct {
    uint32_t bg, surface, text, dim, primary, on_primary;
} tg_menu_theme;

typedef struct {
    tg_menu_state *st;
    tg_menu_theme  theme;

    /* The shelf, for the ROMS page. rom_size lets a cartridge the front
       end cannot hold be shown greyed with its size; NULL shows them all
       plain. */
    const tg_rom_list *lib;
    bool lib_ok;
    long (*rom_size)(unsigned i, void *user);
    void *user;
    long rom_max;

    /* For the About page. */
    const char *build;
    const char *core_name;
    const char *(*recent_log)(unsigned i);

    char note[96];              /* one line at the bottom, or empty */

    /* internal */
    int  page;
    int  stack[4];
    int  depth;
    int  sel[TG_MR_PAGES];
    int  scroll[TG_MR_PAGES];
    bool dirty;

    bool prev_down;
    int  down_x, down_y, last_y;
    int  moved;
    int  scroll_at_down;
    int  press_row;             /* the row lit under the finger, or -1 */

    unsigned prev_keys;
    uint32_t key_down_ms, key_repeat_ms;
} tg_menu_raw;

void tg_menu_raw_init(tg_menu_raw *m, tg_menu_state *st, const tg_menu_theme *theme);
void tg_menu_raw_set_library(tg_menu_raw *m, const tg_rom_list *lib, bool ok);

/* Start on the ROM picker, or on the pause page of the running game. */
void tg_menu_raw_open(tg_menu_raw *m, bool pause);

/*
 * One heartbeat. (tx, ty, down) is the pointer; `keys` is bit 0 for Vol Up
 * and bit 1 for Vol Down, as levels. Draws when something changed. Returns
 * a decision, or TG_MENU_RAW_NONE.
 */
int tg_menu_raw_tick(tg_menu_raw *m, const tg_surface *fb, int tx, int ty,
                     bool down, unsigned keys, uint32_t now_ms);

void tg_menu_raw_note(tg_menu_raw *m, const char *text);
void tg_menu_raw_invalidate(tg_menu_raw *m);
int  tg_menu_raw_page(const tg_menu_raw *m);

#endif /* TINYGB_MENU_RAW_H */

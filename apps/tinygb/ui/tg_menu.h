/*
 * tg_menu.h — the menu, in the TinyPod and Entrain house style.
 *
 * Two things share one screen and neither may draw while the other is on it:
 * the emulator writes scaled pixels straight into the mapped framebuffer, and
 * LVGL paints widgets into the same memory. So LVGL is set up once and then
 * told to hold still - tg_menu_suspend before the game takes over, and
 * tg_menu_resume, which invalidates everything, on the way back. Tearing LVGL
 * down and building it again between every game would be tidier on paper and
 * is four hundred kilobytes of allocation each way.
 *
 * The buttons are the same four the rest of the device uses, which on a
 * handheld with no touchscreen is what makes the menu learnable: whatever
 * you learned in TinyPod is true here.
 *
 *   Vol Up / Vol Down    move the selection
 *   Play/Pause           choose
 *   Home                 back, and from the top screen, leave
 */

#ifndef TINYGB_MENU_H
#define TINYGB_MENU_H

#include <stdbool.h>
#include <stddef.h>

/* What the menu decided. tg_menu_run returns when one of these happens. */
enum tg_menu_action {
    TG_MENU_QUIT = 0,   /* leave TinyGB */
    TG_MENU_PLAY,       /* start st->rom_path */
    TG_MENU_RESUME,     /* back to the game already running */
    TG_MENU_SAVE_STATE,
    TG_MENU_LOAD_STATE,
    TG_MENU_RESET       /* restart the cartridge from power-on */
};

/*
 * What the menu shows and what it changes.
 *
 * The caller owns this: the menu reads it to draw the current settings and
 * writes back what was chosen, so nothing about the running game lives in
 * here. `have_game` decides whether Resume, Save and Load are offered at all.
 */
typedef struct {
    char     rom_path[768];
    char     rom_title[64];   /* what to call the game that is loaded */
    unsigned palette;         /* index into tg_palette */
    bool     smooth;
    bool     tilt;
    bool     have_game;
    bool     can_state;       /* the core supports save states */
    bool     have_state;      /* one has been written for this cartridge */
} tg_menu_state;

/*
 * Bring the menu up on `fb`. Returns false when there is no framebuffer or
 * LVGL will not start, which the caller should say out loud rather than
 * sitting on a blank screen.
 */
bool tg_menu_init(const char *fb);
void tg_menu_close(void);

/*
 * Run the menu until it decides something.
 *
 * `pause` chooses which screen it opens on: false for the ROM picker, which
 * is where the app starts, and true for the in-game menu reached with Home.
 */
enum tg_menu_action tg_menu_run(tg_menu_state *st, bool pause);

/* Around the game, which owns the framebuffer while it runs. */
void tg_menu_suspend(void);
void tg_menu_resume(void);

/* A line under the title while something slow is happening. */
void tg_menu_note(const char *text);

#endif /* TINYGB_MENU_H */

/*
 * tg_menu.c — see tg_menu.h.
 */

#include "tg_menu.h"

#include "../platform/tg_input.h"
#include "../platform/tg_palette.h"
#include "../platform/tg_roms.h"
#include "../core/tg_core.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The panel, as the rest of this app knows it. */
#define W 240
#define H 432

#define MARGIN     12
#define CONTENT_W  (W - 2 * MARGIN)
#define TITLE_Y    10
#define NOTE_Y     34
#define LIST_TOP   58
#define FOOTER_Y   (H - 24)
#define ROW_H      48
#define ROWS       6

/*
 * The house palette, taken from TinyPod so the two look like one device
 * rather than two projects. Green rather than TinyPod's, because that is
 * TinyGB's accent in the launcher manifest and the tile it was opened from.
 */
#define C_BG      0x0B0F0A
#define C_PANEL   0x151B14
#define C_SEL     0x2A3A22
#define C_TEXT    0xE8EEE2
#define C_MUTE    0x8A9683
#define C_ACCENT  0x7BAB3A

/* ---- state --------------------------------------------------------------- */

static lv_display_t *s_disp;
static lv_obj_t     *s_screen;
static lv_obj_t     *s_title;
static lv_obj_t     *s_note;
static lv_obj_t     *s_hint;
static lv_obj_t     *s_empty;

static struct {
    lv_obj_t *root, *l1, *l2, *mark;
} s_row[ROWS];

static tg_input *s_in;
static bool      s_suspended;

/* Which screen the menu is on. */
enum page { P_ROMS = 0, P_PAUSE, P_SETTINGS, P_PALETTE };

static enum page s_page;
static int       s_sel[4];
static int       s_top[4];

static tg_rom_list s_lib;
static bool        s_lib_ok;

/* ---- LVGL scaffolding ---------------------------------------------------- */

static uint32_t tick_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static unsigned long now_ms(void)
{
    return tick_ms();
}

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *o = lv_label_create(parent);

    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_screen(void)
{
    int i;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, W, H);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_title = label(s_screen, "TinyGB", &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_pos(s_title, MARGIN, TITLE_Y);

    s_note = label(s_screen, "", &lv_font_montserrat_14, C_MUTE);
    lv_obj_set_pos(s_note, MARGIN, NOTE_Y);
    lv_obj_set_width(s_note, CONTENT_W);

    for (i = 0; i < ROWS; i++) {
        int y = LIST_TOP + i * ROW_H;

        s_row[i].root = panel(s_screen, MARGIN, y, CONTENT_W, ROW_H - 6,
                              C_PANEL);

        /* The accent bar that marks the selection. Four pixels down the left
           edge rather than a full-width fill: at this size a filled row makes
           the text underneath hard to read. */
        s_row[i].mark = panel(s_row[i].root, 0, 0, 4, ROW_H - 6, C_ACCENT);

        s_row[i].l1 = label(s_row[i].root, "", &lv_font_montserrat_16, C_TEXT);
        lv_obj_set_pos(s_row[i].l1, 14, 4);
        lv_obj_set_width(s_row[i].l1, CONTENT_W - 24);
        lv_label_set_long_mode(s_row[i].l1, LV_LABEL_LONG_DOT);

        s_row[i].l2 = label(s_row[i].root, "", &lv_font_montserrat_14, C_MUTE);
        lv_obj_set_pos(s_row[i].l2, 14, 25);
        lv_obj_set_width(s_row[i].l2, CONTENT_W - 24);
        lv_label_set_long_mode(s_row[i].l2, LV_LABEL_LONG_DOT);
    }

    s_empty = label(s_screen, "", &lv_font_montserrat_14, C_MUTE);
    lv_obj_set_pos(s_empty, MARGIN, LIST_TOP + 8);
    lv_obj_set_width(s_empty, CONTENT_W);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);

    s_hint = label(s_screen, "", &lv_font_montserrat_14, C_MUTE);
    lv_obj_set_pos(s_hint, MARGIN, FOOTER_Y);
    lv_obj_set_width(s_hint, CONTENT_W);

    lv_screen_load(s_screen);
}

bool tg_menu_init(const char *fb)
{
    lv_init();
    lv_tick_set_cb(tick_ms);

    s_disp = lv_linux_fbdev_create();
    if (!s_disp) {
        fprintf(stderr, "tinygb: LVGL would not open a display\n");
        return false;
    }
    lv_linux_fbdev_set_file(s_disp, fb ? fb : "/dev/fb0");
    lv_display_set_resolution(s_disp, W, H);

    build_screen();

    /*
     * The menu shares the game's input, and for the same reason the game has
     * it: these are the only buttons there are. Tilt is off here - a menu that
     * scrolls when the device is picked up is a menu nobody can use.
     */
    s_in = tg_input_open(TG_SRC_KEYS);

    s_lib_ok = tg_roms_scan(&s_lib);
    return true;
}

void tg_menu_close(void)
{
    if (s_in) tg_input_close(s_in);
    s_in = NULL;
    tg_roms_free(&s_lib);
}

void tg_menu_suspend(void)
{
    s_suspended = true;
}

void tg_menu_resume(void)
{
    s_suspended = false;
    /* The game has been writing over every pixel LVGL believes it owns, so
       nothing on screen is what LVGL last drew. */
    if (s_screen)
        lv_obj_invalidate(s_screen);
}

void tg_menu_note(const char *text)
{
    if (!s_note) return;
    lv_label_set_text(s_note, text ? text : "");
    if (!s_suspended) {
        lv_refr_now(NULL);
    }
}

/* ---- what each page holds ------------------------------------------------ */

/* The pause menu, in the order it is drawn. */
enum {
    PAUSE_RESUME = 0,
    PAUSE_SAVE,
    PAUSE_LOAD,
    PAUSE_RESET,
    PAUSE_SETTINGS,
    PAUSE_LIBRARY,
    PAUSE_QUIT,
    PAUSE_N
};

enum { SET_PALETTE = 0, SET_SMOOTH, SET_TILT, SET_N };

static int page_count(const tg_menu_state *st)
{
    switch (s_page) {
    case P_ROMS:     return s_lib_ok ? (int)s_lib.n : 0;
    case P_PAUSE:    return PAUSE_N;
    case P_SETTINGS: return SET_N;
    case P_PALETTE:  return (int)tg_palette_count();
    }
    (void)st;
    return 0;
}

/*
 * One row's two lines. Returns false for a row that is not there, which is
 * how the drawing loop knows to blank the rest.
 */
static bool page_row(const tg_menu_state *st, int i,
                     const char **l1, const char **l2)
{
    static char buf[64];

    *l2 = NULL;

    switch (s_page) {
    case P_ROMS:
        if (!s_lib_ok || (unsigned)i >= s_lib.n) return false;
        *l1 = s_lib.name[i];
        return true;

    case P_PAUSE:
        switch (i) {
        case PAUSE_RESUME: *l1 = "Resume";      return true;
        case PAUSE_SAVE:
            *l1 = "Save state";
            if (!st->can_state) *l2 = "this core cannot";
            return true;
        case PAUSE_LOAD:
            *l1 = "Load state";
            *l2 = st->have_state ? NULL : "nothing saved yet";
            return true;
        case PAUSE_RESET:    *l1 = "Restart cartridge"; return true;
        case PAUSE_SETTINGS: *l1 = "Settings";          return true;
        case PAUSE_LIBRARY:  *l1 = "Choose another game"; return true;
        case PAUSE_QUIT:     *l1 = "Quit TinyGB";       return true;
        }
        return false;

    case P_SETTINGS:
        switch (i) {
        case SET_PALETTE:
            *l1 = "Palette";
            *l2 = tg_palette_at(st->palette)->name;
            return true;
        case SET_SMOOTH:
            *l1 = "Scaling";
            *l2 = st->smooth ? "Smooth" : "Sharp";
            return true;
        case SET_TILT:
            *l1 = "Tilt d-pad";
            *l2 = st->tilt ? "On" : "Off";
            return true;
        }
        return false;

    case P_PALETTE:
        if ((unsigned)i >= tg_palette_count()) return false;
        *l1 = tg_palette_at((unsigned)i)->name;
        if ((unsigned)i == st->palette) {
            snprintf(buf, sizeof buf, "in use");
            *l2 = buf;
        }
        return true;
    }
    return false;
}

static const char *page_title(const tg_menu_state *st)
{
    switch (s_page) {
    case P_ROMS:     return "TinyGB";
    case P_PAUSE:    return st->rom_title[0] ? st->rom_title : "Paused";
    case P_SETTINGS: return "Settings";
    case P_PALETTE:  return "Palette";
    }
    return "TinyGB";
}

static const char *page_hint(void)
{
    switch (s_page) {
    case P_ROMS:  return "VOL move   PLAY start   HOME exit";
    case P_PAUSE: return "VOL move   PLAY choose   HOME resume";
    default:      return "VOL move   PLAY choose   HOME back";
    }
}

/* ---- drawing ------------------------------------------------------------- */

static void draw(const tg_menu_state *st)
{
    int count = page_count(st);
    int sel = s_sel[s_page];
    int top = s_top[s_page];
    int i;

    /* Keep the selection on screen. */
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    if (top < 0) top = 0;
    s_top[s_page] = top;

    lv_label_set_text(s_title, page_title(st));
    lv_label_set_text(s_hint, page_hint());

    for (i = 0; i < ROWS; i++) {
        int idx = top + i;
        const char *l1 = NULL, *l2 = NULL;

        if (idx < count && page_row(st, idx, &l1, &l2)) {
            lv_obj_remove_flag(s_row[i].root, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_row[i].l1, l1 ? l1 : "");
            lv_label_set_text(s_row[i].l2, l2 ? l2 : "");

            if (idx == sel) {
                lv_obj_set_style_bg_color(s_row[i].root,
                                          lv_color_hex(C_SEL), 0);
                lv_obj_remove_flag(s_row[i].mark, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_style_bg_color(s_row[i].root,
                                          lv_color_hex(C_PANEL), 0);
                lv_obj_add_flag(s_row[i].mark, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_row[i].root, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (count == 0 && s_page == P_ROMS) {
        char msg[512];

        snprintf(msg, sizeof msg,
                 "No cartridges in\n%.400s\n\nPut .gb files there, or set "
                 "TINYGB_ROMS.", s_lib.dir);
        lv_label_set_text(s_empty, msg);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- buttons ------------------------------------------------------------- */

/*
 * The menu's four keys, out of the game's button mask.
 *
 * Edges rather than levels, with a repeat while one is held: the mask says
 * what is down, and a menu that acted on that would run the whole list past
 * in a single press. The first repeat waits longer than the rest, which is
 * what makes a tap move one row and a hold move steadily.
 */
#define REPEAT_FIRST_MS 380
#define REPEAT_NEXT_MS  90

enum key { K_NONE = 0, K_UP, K_DOWN, K_SELECT, K_BACK };

static uint8_t       s_held;
static unsigned long s_since;
static bool          s_repeated;

/*
 * Forget everything the buttons have been doing.
 *
 * The menu keeps its own evdev descriptors open for as long as TinyGB runs,
 * and evdev delivers every event to every reader - so while a game is being
 * played, all of it queues up here as well. Without this, opening the pause
 * menu replays a level's worth of button presses into it and the selection
 * shoots off somewhere before anyone has touched anything.
 *
 * One poll is enough to drain the queue, because tg_input_poll reads
 * everything pending and reduces it to the current state. The Home press that
 * opened the menu is discarded with it, or the menu would immediately act on
 * the button that got here.
 */
static void flush_keys(void)
{
    if (!s_in)
        return;

    s_held = tg_input_poll(s_in);
    (void)tg_input_take_quit(s_in);
    s_since = now_ms();
    s_repeated = false;
}

static enum key read_key(void)
{
    uint8_t mask, fresh;
    unsigned long now = now_ms();

    if (!s_in)
        return K_NONE;

    if (tg_input_take_quit(s_in))
        return K_BACK;

    mask = tg_input_poll(s_in);
    fresh = (uint8_t)(mask & ~s_held);

    if (mask != s_held) {
        s_held = mask;
        s_since = now;
        s_repeated = false;
        if (fresh & TG_A)     return K_UP;
        if (fresh & TG_B)     return K_DOWN;
        if (fresh & TG_START) return K_SELECT;
        return K_NONE;
    }

    /* Still held: repeat, but only the two that move. Repeating a select
       would choose the same row over and over. */
    if (s_held & (TG_A | TG_B)) {
        unsigned long wait = s_repeated ? REPEAT_NEXT_MS : REPEAT_FIRST_MS;

        if (now - s_since >= wait) {
            s_since = now;
            s_repeated = true;
            return (s_held & TG_A) ? K_UP : K_DOWN;
        }
    }
    return K_NONE;
}

/* ---- the loop ------------------------------------------------------------ */

static enum tg_menu_action choose(tg_menu_state *st, bool *done)
{
    int sel = s_sel[s_page];

    switch (s_page) {
    case P_ROMS:
        if (!s_lib_ok || (unsigned)sel >= s_lib.n) break;
        snprintf(st->rom_path, sizeof st->rom_path, "%s/%s",
                 s_lib.dir, s_lib.name[sel]);
        *done = true;
        return TG_MENU_PLAY;

    case P_PAUSE:
        switch (sel) {
        case PAUSE_RESUME: *done = true; return TG_MENU_RESUME;
        case PAUSE_SAVE:
            if (!st->can_state) break;
            *done = true;
            return TG_MENU_SAVE_STATE;
        case PAUSE_LOAD:
            if (!st->can_state || !st->have_state) break;
            *done = true;
            return TG_MENU_LOAD_STATE;
        case PAUSE_RESET: *done = true; return TG_MENU_RESET;
        case PAUSE_SETTINGS:
            s_page = P_SETTINGS;
            break;
        case PAUSE_LIBRARY:
            /* Re-read: cartridges can have appeared since startup, and this
               is the moment someone is looking for one. */
            tg_roms_free(&s_lib);
            s_lib_ok = tg_roms_scan(&s_lib);
            s_page = P_ROMS;
            break;
        case PAUSE_QUIT: *done = true; return TG_MENU_QUIT;
        }
        break;

    case P_SETTINGS:
        switch (sel) {
        case SET_PALETTE: s_page = P_PALETTE; break;
        case SET_SMOOTH:  st->smooth = !st->smooth; break;
        case SET_TILT:    st->tilt = !st->tilt; break;
        }
        break;

    case P_PALETTE:
        if ((unsigned)sel < tg_palette_count())
            st->palette = (unsigned)sel;
        break;
    }

    return TG_MENU_RESUME;
}

/* Home: back a page, and out from the top one. */
static enum tg_menu_action go_back(const tg_menu_state *st, bool *done)
{
    switch (s_page) {
    case P_PALETTE:
        s_page = P_SETTINGS;
        return TG_MENU_RESUME;
    case P_SETTINGS:
        s_page = st->have_game ? P_PAUSE : P_ROMS;
        return TG_MENU_RESUME;
    case P_PAUSE:
        /* Home on the pause menu is the way back into the game, which is
           what it did to get here. */
        *done = true;
        return TG_MENU_RESUME;
    case P_ROMS:
        *done = true;
        /* Nothing to go back to. If a game is loaded this is a return to it,
           otherwise it is the way out of the app. */
        return st->have_game ? TG_MENU_RESUME : TG_MENU_QUIT;
    }
    return TG_MENU_QUIT;
}

enum tg_menu_action tg_menu_run(tg_menu_state *st, bool pause)
{
    enum tg_menu_action act = TG_MENU_QUIT;
    bool done = false;

    s_page = pause ? P_PAUSE : P_ROMS;
    if (s_page == P_PAUSE)
        s_sel[P_PAUSE] = PAUSE_RESUME;

    /* A fresh scan whenever the picker is opened cold. */
    if (!pause && !s_lib_ok)
        s_lib_ok = tg_roms_scan(&s_lib);

    /* Whatever the game did with these buttons is not a menu instruction. */
    flush_keys();

    tg_menu_resume();
    draw(st);

    while (!done) {
        enum key k = read_key();
        int count;

        if (k != K_NONE) {
            count = page_count(st);
            switch (k) {
            case K_UP:
                if (s_sel[s_page] > 0) s_sel[s_page]--;
                break;
            case K_DOWN:
                if (s_sel[s_page] < count - 1) s_sel[s_page]++;
                break;
            case K_SELECT:
                act = choose(st, &done);
                break;
            case K_BACK:
                act = go_back(st, &done);
                break;
            default:
                break;
            }
            if (!done)
                draw(st);
        }

        lv_timer_handler();
        usleep(16000);
    }

    return act;
}

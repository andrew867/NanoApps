/*
 * tinygb.c — the RetailOS front end, as a NanoApps raw-surface app.
 *
 * The emulator is core/tg_core.h; everything below it is portable and tested
 * on a desktop. This file is what RetailOS needs around it, and the shape of
 * it is dictated by the raw surface (docs/SPEC-retailos-frontend.md):
 *
 *   - the OS calls hb_raw_init once and hb_raw_frame on a ~60 Hz heartbeat,
 *     and blits the whole surface after every tick. There is no exit
 *     callback: Home ends the app. So everything is a mode inside the tick,
 *     and anything that matters is on disk within a couple of seconds.
 *   - the surface is 240x432 XRGB8888 and persists between ticks, which is
 *     what lets the scaler skip rows that did not change.
 *   - there is no libc and no malloc. The cartridge lives in a static window
 *     (.bss costs nothing in the file), the machine in a static context.
 *
 * Phase 3: the library, the picture, the pad, the buttons, battery saves.
 * Silent - the audio queue and the frame pacing that hangs off it are
 * Phase 4, and until then one emulated frame runs per tick. The menu pages
 * are Phase 5; the pill under the picture returns to the library for now.
 */

#include "hb_sdk.h"
#include "hb_raw_surface.h"
#include "hb_heap.h"
#include "hb_prefs.h"

#include "core/tg_core.h"
#include "platform/tg_input_hb.h"
#include "platform/tg_pad.h"
#include "platform/tg_palette.h"
#include "platform/tg_roms.h"
#include "platform/tg_save.h"
#include "platform/tg_scale.h"
#include "platform/tg_surface.h"
#include "platform/tg_sys.h"
#include "platform/tg_text.h"
#include "platform/tg_util.h"

#include <string.h>

/* ---- memory ----------------------------------------------------------------
 *
 * Static, all of it. The .hbapp carries none of .bss, so a megabyte here is
 * free on disk; what it costs is at launch, where the resident allocates
 * code + data + .bss as one block with an allocator that reboots the device
 * when it cannot. hb_raw_init logs the heap headroom to the trace ring so
 * that cost can be read back from a real device before this number is
 * final (OQ-004).
 */
#define TG_ROS_ROM_MAX   (1024u * 1024u)
#define TG_ROS_SRAM_MAX  (128u * 1024u)
#define TG_ROS_CTX_MAX   (64u * 1024u)

static uint8_t  s_rom[TG_ROS_ROM_MAX];
static uint8_t  s_sram[TG_ROS_SRAM_MAX];
static uint64_t s_ctx[TG_ROS_CTX_MAX / 8];    /* aligned for the core */

/* ---- the screen ----------------------------------------------------------- */

static tg_surface s_fb;

#define TITLE_H   44
#define ROW_H     48
#define ROWS_Y    TITLE_H
#define ROWS_VIS  ((432 - ROWS_Y) / ROW_H)    /* 8 */

/* The compositor wants 0xFF in the top byte; the palette carries it so the
   scaler never has to think about it. */
#define OPAQUE    0xFF000000u

/* As long as a library entry can be; the list itself caps names at this. */
#define NAME_MAX_LEN 96

/* ---- state ---------------------------------------------------------------- */

enum { M_LIBRARY, M_PLAYING };

static int s_mode = M_LIBRARY;

/* The library. */
static tg_rom_list s_lib;
static bool        s_lib_ok;
static int         s_sel;          /* the highlighted row */
static int         s_scroll;       /* pixels the list is scrolled by */
static bool        s_ui_dirty;     /* repaint on the next tick */

/* Touch, as edges: the OS hands over a level. */
static bool s_prev_down;
static int  s_down_x, s_down_y;    /* where this press began */
static int  s_last_y;
static int  s_moved;               /* total travel since the press */
static int  s_scroll_at_down;

/* Buttons, as edges. */
static unsigned s_prev_keys;

/* The cartridge. */
static const tg_core *s_core;
static tg_rom_info    s_info;
static size_t         s_rom_len;
static char           s_rom_path[256];
static tg_save        s_save;
static tg_scaler      s_scaler;
static tg_input_hb    s_input;
static char           s_note[96];  /* one line under the list, or empty */

/* ---- helpers -------------------------------------------------------------- */

static uint32_t col_bg(void)      { return hb_color_bg(); }
static uint32_t col_surface(void) { return hb_color_surface(); }
static uint32_t col_text(void)    { return hb_color_text(); }
static uint32_t col_dim(void)     { return hb_color_text_dim(); }
static uint32_t col_primary(void) { return hb_color_primary(); }

static void note(const char *line)
{
    tg_strlcpy(s_note, line ? line : "", sizeof s_note);
    s_ui_dirty = true;
}

static void rom_path_of(unsigned i, char *out, size_t cap)
{
    tg_strlcpy(out, s_lib.dir, cap);
    tg_strlcat(out, "/", cap);
    tg_strlcat(out, s_lib.name[i], cap);
}

/* A cartridge's name as a person would say it: no extension. */
static void display_name(const char *file, char *out, size_t cap)
{
    const char *dot = tg_strrchr(file, '.');
    size_t n = dot ? (size_t)(dot - file) : strlen(file);

    if (n >= cap) n = cap - 1;
    memcpy(out, file, n);
    out[n] = 0;
}

/* ---- the library screen --------------------------------------------------- */

static void lib_draw(void)
{
    const tg_font *f = &tg_font_ui;
    int y;

    tg_surface_rect(&s_fb, 0, 0, 240, 432, col_bg());

    /* The title bar, in the shape the nano's own screens have. */
    tg_surface_rect(&s_fb, 0, 0, 240, TITLE_H, col_surface());
    tg_text_draw(&s_fb, f, (240 - (int)tg_text_width(f, "TinyGB", 1)) / 2,
                 (TITLE_H - f->h) / 2, "TinyGB", 1, col_text());
    tg_surface_rect(&s_fb, 0, TITLE_H - 1, 240, 1, col_dim());

    if (!s_lib_ok || s_lib.n == 0) {
        const tg_font *sf = &tg_font_small;
        int ty = ROWS_Y + 24;

        tg_text_draw(&s_fb, f, 12, ty, "No cartridges yet.", 1, col_text());
        ty += f->h + 12;
        tg_text_draw(&s_fb, sf, 12, ty, "Put .gb files in", 1, col_dim());
        ty += sf->h + 4;
        tg_text_draw(&s_fb, sf, 12, ty, "/Apps/Data/TinyGB/roms", 1, col_dim());
        ty += sf->h + 4;
        tg_text_draw(&s_fb, sf, 12, ty, "in disk mode, then relaunch.", 1, col_dim());
        if (!s_lib_ok) {
            ty += sf->h + 12;
            tg_text_draw(&s_fb, sf, 12, ty, "(the folder could not be read)", 1, col_dim());
        }
    }

    y = ROWS_Y - s_scroll;
    for (unsigned i = 0; i < s_lib.n; i++, y += ROW_H) {
        char name[NAME_MAX_LEN], path[256];
        unsigned fit;
        uint32_t fg = col_text();
        long size;

        if (y + ROW_H <= ROWS_Y || y >= 432) continue;

        if ((int)i == s_sel)
            tg_surface_rect(&s_fb, 0, y, 240, ROW_H, col_primary());
        tg_surface_rect(&s_fb, 12, y + ROW_H - 1, 228, 1, col_dim());

        display_name(s_lib.name[i], name, sizeof name);

        /* A cartridge the window cannot hold is shown, greyed, with the
           reason, rather than hidden - the owner should know why. */
        rom_path_of(i, path, sizeof path);
        size = tg_file_size(path);
        if (size > (long)TG_ROS_ROM_MAX) {
            char kb[24];

            fg = col_dim();
            /* Leave room for "  1024 KB" after the name. */
            name[tg_text_fit(f, name, 240 - 24 - 9 * 10, 1)] = 0;
            tg_strlcat(name, "  ", sizeof name);
            tg_strlcat(name, tg_utoa((unsigned long)(size / 1024), kb, sizeof kb), sizeof name);
            tg_strlcat(name, " KB", sizeof name);
        }

        fit = tg_text_fit(f, name, 240 - 24, 1);
        name[fit] = 0;
        tg_text_draw(&s_fb, f, 12, y + (ROW_H - f->h) / 2, name, 1, fg);
    }

    if (s_lib.skipped) {
        char line[48], num[16];

        tg_strlcpy(line, "and ", sizeof line);
        tg_strlcat(line, tg_utoa(s_lib.skipped, num, sizeof num), sizeof line);
        tg_strlcat(line, " more not listed", sizeof line);
        tg_text_draw(&s_fb, &tg_font_small, 12, y + 8, line, 1, col_dim());
    }

    if (s_note[0]) {
        /* The last thing that went wrong, over the bottom of the list. */
        tg_surface_rect(&s_fb, 0, 432 - 22, 240, 22, col_surface());
        tg_text_draw(&s_fb, &tg_font_small, 6, 432 - 22 + 4, s_note, 1, col_text());
    }

    s_ui_dirty = false;
}

/* Keep the highlighted row on the screen. */
static void lib_scroll_to_sel(void)
{
    int top = s_sel * ROW_H;
    int max_scroll = (int)s_lib.n * ROW_H - (432 - ROWS_Y);

    if (max_scroll < 0) max_scroll = 0;
    if (top < s_scroll) s_scroll = top;
    if (top + ROW_H > s_scroll + (432 - ROWS_Y)) s_scroll = top + ROW_H - (432 - ROWS_Y);
    if (s_scroll > max_scroll) s_scroll = max_scroll;
    if (s_scroll < 0) s_scroll = 0;
}

static void start_cartridge(unsigned i);

static void lib_touch(const hb_spoint_t *t)
{
    bool down = t->down != 0;

    if (down && !s_prev_down) {
        s_down_x = t->x;
        s_down_y = s_last_y = t->y;
        (void)s_down_x;
        s_moved = 0;
        s_scroll_at_down = s_scroll;
    } else if (down && s_prev_down) {
        int dy = t->y - s_last_y;

        s_moved += dy < 0 ? -dy : dy;
        s_last_y = t->y;

        /* Past a few pixels of travel this is a drag, and the list follows
           the finger. */
        if (s_moved > 8 && s_lib.n) {
            int max_scroll = (int)s_lib.n * ROW_H - (432 - ROWS_Y);

            if (max_scroll < 0) max_scroll = 0;
            s_scroll = s_scroll_at_down - (t->y - s_down_y);
            if (s_scroll < 0) s_scroll = 0;
            if (s_scroll > max_scroll) s_scroll = max_scroll;
            s_ui_dirty = true;
        }
    } else if (!down && s_prev_down) {
        /* Release. A tap is a press that did not travel; it lands on the
           row under where the finger lifted. */
        if (s_moved <= 8 && t->y >= ROWS_Y && s_lib.n) {
            int row = (t->y - ROWS_Y + s_scroll) / ROW_H;

            if (row >= 0 && row < (int)s_lib.n) {
                s_sel = row;
                s_ui_dirty = true;
                start_cartridge((unsigned)row);
            }
        }
    }
    s_prev_down = down;
}

static void lib_keys(void)
{
    unsigned keys = 0;

    if (hb_button_pressed(HB_BTN_VOL_UP))   keys |= 1;
    if (hb_button_pressed(HB_BTN_VOL_DOWN)) keys |= 2;

    /* Edges only: the SDK reports levels and would otherwise scroll a row
       per tick for as long as the key is held. */
    if ((keys & 1) && !(s_prev_keys & 1) && s_sel > 0) {
        s_sel--; lib_scroll_to_sel(); s_ui_dirty = true;
    }
    if ((keys & 2) && !(s_prev_keys & 2) && s_sel + 1 < (int)s_lib.n) {
        s_sel++; lib_scroll_to_sel(); s_ui_dirty = true;
    }
    s_prev_keys = keys;
}

static void enter_library(void)
{
    s_mode = M_LIBRARY;
    s_lib_ok = tg_roms_scan(&s_lib);
    if (s_sel >= (int)s_lib.n) s_sel = s_lib.n ? (int)s_lib.n - 1 : 0;
    lib_scroll_to_sel();
    s_prev_down = true;      /* a finger still down from the pill is not a tap */
    s_prev_keys = 3;
    s_ui_dirty = true;
}

/* ---- the cartridge -------------------------------------------------------- */

static void stop_cartridge(void)
{
    if (!s_core) return;
    if (s_info.sram_size) tg_save_flush(&s_save);
    s_core->close(s_ctx);
    s_core = NULL;
}

static void start_cartridge(unsigned i)
{
    long size;
    enum tg_result r;
    uint32_t pal[4];
    const uint32_t *shade = tg_palette_at(0)->shade;

    stop_cartridge();

    rom_path_of(i, s_rom_path, sizeof s_rom_path);
    size = tg_file_size(s_rom_path);
    if (size < 0) { note("that cartridge could not be read"); return; }
    if (size > (long)TG_ROS_ROM_MAX) { note("too big for this build (1 MB max)"); return; }

    /* One read, the whole file. This blocks the tick for as long as it
       takes, which is why it happens here and never mid-game. */
    if (tg_file_read(s_rom_path, s_rom, sizeof s_rom) != size) {
        note("that cartridge could not be read");
        return;
    }
    s_rom_len = (size_t)size;

    if ((r = tg_rom_probe(s_rom, s_rom_len, &s_info)) != TG_OK) {
        note(tg_strerror(r));
        return;
    }
    if (!(s_core = tg_core_for_rom(s_rom, s_rom_len))) {
        note("no core can run that cartridge");
        return;
    }
    if (s_core->ctx_size > sizeof s_ctx || s_info.sram_size > sizeof s_sram) {
        note("that cartridge needs more memory than this build has");
        s_core = NULL;
        return;
    }

    /* The battery save goes in before the machine starts, because a
       cartridge reads it at boot. */
    memset(s_sram, 0, sizeof s_sram);
    if (s_info.sram_size) {
        char sav[256];

        tg_save_path_for(s_rom_path, sav, sizeof sav);
        tg_save_load(&s_save, sav, s_sram, s_info.sram_size);
    }

    /* Sound is Phase 4; audio_rate 0 asks the core for none. */
    r = s_core->open(s_ctx, s_rom, s_rom_len, s_sram, s_info.sram_size, 0);
    if (r != TG_OK) {
        note(tg_strerror(r));
        s_core = NULL;
        return;
    }

    for (unsigned k = 0; k < 4; k++) pal[k] = shade[k] | OPAQUE;
    tg_scaler_init(&s_scaler, pal, true);
    tg_input_hb_init(&s_input, false);

    tg_surface_rect(&s_fb, 0, 0, 240, 432, 0x000000u);
    tg_pad_invalidate();
    tg_pad_draw(&s_fb, 0, true);
    s_note[0] = 0;
    s_mode = M_PLAYING;
}

static void play_tick(void)
{
    unsigned held = tg_input_hb_poll(&s_input);

    if (held & TG_PAD_MENU) {
        /* Phase 5 puts the pause menu here. For now the pill is the way
           back to the shelf. */
        stop_cartridge();
        enter_library();
        return;
    }

    s_core->set_buttons(s_ctx, (uint8_t)(held & 0xFF));
    s_core->run_frame(s_ctx);

    /* The picture sits at the top, 240 wide - exactly the surface's width,
       so its stride is the surface's and its origin is pixel zero. */
    tg_scale_15(&s_scaler, s_fb.px, s_fb.stride_px, s_core->pixels(s_ctx));
    tg_pad_draw(&s_fb, held & 0xFF, false);

    if (s_info.sram_size) tg_save_tick(&s_save, tg_now_ns());
}

/* ---- the raw-surface contract --------------------------------------------- */

void hb_raw_init(int w, int h)
{
    s_fb.px        = hb_raw_fb();
    s_fb.w         = (unsigned)w;
    s_fb.h         = (unsigned)h;
    s_fb.stride_px = (unsigned)w;
    s_fb.or_mask   = OPAQUE;

    /* The surface is shared between apps and holds whatever the last one
       left there; paint all of it before the first tick returns. */
    tg_surface_rect(&s_fb, 0, 0, s_fb.w, s_fb.h, 0x000000u);

    /* What the OS can spare, for the trace ring: the number OQ-004 is
       decided by, read back with `start trace` from the NanoApps tree. */
    hb_trace_init();
    hb_trace_log("TGHP", hb_os_heap_largest(), hb_os_heap_free());

    s_note[0] = 0;
    enter_library();
    lib_draw();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    switch (s_mode) {
    case M_LIBRARY:
        lib_touch(touch);
        lib_keys();
        if (s_ui_dirty) lib_draw();
        break;
    case M_PLAYING:
        play_tick();
        break;
    }
}

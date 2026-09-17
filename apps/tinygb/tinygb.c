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
 * Two modes. MENU is the library, the pause pages and the settings, all in
 * ui/tg_menu_raw.c, which is told where the finger is and answers with what
 * was chosen. PLAYING runs the Game Boy: sound is a chain of 60 ms blocks the
 * OS mixer plays (platform/tg_audq.h), and the depth of that chain is the
 * clock - a tick runs no emulated frame, one, or two, as many as the chain
 * is short by. Home is the only way out; the pill under the pad is the way
 * to the pause menu.
 */

#include "hb_sdk.h"
#include "hb_raw_surface.h"
#include "hb_heap.h"
#include "hb_prefs.h"

#include "core/tg_core.h"
#include "linux/shim/build_stamp.h"
#include "platform/tg_audio.h"
#include "platform/tg_audio_hb.h"
#include "platform/tg_audq.h"
#include "platform/tg_input_hb.h"
#include "platform/tg_pad.h"
#include "platform/tg_palette.h"
#include "platform/tg_roms.h"
#include "platform/tg_save.h"
#include "platform/tg_scale.h"
#include "platform/tg_settings.h"
#include "platform/tg_surface.h"
#include "platform/tg_sys.h"
#include "platform/tg_text.h"
#include "platform/tg_util.h"
#include "ui/tg_menu_raw.h"

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
#define TG_ROS_ROM_MAX    (1024u * 1024u)
#define TG_ROS_SRAM_MAX   (128u * 1024u)
#define TG_ROS_CTX_MAX    (64u * 1024u)
#define TG_ROS_STATE_MAX  (160u * 1024u)   /* context + cart RAM + a header */

static uint8_t  s_rom[TG_ROS_ROM_MAX];
static uint8_t  s_sram[TG_ROS_SRAM_MAX];
static uint64_t s_ctx[TG_ROS_CTX_MAX / 8];    /* aligned for the core */
static uint8_t  s_state[TG_ROS_STATE_MAX];

/* The sound buffers. Static rather than hb_os_alloc, and never given back:
   a voice may still be reading one when Home ends the app, and this arena
   outlives it by the seconds it takes to launch anything again. 63 KB. */
static int16_t  s_aud_pool[TG_AUDQ_POOL_SAMPLES];

/* Comfortable rather than loud: 0x7fff is the OS's full scale. */
#define TG_AUD_VOLUME 0x4800u

/* The most one emulated frame can produce at 22050 (369 or 370), with room. */
#define ABUF_FRAMES 512

/* ---- the screen ----------------------------------------------------------- */

static tg_surface s_fb;

/* The compositor wants 0xFF in the top byte; the palette carries it so the
   scaler never has to think about it. */
#define OPAQUE    0xFF000000u

#define DATA_DIR      "/Apps/Data/TinyGB"
#define SETTINGS_PATH DATA_DIR "/settings.txt"

/* ---- state ---------------------------------------------------------------- */

enum { M_MENU, M_PLAYING };

static int s_mode = M_MENU;

/* The library, the menu, the settings. */
static tg_rom_list   s_lib;
static bool          s_lib_ok;
static tg_menu_state s_mst;
static tg_menu_raw   s_menu;
static tg_settings   s_settings;

/* The cartridge. */
static const tg_core *s_core;
static tg_rom_info    s_info;
static size_t         s_rom_len;
static char           s_rom_path[256];
static tg_save        s_save;
static tg_scaler      s_scaler;
static tg_input_hb    s_input;

/* Sound. */
static tg_audq        s_audq;
static tg_audio_clock s_aclock;
static int16_t        s_abuf[ABUF_FRAMES * 2];
static bool           s_sound;             /* this cartridge has a sink */
static uint32_t       s_media_check_ms;

/* Keeping the panel lit, and the counters. */
static uint32_t       s_wake_ms;
static uint32_t       s_fps_ms;
static unsigned       s_frames_run, s_fps;

#ifdef TG_TONE
/* A 440 Hz sine at 40 % of full scale, in place of the Game Boy, for
   proving the chain with a recording: a gap, a click or a pitch step is
   plain to see in a waveform of a tone and buried in a game's. */
static const int16_t k_sine[64] = {
    0, 1285, 2557, 3805, 5016, 6179, 7282, 8315, 9268, 10132, 10898, 11559,
    12109, 12542, 12855, 13044, 13107, 13044, 12855, 12542, 12109, 11559,
    10898, 10132, 9268, 8315, 7282, 6179, 5016, 3805, 2557, 1285, 0, -1285,
    -2557, -3805, -5016, -6179, -7282, -8315, -9268, -10132, -10898, -11559,
    -12109, -12542, -12855, -13044, -13107, -13044, -12855, -12542, -12109,
    -11559, -10898, -10132, -9268, -8315, -7282, -6179, -5016, -3805, -2557,
    -1285,
};
static uint32_t s_tone_phase;              /* 26.6 fixed: 64 entries */
#define TONE_STEP ((440u << 16) * 64u / TG_AUDQ_RATE)
#endif

/* ---- helpers -------------------------------------------------------------- */

static void note(const char *line)
{
    tg_menu_raw_note(&s_menu, line);
}

static long rom_size_cb(unsigned i, void *user)
{
    char path[256];

    (void)user;
    tg_strlcpy(path, s_lib.dir, sizeof path);
    tg_strlcat(path, "/", sizeof path);
    tg_strlcat(path, s_lib.name[i], sizeof path);
    return tg_file_size(path);
}

static void state_path_for(const char *rom_path, char *out, size_t cap)
{
    tg_save_path_for(rom_path, out, cap);        /* "<rom>.sav" */
    tg_strlcpy(out + strlen(out) - 4, ".st0", 5);
}

/* The theme the rest of the device is wearing: NanoApps' semantic colours
   follow the device colour and the light/dark setting. */
static void load_theme(tg_menu_theme *th)
{
    th->bg         = hb_color_bg();
    th->surface    = hb_color_surface();
    th->text       = hb_color_text();
    th->dim        = hb_color_text_dim();
    th->primary    = hb_color_primary();
    th->on_primary = hb_color_on_primary();
}

/*
 * Keep the panel lit while a game runs. The raw runtime has no wake lock;
 * this is what the LVGL runtime's does under the hood - tell the OS's
 * power/idle singleton that a touch happened - every ten seconds, which is
 * well under the dim timeout and nothing per frame.
 */
#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define HB_KEVENT_TOUCHACTIVITY 4

static void wake_poke(void)
{
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();

    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, HB_KEVENT_TOUCHACTIVITY);
}

/* RetailOS acts on the Play/Pause key before this app sees it and starts
   the Music player underneath. It cannot be stopped from doing that, but it
   can be undone: if the OS player has started, pause it. Entrain's trick. */
static void suppress_os_media(void)
{
    uint32_t now = hb_time_uptime_ms();

    if (now - s_media_check_ms < 250) return;   /* 4 Hz is plenty */
    s_media_check_ms = now;
    if (hb_media_state() == 0) hb_media_set_paused(true);
}

/* ---- settings ------------------------------------------------------------- */

static void settings_to_menu(void)
{
    s_mst.palette = tg_palette_index(s_settings.palette);
    s_mst.smooth  = s_settings.smooth;
    s_mst.tilt    = s_settings.tilt;
    s_mst.overlay = s_settings.overlay;
}

static void settings_from_menu(void)
{
    tg_strlcpy(s_settings.palette, tg_palette_at(s_mst.palette)->name, sizeof s_settings.palette);
    s_settings.smooth  = s_mst.smooth;
    s_settings.tilt    = s_mst.tilt;
    s_settings.overlay = s_mst.overlay;
    tg_settings_save(&s_settings, SETTINGS_PATH);
}

/* The picture and the pad, for the current settings. */
static void apply_video_settings(void)
{
    uint32_t pal[4];
    const uint32_t *shade = tg_palette_at(s_mst.palette)->shade;

    for (unsigned k = 0; k < 4; k++) pal[k] = shade[k] | OPAQUE;
    tg_scaler_init(&s_scaler, pal, s_mst.smooth);
    s_input.tilt_on = s_mst.tilt;
}

/* ---- the library ---------------------------------------------------------- */

static void enter_library(void)
{
    s_mode = M_MENU;
    s_lib_ok = tg_roms_scan(&s_lib);
    tg_menu_raw_set_library(&s_menu, &s_lib, s_lib_ok);

    /* A Resume row for the last cartridge, when it is still there and has a
       state to come back to. */
    s_mst.have_game = false;
    s_mst.have_state = false;
    s_mst.can_state = false;
    s_mst.rom_path[0] = 0;
    s_mst.rom_title[0] = 0;
    if (s_settings.last_rom[0]) {
        char st[256];

        tg_strlcpy(s_mst.rom_path, s_lib.dir, sizeof s_mst.rom_path);
        tg_strlcat(s_mst.rom_path, "/", sizeof s_mst.rom_path);
        tg_strlcat(s_mst.rom_path, s_settings.last_rom, sizeof s_mst.rom_path);
        state_path_for(s_mst.rom_path, st, sizeof st);
        if (tg_file_exists(s_mst.rom_path) && tg_file_exists(st)) {
            const char *dot = tg_strrchr(s_settings.last_rom, '.');
            size_t n = dot ? (size_t)(dot - s_settings.last_rom) : strlen(s_settings.last_rom);

            if (n >= sizeof s_mst.rom_title) n = sizeof s_mst.rom_title - 1;
            memcpy(s_mst.rom_title, s_settings.last_rom, n);
            s_mst.rom_title[n] = 0;
            s_mst.have_state = true;
        } else {
            s_mst.rom_path[0] = 0;
        }
    }

    tg_menu_raw_open(&s_menu, false);
}

/* ---- states --------------------------------------------------------------- */

static bool save_state(void)
{
    char path[256];
    size_t need;

    if (!s_core || !s_core->state_save) { note("this core has no save states"); return false; }
    if (s_info.sram_size) tg_save_flush(&s_save);   /* never a state beside a stale .sav */

    need = s_core->state_size(s_ctx);
    if (need > sizeof s_state) { note("state too large for this build"); return false; }
    if (s_core->state_save(s_ctx, s_state, sizeof s_state) != TG_OK) {
        note("the core would not save its state");
        return false;
    }
    state_path_for(s_rom_path, path, sizeof path);
    if (!tg_file_write(path, s_state, need)) { note("could not write the state"); return false; }
    s_mst.have_state = true;
    return true;
}

static bool load_state(void)
{
    char path[256];
    long got;
    enum tg_result r;

    if (!s_core || !s_core->state_load) return false;
    state_path_for(s_rom_path, path, sizeof path);
    got = tg_file_read(path, s_state, sizeof s_state);
    if (got <= 0) { note("no state to load"); return false; }
    if ((r = s_core->state_load(s_ctx, s_state, (size_t)got)) != TG_OK) {
        note(tg_strerror(r));
        return false;
    }
    return true;
}

/* ---- the cartridge -------------------------------------------------------- */

/* One emulated frame's worth of sound, into the chain. */
static void emit_audio(void)
{
    unsigned n = tg_audio_clock_next(&s_aclock);
    unsigned got;

    if (n > ABUF_FRAMES) n = ABUF_FRAMES;

#ifdef TG_TONE
    for (unsigned i = 0; i < n; i++) {
        int16_t v = k_sine[(s_tone_phase >> 16) & 63];

        s_abuf[2 * i] = s_abuf[2 * i + 1] = v;
        s_tone_phase += TONE_STEP;
    }
    got = n;
#else
    got = s_core->audio_pull(s_ctx, s_abuf, n);
#endif
    /* A short fill is padded with silence, never with a repeat. */
    for (unsigned i = got * 2; i < n * 2; i++) s_abuf[i] = 0;
    tg_audq_push(&s_audq, s_abuf, n);
}

static void stop_cartridge(void)
{
    if (!s_core) return;
    if (s_sound) tg_audq_stop(&s_audq);
    if (s_info.sram_size) tg_save_flush(&s_save);
    s_core->close(s_ctx);
    s_core = NULL;
    s_sound = false;
}

/* From the menu into the game: a full repaint, because the menu drew over
   everything, and the sound picks up where the silence was looping. */
static void resume_play(void)
{
    tg_surface_rect(&s_fb, 0, 0, 240, 432, 0x000000u);
    tg_scaler_invalidate(&s_scaler);
    tg_pad_invalidate();
    tg_pad_draw(&s_fb, 0, true);
    if (s_sound) tg_audq_resume(&s_audq);
    s_input.calibrated = false;            /* level is wherever it is held now */
    s_input.calib_until_ms = hb_time_uptime_ms() + 250;
    s_input.sx = s_input.sy = s_input.sz = 0;
    s_input.samples = 0;
    wake_poke();
    s_wake_ms = hb_time_uptime_ms();
    s_mode = M_PLAYING;
}

static void start_cartridge(const char *path, const char *title, bool with_state)
{
    long size;
    enum tg_result r;
    const char *slash;

    stop_cartridge();

    tg_strlcpy(s_rom_path, path, sizeof s_rom_path);
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

    r = s_core->open(s_ctx, s_rom, s_rom_len, s_sram, s_info.sram_size,
                     TG_AUDQ_RATE);
    if (r != TG_OK) {
        note(tg_strerror(r));
        s_core = NULL;
        return;
    }

    /* The queue itself was set up once at launch and is still holding the
       voice (looping silence) from the last cartridge, if there was one;
       the first block of this one chains from that. Only the clock resets. */
    s_sound = s_core->audio_pull != NULL;
#ifdef TG_TONE
    s_sound = true;
#endif
    tg_audio_clock_init(&s_aclock, TG_AUDQ_RATE);

    /* What the pause page shows and offers. */
    tg_strlcpy(s_mst.rom_path, s_rom_path, sizeof s_mst.rom_path);
    tg_strlcpy(s_mst.rom_title, title, sizeof s_mst.rom_title);
    s_mst.have_game = true;
    s_mst.can_state = s_core->state_size && s_core->state_save && s_core->state_load;
    {
        char st[256];

        state_path_for(s_rom_path, st, sizeof st);
        s_mst.have_state = s_mst.can_state && tg_file_exists(st);
    }

    /* Remember it for the Resume row, by its file name within the shelf. */
    slash = tg_strrchr(s_rom_path, '/');
    tg_strlcpy(s_settings.last_rom, slash ? slash + 1 : s_rom_path, sizeof s_settings.last_rom);
    tg_settings_save(&s_settings, SETTINGS_PATH);

    tg_input_hb_init(&s_input, s_mst.tilt);
    apply_video_settings();

    if (with_state && s_mst.have_state) load_state();

    s_frames_run = 0;
    s_fps = 0;
    s_fps_ms = hb_time_uptime_ms();
    resume_play();
}

/* The pill: into the pause menu. The battery save is flushed and a state
   written, so a press of Home from the menu loses nothing (REQ-DATA-023). */
static void pause_game(void)
{
    if (s_sound) tg_audq_pause(&s_audq);
    if (s_info.sram_size) tg_save_flush(&s_save);
    if (s_mst.can_state) save_state();
    tg_menu_raw_open(&s_menu, true);
    s_mode = M_MENU;
}

/* fps and the sound queue, in the strip beside the pill, when asked. */
static void draw_overlay(void)
{
    char line[40], num[16];
    const tg_font *f = &tg_font_small;

    tg_strlcpy(line, "fps ", sizeof line);
    tg_strlcat(line, tg_utoa(s_fps, num, sizeof num), sizeof line);
    tg_strlcat(line, " q", sizeof line);
    tg_strlcat(line, tg_utoa(tg_audq_queued_frames(&s_audq) / 369u, num, sizeof num), sizeof line);
    tg_surface_rect(&s_fb, 2, 218, 94, f->h + 2, 0x000000u);
    tg_text_draw(&s_fb, f, 3, 219, line, 1, 0x8B92A0u);

    tg_strlcpy(line, "u", sizeof line);
    tg_strlcat(line, tg_utoa(s_audq.underruns, num, sizeof num), sizeof line);
    tg_strlcat(line, " r", sizeof line);
    tg_strlcat(line, tg_utoa(s_audq.restarts, num, sizeof num), sizeof line);
    tg_strlcat(line, " d", sizeof line);
    tg_strlcat(line, tg_utoa(s_audq.dropped_frames, num, sizeof num), sizeof line);
    tg_surface_rect(&s_fb, 144, 218, 94, f->h + 2, 0x000000u);
    tg_text_draw(&s_fb, f, 145, 219, line, 1, 0x8B92A0u);
}

static void play_tick(void)
{
    unsigned held;
    uint32_t now = hb_time_uptime_ms();

    tg_audq_tick(&s_audq);
    suppress_os_media();

    /* Every ten seconds, the poke that keeps the backlight up. */
    if (now - s_wake_ms >= 10000) { wake_poke(); s_wake_ms = now; }

    held = tg_input_hb_poll(&s_input);
    if (held & TG_PAD_MENU) {
        pause_game();
        return;
    }

    s_core->set_buttons(s_ctx, (uint8_t)(held & 0xFF));

    /*
     * The chain is the clock. Run as many emulated frames as it is short
     * by, up to two, so pitch is the mixer's and never ours; a tick that
     * runs none repeats the picture, which nobody sees, and one that runs
     * two catches up a stall. Without a sink there is nothing to pace
     * against and the tick itself is the clock.
     */
    if (!s_sound) {
        s_core->run_frame(s_ctx);
        s_frames_run++;
    } else {
        for (int runs = 0; runs < 2 && tg_audq_frames_wanted(&s_audq) > 0; runs++) {
            s_core->run_frame(s_ctx);
            emit_audio();
            s_frames_run++;
        }
    }

    /* The picture sits at the top, 240 wide - exactly the surface's width,
       so its stride is the surface's and its origin is pixel zero. Rows that
       did not change - all of them, on a tick that ran nothing - are
       skipped. */
    tg_scale_15(&s_scaler, s_fb.px, s_fb.stride_px, s_core->pixels(s_ctx));
    tg_pad_draw(&s_fb, held & 0xFF, false);

    if (now - s_fps_ms >= 1000) {
        s_fps = s_frames_run;
        s_frames_run = 0;
        s_fps_ms = now;
    }
    if (s_mst.overlay) draw_overlay();

    if (s_info.sram_size) tg_save_tick(&s_save, tg_now_ns());
}

/* ---- the menu ------------------------------------------------------------- */

static void menu_tick(const hb_spoint_t *touch)
{
    unsigned keys = 0;
    int act;

    if (hb_button_pressed(HB_BTN_VOL_UP))   keys |= 1;
    if (hb_button_pressed(HB_BTN_VOL_DOWN)) keys |= 2;

    act = tg_menu_raw_tick(&s_menu, &s_fb, touch->x, touch->y, touch->down != 0,
                           keys, hb_time_uptime_ms());

    switch (act) {
    case TG_MENU_PLAY:
        start_cartridge(s_mst.rom_path, s_mst.rom_title, false);
        break;
    case TG_MENU_RAW_RESUME_STATE:
        start_cartridge(s_mst.rom_path, s_mst.rom_title, true);
        break;
    case TG_MENU_RESUME:
        if (s_core) resume_play(); else enter_library();
        break;
    case TG_MENU_SAVE_STATE:
        if (save_state()) note("state saved");
        break;
    case TG_MENU_LOAD_STATE:
        if (load_state()) resume_play();
        break;
    case TG_MENU_RESET:
        if (s_core) { s_core->reset(s_ctx); resume_play(); }
        break;
    case TG_MENU_RAW_CHOOSE:
        stop_cartridge();
        enter_library();
        break;
    case TG_MENU_RAW_SETTINGS_CHANGED:
        settings_from_menu();
        if (s_core) apply_video_settings();
        break;
    default:
        break;
    }
}

/* ---- the raw-surface contract --------------------------------------------- */

void hb_raw_init(int w, int h)
{
    tg_menu_theme th;

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

    /* Once, for the life of the app: the queue keeps the voice it starts,
       looping silence between cartridges, so nothing is ever started twice. */
    tg_audq_init(&s_audq, tg_audio_hb_ops(), s_aud_pool, TG_AUD_VOLUME);

    hb_fs_mkdir(DATA_DIR);
    tg_settings_load(&s_settings, SETTINGS_PATH, false);

    load_theme(&th);
    memset(&s_mst, 0, sizeof s_mst);
    settings_to_menu();
    tg_menu_raw_init(&s_menu, &s_mst, &th);
    s_menu.rom_size   = rom_size_cb;
    s_menu.rom_max    = (long)TG_ROS_ROM_MAX;
    s_menu.build      = en_build_version();
    s_menu.core_name  = tg_core_count() ? tg_core_at(0)->name : "-";
    s_menu.recent_log = tg_log_recent;

    enter_library();
    tg_menu_raw_tick(&s_menu, &s_fb, 0, 0, false, 0, hb_time_uptime_ms());
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    switch (s_mode) {
    case M_MENU:    menu_tick(touch); break;
    case M_PLAYING: play_tick();      break;
    }
}

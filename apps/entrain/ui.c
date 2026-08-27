/*
 * ui.c — every screen, shared by the device and the host build.
 *
 * Design rules this file follows, and the reasons they are rules:
 *
 *   240 px wide, finger only. Every tappable thing is a full-width row at
 *   least 44 px tall. There is no room for a toolbar of small buttons and no
 *   pointer precise enough to use one, so the primary control on Now Playing
 *   is the 180 px ring itself.
 *
 *   Flat fills and one-pixel hairlines. No shadows, no blur, no large
 *   gradients — the device software-renders every pixel and those are the
 *   effects that cost the most for the least.
 *
 *   Numeric readouts sit in fixed-width containers. LVGL's Montserrat is
 *   proportional, so a beat counting 9.99 -> 10.00 would otherwise shift the
 *   whole block sideways.
 *
 *   The ring pulses; nothing else does. A full-screen element flashing between
 *   8 and 30 Hz is a genuine seizure risk, so the pulse is confined to the
 *   ring, its depth is shallow, its rate is halved until it is under 4 Hz, and
 *   Reduce Motion turns it off entirely.
 */

#include "ui.h"
#include "engine.h"
#include "core/program.h"
#include "platform/audio.h"
#include "platform/sys.h"

#include "lvgl/lvgl.h"

#include <string.h>

/* ---- palette ------------------------------------------------------------- */

#define C_BG        0x07080C
#define C_SURFACE   0x0E1017
#define C_SURFACE_2 0x141824
#define C_HAIRLINE  0x1E2430
#define C_TEXT      0xE8EAF0
#define C_TEXT_DIM  0x8A8F98
#define C_TEXT_MUTE 0x565C68
/* The identity accent, matching the Home Screen icon. Band accents override it
   wherever the band is what the element means; this is what LVGL's own theme
   uses for the widgets we do not paint by hand, so a switch reads as part of
   the same app rather than as LVGL's default blue. */
#define C_ACCENT    0x8B5CF6

/* Type scale. The brief asks for 34/22/16/13; the LVGL config the device
   already ships carries 36/20/16/14, and adding two more Montserrat faces
   would grow every LVGL app in the repo, not just this one. The nearest
   available sizes are used instead. */
#define F_HERO    (&lv_font_montserrat_36)
#define F_TITLE   (&lv_font_montserrat_20)
#define F_BODY    (&lv_font_montserrat_16)
#define F_CAPTION (&lv_font_montserrat_14)

#define MARGIN     12
#define CONTENT_W  (EN_SCREEN_W - 2 * MARGIN)
#define ROW_H      60
#define TAP_MIN    44
#define ANIM_MS    180        /* every transition <= 200 ms */

#define RING_R     90         /* 180 px across */
#define RING_CX    (EN_SCREEN_W / 2)
#define RING_CY    150

/* Pulse: shallow by design. 12% opacity swing and 3 px of radius on a 180 px
   ring is a breath, not a strobe. */
#define PULSE_OPA_MIN  200
#define PULSE_OPA_MAX  255
#define PULSE_R_SWING  3
#define PULSE_MAX_HZ   4.0
/* The halo sits outside the progress arc. At the same radius the two would be
   the same colour in the same place, and the progress fill would be invisible. */
#define PULSE_R        (RING_R + 9)

/* ---- persisted settings -------------------------------------------------- */

typedef struct {
    uint32_t magic;
    int      volume;           /* 0..100 */
    int      volume_cap;       /* 0..100, the ceiling the slider allows */
    int      blank_delay_s;    /* 0 = never blank */
    int      reduce_motion;
    int      blank_while_playing;
    int      sleep_timer_s;
    int      last_source;      /* en_source_kind_t */
    int      last_index;
    int      seen_first_run;
} en_prefs_t;

#define EN_PREFS_MAGIC 0x4E455231u   /* 'NER1' */

static en_prefs_t P = {
    EN_PREFS_MAGIC, 45, 80, 30, 0, 1, 0, 0, 0, 0
};

/* ---- state --------------------------------------------------------------- */

static lv_obj_t *s_screen[EN_SCREEN_COUNT];
static en_screen_t s_current = EN_SCREEN_LIBRARY;

/* library */
static lv_obj_t *s_tab_btn[3];
static lv_obj_t *s_list;
static lv_obj_t *s_battery_lbl;
static int       s_tab;          /* 0 presets, 1 programs, 2 custom */

/* now playing */
static lv_obj_t *s_ring;         /* progress arc */
static lv_obj_t *s_pulse;        /* the breathing circle behind it */
static lv_obj_t *s_beat_lbl, *s_hz_lbl, *s_band_lbl, *s_carrier_lbl;
static lv_obj_t *s_state_lbl, *s_time_lbl, *s_title_lbl;
static lv_obj_t *s_timeline, *s_timeline_fill;
static lv_obj_t *s_seg_tick[EN_USER_MAX_SEGS];
static lv_obj_t *s_prep_lbl;
static lv_obj_t *s_mode_lbl;

/* live tune */
static lv_obj_t *s_tune_beat, *s_tune_carrier, *s_tune_state, *s_tune_ring;
static lv_point_t s_drag_last;
static bool       s_dragging;
static uint32_t   s_last_tap_ms;

/* timer + settings */
static lv_obj_t *s_timer_rows[6];
static lv_obj_t *s_sleep_lbl;
static lv_obj_t *s_vol_slider, *s_vol_lbl;
static lv_obj_t *s_blank_lbl;
static lv_obj_t *s_motion_sw, *s_blankplay_sw;

/* Which program the next-program key will start. Begins at -1 so the first
   press plays program 0 rather than program 1. */
static int s_prog_cycle = -1;

/* screen blanking */
static uint32_t s_last_touch_ms;
static bool     s_blanked;
static bool     s_blanking_allowed = true;

static const uint32_t SLEEP_CHOICES[] = { 0, 15 * 60, 30 * 60, 45 * 60,
                                          60 * 60, 90 * 60 };
static const char *SLEEP_LABELS[] = { "Off", "15 min", "30 min", "45 min",
                                      "60 min", "90 min" };

/* ---- formatting (no printf: LVGL's builtin sprintf has no %f) ------------- */

static int put_uint(char *b, int at, uint32_t v, int pad)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n < pad) tmp[n++] = '0';
    while (n) b[at++] = tmp[--n];
    b[at] = 0;
    return at;
}

/* Fixed decimals, so a readout never changes width by gaining a digit after
   the point. */
static void fmt_fixed(char *b, double v, int decimals)
{
    int neg = v < 0.0;
    if (neg) v = -v;
    uint32_t scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    uint32_t whole = (uint32_t)v;
    uint32_t frac = (uint32_t)((v - (double)whole) * (double)scale + 0.5);
    if (frac >= scale) { whole++; frac -= scale; }

    int at = 0;
    if (neg) b[at++] = '-';
    at = put_uint(b, at, whole, 1);
    if (decimals > 0) {
        b[at++] = '.';
        at = put_uint(b, at, frac, decimals);
    }
    b[at] = 0;
}

static void fmt_time(char *b, uint32_t seconds)
{
    uint32_t m = seconds / 60u, s = seconds % 60u;
    int at = 0;
    if (m >= 60) {
        at = put_uint(b, at, m / 60u, 1);
        b[at++] = ':';
        at = put_uint(b, at, m % 60u, 2);
    } else {
        at = put_uint(b, at, m, 1);
    }
    b[at++] = ':';
    put_uint(b, at, s, 2);
}

static void str_cat(char *dst, const char *src, int cap)
{
    int n = 0;
    while (dst[n] && n < cap - 1) n++;
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
}

/* ---- style helpers ------------------------------------------------------- */

static void flat(lv_obj_t *o, uint32_t bg)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* Body copy. An LVGL label with no width set does not wrap — it runs off the
   right edge — so every multi-line block goes through here. */
static lv_obj_t *make_para(lv_obj_t *parent, const char *text,
                           const lv_font_t *font, uint32_t color, int width)
{
    lv_obj_t *l = make_label(parent, text, font, color);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* A one-pixel rule. Hairlines instead of borders keeps the whole UI flat and
   costs one filled rect. */
static lv_obj_t *make_hairline(lv_obj_t *parent, int y, int x, int w)
{
    lv_obj_t *h = lv_obj_create(parent);
    flat(h, C_HAIRLINE);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(h, w, 1);
    lv_obj_set_pos(h, x, y);
    return h;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    flat(s, C_BG);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

/* ---- navigation ---------------------------------------------------------- */

const char *en_ui_screen_name(en_screen_t s)
{
    switch (s) {
    case EN_SCREEN_LIBRARY:  return "library";
    case EN_SCREEN_NOW:      return "now-playing";
    case EN_SCREEN_TUNE:     return "live-tune";
    case EN_SCREEN_TIMER:    return "timer";
    case EN_SCREEN_SETTINGS: return "settings";
    case EN_SCREEN_FIRSTRUN: return "first-run";
    default:                 return "?";
    }
}

en_screen_t en_ui_current(void) { return s_current; }

static void refresh_now(void);
static void refresh_library(void);
static void refresh_settings(void);
static void refresh_timer(void);

void en_ui_goto(en_screen_t screen, bool animate)
{
    if (screen < 0 || screen >= EN_SCREEN_COUNT || !s_screen[screen]) return;

    if (screen == EN_SCREEN_NOW)      refresh_now();
    if (screen == EN_SCREEN_LIBRARY)  refresh_library();
    if (screen == EN_SCREEN_SETTINGS) refresh_settings();
    if (screen == EN_SCREEN_TIMER)    refresh_timer();

    /* Vertical moves for the play stack, so the gesture and the motion agree:
       Now Playing lives below the Library, Live Tune below Now Playing. */
    lv_screen_load_anim_t anim = LV_SCR_LOAD_ANIM_NONE;
    if (animate) {
        if (screen > s_current) anim = LV_SCR_LOAD_ANIM_MOVE_TOP;
        else if (screen < s_current) anim = LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
    }
    lv_screen_load_anim(s_screen[screen], anim, animate ? ANIM_MS : 0, 0, false);
    s_current = screen;
}

/* ---- screen blanking ----------------------------------------------------- */

/* Blanking kills the backlight and nothing else. On the device, stopping the
   render loop can take the audio subsystem down with it, so "blank" must never
   mean "stop drawing" — see AUDIO_NOTES.md. */
static void wake_screen(void)
{
    s_last_touch_ms = en_sys_millis();
    if (s_blanked) {
        s_blanked = false;
        en_sys_backlight(100);
    }
}

static void blank_tick(void)
{
    if (!s_blanking_allowed || !P.blank_while_playing || P.blank_delay_s <= 0) {
        if (s_blanked) wake_screen();
        return;
    }
    if (!en_engine_is_playing()) {
        if (s_blanked) wake_screen();
        return;
    }
    uint32_t idle = (en_sys_millis() - s_last_touch_ms) / 1000u;
    if (!s_blanked && idle >= (uint32_t)P.blank_delay_s) {
        s_blanked = true;
        en_sys_backlight(0);
    }
}

/* ---- prefs --------------------------------------------------------------- */

static void prefs_save(void)
{
    en_sys_prefs_save(&P, sizeof P);
}

static void prefs_load(void)
{
    en_prefs_t tmp;
    if (en_sys_prefs_load(&tmp, sizeof tmp) && tmp.magic == EN_PREFS_MAGIC)
        P = tmp;
    en_audio_set_volume(P.volume);
}

/* ---- gestures ------------------------------------------------------------ */

typedef struct { en_screen_t up, down; } nav_t;

static void on_gesture(lv_event_t *e)
{
    const nav_t *n = (const nav_t *)lv_event_get_user_data(e);
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    wake_screen();
    if (d == LV_DIR_TOP && n->up != EN_SCREEN_COUNT)
        en_ui_goto(n->up, true);
    else if (d == LV_DIR_BOTTOM && n->down != EN_SCREEN_COUNT)
        en_ui_goto(n->down, true);
}

/* ---- library ------------------------------------------------------------- */

static void on_tab(lv_event_t *e)
{
    wake_screen();
    s_tab = (int)(lv_uintptr_t)lv_event_get_user_data(e);
    refresh_library();
}

/* Report a bad user-program file on its own row: the line number is the only
   thing that makes a hand-written file fixable. */
static lv_obj_t *s_row_error;

static void show_row_error(int index, const char *what, int line)
{
    (void)index;
    if (!s_row_error) return;
    char buf[64];
    buf[0] = 0;
    str_cat(buf, what, sizeof buf);
    str_cat(buf, " on line ", sizeof buf);
    put_uint(buf + strlen(buf), 0, (uint32_t)line, 1);
    lv_label_set_text(s_row_error, buf);
}

static void on_row(lv_event_t *e)
{
    wake_screen();
    int idx = (int)(lv_uintptr_t)lv_event_get_user_data(e);

    bool ok = false;
    if (s_tab == 0) {
        ok = en_engine_play_preset(idx);
    } else if (s_tab == 1) {
        ok = en_engine_play_program(idx);
    } else {
        /* A user program: read the file, parse it, play it. A malformed file
           is reported on its own row rather than failing silently. */
        char name[64], path[192];
        if (en_sys_list_dir(en_sys_programs_dir(), idx, name, sizeof name)) {
            path[0] = 0;
            str_cat(path, en_sys_programs_dir(), sizeof path);
            str_cat(path, "/", sizeof path);
            str_cat(path, name, sizeof path);

            static char text[4096];
            uint32_t n = en_sys_read_file(path, text, sizeof text - 1);
            if (n) {
                text[n] = 0;
                en_user_program_t up;
                int err_line = 0;
                int rc = en_parse_program(text, n, &up, &err_line);
                if (rc == EN_PARSE_OK) {
                    ok = en_engine_play_user(&up);
                } else {
                    show_row_error(idx, en_parse_error_text(rc), err_line);
                    return;
                }
            }
        }
    }

    if (ok) {
        P.last_source = s_tab == 0 ? EN_SRC_PRESET : EN_SRC_PROGRAM;
        P.last_index = idx;
        prefs_save();
        en_engine_set_sleep_timer((uint32_t)P.sleep_timer_s);
        en_ui_goto(EN_SCREEN_NOW, true);
    }
}

static void style_tab(int i, bool on)
{
    lv_obj_set_style_bg_color(s_tab_btn[i],
                              lv_color_hex(on ? C_SURFACE_2 : C_SURFACE), 0);
    lv_obj_t *l = lv_obj_get_child(s_tab_btn[i], 0);
    if (l) lv_obj_set_style_text_color(l,
              lv_color_hex(on ? C_TEXT : C_TEXT_DIM), 0);
}

/* A library row: band dot, name, technical one-liner, right-aligned value. */
static void add_row(int index, uint32_t color, const char *name,
                    const char *detail, const char *value)
{
    lv_obj_t *row = lv_obj_create(s_list);
    flat(row, C_BG);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, CONTENT_W, ROW_H);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED,
                        (void *)(lv_uintptr_t)index);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_SURFACE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *dot = lv_obj_create(row);
    flat(dot, color);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_pos(dot, 0, 15);

    lv_obj_t *n = make_label(row, name, F_BODY, C_TEXT);
    lv_obj_set_pos(n, 18, 8);

    lv_obj_t *d = make_label(row, detail, F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_width(d, CONTENT_W - 18);
    /* Width first, then the mode: a detail line that wrapped would push into
       the row below instead of being trimmed. */
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(d, 18, 31);

    lv_obj_t *v = make_label(row, value, F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(v, 78);
    lv_obj_set_pos(v, CONTENT_W - 78, 10);

    make_hairline(row, ROW_H - 1, 18, CONTENT_W - 18);
}

static void refresh_library(void)
{
    for (int i = 0; i < 3; i++) style_tab(i, i == s_tab);
    if (s_row_error) lv_label_set_text(s_row_error, "");
    lv_obj_clean(s_list);

    char buf[64];
    if (s_tab == 0) {
        int n;
        const en_preset_t *ps = en_presets(&n);
        for (int i = 0; i < n; i++) {
            en_loop_plan_t plan;
            buf[0] = 0;
            if (en_plan_loop(ps[i].beat_hz, ps[i].carrier_hz, EN_RATES,
                             EN_RATES_COUNT, EN_TARGET_BYTES, &plan)) {
                /* Show the realised beat, never the requested one. */
                fmt_fixed(buf, plan.beat_hz, 2);
                str_cat(buf, " Hz", sizeof buf);
            }
            add_row(i, en_band_color(en_band_of(ps[i].beat_hz)),
                    ps[i].name, ps[i].detail, buf);
        }
    } else if (s_tab == 1) {
        int n;
        const en_program_t *ps = en_programs(&n);
        for (int i = 0; i < n; i++) {
            uint32_t secs = en_program_seconds(&ps[i]);
            buf[0] = 0;
            put_uint(buf, 0, secs / 60u, 1);
            str_cat(buf, " min", sizeof buf);
            add_row(i, en_band_color(en_program_band(&ps[i])),
                    ps[i].name, ps[i].detail, buf);
        }
    } else {
        int shown = 0;
        char name[64];
        for (int i = 0; i < 32; i++) {
            if (!en_sys_list_dir(en_sys_programs_dir(), i, name, sizeof name))
                break;
            add_row(i, C_TEXT_MUTE, name, "user program", "");
            shown++;
        }
        if (shown == 0) {
            lv_obj_t *empty = lv_obj_create(s_list);
            flat(empty, C_BG);
            lv_obj_remove_flag(empty, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(empty, CONTENT_W, 120);
            lv_obj_t *t = make_label(empty, "No user programs", F_BODY,
                                     C_TEXT_DIM);
            lv_obj_set_pos(t, 0, 24);
            lv_obj_t *h = make_para(empty,
                "Drop .txt program files into "
                "/Apps/Data/Entrain/programs. The format is in the README.",
                F_CAPTION, C_TEXT_MUTE, CONTENT_W);
            lv_obj_set_pos(h, 0, 50);
        }
    }
}

static void build_library(void)
{
    lv_obj_t *s = make_screen();
    s_screen[EN_SCREEN_LIBRARY] = s;

    static const nav_t nav = { EN_SCREEN_NOW, EN_SCREEN_COUNT };
    lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, (void *)&nav);

    lv_obj_t *title = make_label(s, "Entrain", F_TITLE, C_TEXT);
    lv_obj_set_pos(title, MARGIN, 12);

    s_battery_lbl = make_label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_battery_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_battery_lbl, 60);
    lv_obj_set_pos(s_battery_lbl, EN_SCREEN_W - MARGIN - 60, 18);

    make_hairline(s, 46, 0, EN_SCREEN_W);

    static const char *TABS[3] = { "Presets", "Programs", "Custom" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_obj_create(s);
        flat(b, C_SURFACE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(b, CONTENT_W / 3, TAP_MIN);
        lv_obj_set_pos(b, MARGIN + i * (CONTENT_W / 3), 58);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, on_tab, LV_EVENT_CLICKED,
                            (void *)(lv_uintptr_t)i);
        lv_obj_t *l = make_label(b, TABS[i], F_CAPTION, C_TEXT_DIM);
        lv_obj_center(l);
        s_tab_btn[i] = b;
    }

    s_list = lv_obj_create(s);
    flat(s_list, C_BG);
    lv_obj_set_size(s_list, CONTENT_W, EN_SCREEN_H - 118);
    lv_obj_set_pos(s_list, MARGIN, 114);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    /* Momentum, no snapping — the list should coast, not click into place. */
    lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_row_error = make_label(s, "", F_CAPTION, 0xE06C6C);
    lv_obj_set_width(s_row_error, CONTENT_W);
    lv_obj_set_pos(s_row_error, MARGIN, EN_SCREEN_H - 20);
}

/* ---- now playing --------------------------------------------------------- */

static void on_ring_click(lv_event_t *e)
{
    (void)e;
    wake_screen();
    if (en_engine_is_active()) en_engine_toggle_pause();
}

static void build_now(void)
{
    lv_obj_t *s = make_screen();
    s_screen[EN_SCREEN_NOW] = s;

    static const nav_t nav = { EN_SCREEN_TUNE, EN_SCREEN_LIBRARY };
    lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, (void *)&nav);

    /* A grabber, so "swipe down to go back" is discoverable without a label. */
    lv_obj_t *grab = lv_obj_create(s);
    flat(grab, C_HAIRLINE);
    lv_obj_remove_flag(grab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(grab, 36, 3);
    lv_obj_set_style_radius(grab, 2, 0);
    lv_obj_set_pos(grab, (EN_SCREEN_W - 36) / 2, 10);

    s_title_lbl = make_label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_title_lbl, CONTENT_W);
    lv_obj_set_pos(s_title_lbl, MARGIN, 26);

    /* The breathing circle sits behind the arc so the pulse reads as the ring
       glowing rather than the progress jumping. */
    s_pulse = lv_obj_create(s);
    flat(s_pulse, C_BG);
    lv_obj_remove_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pulse, PULSE_R * 2, PULSE_R * 2);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pulse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pulse, 2, 0);
    lv_obj_set_style_border_color(s_pulse, lv_color_hex(C_HAIRLINE), 0);
    lv_obj_set_pos(s_pulse, RING_CX - PULSE_R, RING_CY - PULSE_R);

    s_ring = lv_arc_create(s);
    lv_obj_set_size(s_ring, RING_R * 2, RING_R * 2);
    lv_obj_set_pos(s_ring, RING_CX - RING_R, RING_CY - RING_R);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(C_HAIRLINE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, LV_PART_KNOB);

    /* The tap target is the whole ring area, not a small transport button. */
    lv_obj_t *hit = lv_obj_create(s);
    flat(hit, C_BG);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(hit, RING_R * 2, RING_R * 2);
    lv_obj_set_pos(hit, RING_CX - RING_R, RING_CY - RING_R);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, on_ring_click, LV_EVENT_CLICKED, NULL);

    s_beat_lbl = make_label(hit, "--", F_HERO, C_TEXT);
    lv_obj_set_style_text_align(s_beat_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_beat_lbl, RING_R * 2 - 20);
    lv_obj_set_pos(s_beat_lbl, 10, 44);

    s_hz_lbl = make_label(hit, "Hz", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_hz_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_hz_lbl, RING_R * 2 - 20);
    lv_obj_set_pos(s_hz_lbl, 10, 88);

    s_band_lbl = make_label(hit, "", F_BODY, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_band_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_band_lbl, RING_R * 2 - 20);
    lv_obj_set_pos(s_band_lbl, 10, 108);

    s_carrier_lbl = make_label(hit, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_carrier_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_carrier_lbl, RING_R * 2 - 20);
    lv_obj_set_pos(s_carrier_lbl, 10, 130);

    s_state_lbl = make_label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_state_lbl, CONTENT_W);
    lv_obj_set_pos(s_state_lbl, MARGIN, RING_CY + RING_R + 14);

    s_time_lbl = make_label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_time_lbl, CONTENT_W);
    lv_obj_set_pos(s_time_lbl, MARGIN, RING_CY + RING_R + 40);

    /* The timeline: a thin rule with a fill and a tick at each segment
       boundary, so a program's shape is legible at a glance. */
    s_timeline = lv_obj_create(s);
    flat(s_timeline, C_HAIRLINE);
    lv_obj_remove_flag(s_timeline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_timeline, CONTENT_W, 3);
    lv_obj_set_pos(s_timeline, MARGIN, RING_CY + RING_R + 66);

    s_timeline_fill = lv_obj_create(s_timeline);
    flat(s_timeline_fill, C_TEXT_DIM);
    lv_obj_remove_flag(s_timeline_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_timeline_fill, 0, 3);
    lv_obj_set_pos(s_timeline_fill, 0, 0);

    for (int i = 0; i < EN_USER_MAX_SEGS; i++) {
        lv_obj_t *t = lv_obj_create(s);
        flat(t, C_TEXT_MUTE);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(t, 1, 9);
        lv_obj_set_pos(t, MARGIN, RING_CY + RING_R + 63);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        s_seg_tick[i] = t;
    }

    s_mode_lbl = make_label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_mode_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_mode_lbl, CONTENT_W);
    lv_obj_set_pos(s_mode_lbl, MARGIN, RING_CY + RING_R + 92);

    s_prep_lbl = make_label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_prep_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_prep_lbl, CONTENT_W);
    lv_obj_set_pos(s_prep_lbl, MARGIN, RING_CY + RING_R + 118);

    lv_obj_t *hint = make_label(s, "up to tune  •  down for library",
                                F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, CONTENT_W);
    lv_obj_set_pos(hint, MARGIN, EN_SCREEN_H - 26);
}

static void refresh_now(void)
{
    uint32_t accent = en_band_color(en_engine_band());
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_pulse, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_color(s_timeline_fill, lv_color_hex(accent), 0);

    lv_label_set_text(s_title_lbl, en_engine_title());

    char buf[64];
    fmt_fixed(buf, en_engine_beat(), 2);
    lv_label_set_text(s_beat_lbl, buf);
    lv_label_set_text(s_band_lbl, en_band_name(en_engine_band()));

    buf[0] = 0;
    fmt_fixed(buf, en_engine_carrier(), 1);
    str_cat(buf, " Hz carrier", sizeof buf);
    lv_label_set_text(s_carrier_lbl, buf);

    /* Segment ticks. Hidden entirely for a preset, which has no timeline. */
    double total = en_engine_total();
    int nseg = en_engine_seg_count();
    for (int i = 0; i < EN_USER_MAX_SEGS; i++) {
        if (i > 0 && i < nseg && total > 0.0) {
            int x = MARGIN + (int)((en_engine_seg_start(i) / total)
                                   * (double)CONTENT_W);
            lv_obj_set_pos(s_seg_tick[i], x, RING_CY + RING_R + 63);
            lv_obj_remove_flag(s_seg_tick[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_seg_tick[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (total <= 0.0) lv_obj_add_flag(s_timeline, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_timeline, LV_OBJ_FLAG_HIDDEN);

    /* Which mode is playing is not audible from the tone, and it decides
       whether headphones are required, so it belongs on this screen. */
    buf[0] = 0;
    str_cat(buf, en_mode_name(en_engine_mode()), sizeof buf);
    if (en_engine_mode() == EN_MODE_BINAURAL)
        str_cat(buf, "  •  headphones", sizeof buf);
    lv_label_set_text(s_mode_lbl, buf);
}

static void tick_now(void)
{
    if (s_current != EN_SCREEN_NOW) return;

    char buf[64];
    fmt_fixed(buf, en_engine_beat(), 2);
    lv_label_set_text(s_beat_lbl, buf);

    double elapsed = en_engine_elapsed();
    double total = en_engine_total();

    if (total > 0.0) {
        double u = elapsed / total;
        if (u > 1.0) u = 1.0;
        lv_arc_set_value(s_ring, (int32_t)(u * 1000.0));
        lv_obj_set_width(s_timeline_fill, (int32_t)(u * CONTENT_W));

        buf[0] = 0;
        fmt_time(buf, (uint32_t)elapsed);
        str_cat(buf, "  /  ", sizeof buf);
        char rem[16];
        fmt_time(rem, (uint32_t)(total - elapsed > 0.0 ? total - elapsed : 0.0));
        str_cat(buf, rem, sizeof buf);
        lv_label_set_text(s_time_lbl, buf);
    } else {
        /* A preset is endless: the ring shows the loop position instead of a
           progress that would never arrive anywhere. */
        lv_arc_set_value(s_ring, 0);
        buf[0] = 0;
        fmt_time(buf, (uint32_t)elapsed);
        str_cat(buf, "  elapsed", sizeof buf);
        lv_label_set_text(s_time_lbl, buf);
    }

    const char *state = "";
    if (en_engine_is_paused())       state = "Paused";
    else if (en_engine_is_playing()) state = "";
    else if (en_engine_is_active())  state = "Preparing";
    else                             state = "Tap the ring to play";
    lv_label_set_text(s_state_lbl, state);

    double prog = en_engine_render_progress();
    if (prog >= 0.0) {
        buf[0] = 0;
        str_cat(buf, "rendering ", sizeof buf);
        put_uint(buf + strlen(buf), 0, (uint32_t)(prog * 100.0), 1);
        str_cat(buf, "%", sizeof buf);
        lv_label_set_text(s_prep_lbl, buf);
    } else if (en_engine_is_retuning()) {
        lv_label_set_text(s_prep_lbl, "retuning");
    } else {
        uint32_t sleep_left = en_engine_sleep_remaining();
        if (sleep_left) {
            buf[0] = 0;
            str_cat(buf, "sleep in ", sizeof buf);
            char t[16];
            fmt_time(t, sleep_left);
            str_cat(buf, t, sizeof buf);
            lv_label_set_text(s_prep_lbl, buf);
        } else {
            lv_label_set_text(s_prep_lbl, "");
        }
    }
}

/* The pulse. Confined to the ring, shallow, and rate-limited — see the header
   comment. Reduce Motion switches it off completely. */
static void tick_pulse(void)
{
    if (s_current != EN_SCREEN_NOW) return;

    if (P.reduce_motion || !en_engine_is_playing()) {
        lv_obj_set_style_border_opa(s_pulse, LV_OPA_40, 0);
        lv_obj_set_size(s_pulse, PULSE_R * 2, PULSE_R * 2);
        lv_obj_set_pos(s_pulse, RING_CX - PULSE_R, RING_CY - PULSE_R);
        return;
    }

    /* Halve the visual rate until it is a breath rather than a flicker. It
       stays phase-locked to the beat, so it still reads as the same rhythm. */
    double hz = en_engine_beat();
    while (hz > PULSE_MAX_HZ) hz *= 0.5;

    double phase = en_engine_elapsed() * hz;
    phase -= (double)(long)phase;
    double s = 0.5 - 0.5 * en_sin_turns(0.25 + phase);   /* 0..1, smooth */

    lv_opa_t opa = (lv_opa_t)(PULSE_OPA_MIN
                    + (PULSE_OPA_MAX - PULSE_OPA_MIN) * s);
    lv_obj_set_style_border_opa(s_pulse, opa, 0);

    int r = PULSE_R + (int)(PULSE_R_SWING * s);
    lv_obj_set_size(s_pulse, r * 2, r * 2);
    lv_obj_set_pos(s_pulse, RING_CX - r, RING_CY - r);
}

/* ---- live tune ----------------------------------------------------------- */

/* Vertical drag moves the beat, horizontal moves the carrier. Both are
   continuous; the readout updates immediately and the audio catches up once
   the gesture settles, with the old loop still playing meanwhile. */
static void on_tune_press(lv_event_t *e)
{
    (void)e;
    wake_screen();
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_indev_get_point(in, &s_drag_last);
    s_dragging = true;

    uint32_t now = en_sys_millis();
    if (now - s_last_tap_ms < 350) {
        en_engine_live_reset();     /* double tap resets to the preset */
        s_dragging = false;
    }
    s_last_tap_ms = now;
}

static void on_tune_move(lv_event_t *e)
{
    (void)e;
    if (!s_dragging) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;

    lv_point_t p;
    lv_indev_get_point(in, &p);
    int dx = p.x - s_drag_last.x;
    int dy = p.y - s_drag_last.y;
    s_drag_last = p;

    /* Up is faster. 0.02 Hz per pixel over a 180 px ring is about 3.6 Hz of
       travel across the whole gesture — fine control without being fiddly. */
    en_engine_live_adjust(-dy * 0.02, dx * 0.5);
}

static void on_tune_release(lv_event_t *e)
{
    (void)e;
    s_dragging = false;
}

static void build_tune(void)
{
    lv_obj_t *s = make_screen();
    s_screen[EN_SCREEN_TUNE] = s;

    static const nav_t nav = { EN_SCREEN_TIMER, EN_SCREEN_NOW };
    lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, (void *)&nav);

    lv_obj_t *title = make_label(s, "Live Tune", F_TITLE, C_TEXT);
    lv_obj_set_pos(title, MARGIN, 12);

    lv_obj_t *hint = make_para(s,
        "Drag up and down for the beat, left and right for the carrier. "
        "Double tap to reset.",
        F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(hint, MARGIN, 44);

    s_tune_ring = lv_obj_create(s);
    flat(s_tune_ring, C_SURFACE);
    lv_obj_remove_flag(s_tune_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_tune_ring, RING_R * 2, RING_R * 2);
    lv_obj_set_style_radius(s_tune_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(s_tune_ring, RING_CX - RING_R, 118);
    lv_obj_add_flag(s_tune_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tune_ring, on_tune_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_tune_ring, on_tune_move, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_tune_ring, on_tune_release, LV_EVENT_RELEASED, NULL);
    /* Without this a drag upward to raise the beat would also register as a
       swipe on the screen and navigate away mid-gesture. */
    lv_obj_remove_flag(s_tune_ring, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_tune_beat = make_label(s_tune_ring, "--", F_HERO, C_TEXT);
    lv_obj_set_style_text_align(s_tune_beat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tune_beat, RING_R * 2 - 20);
    lv_obj_set_pos(s_tune_beat, 10, 52);

    s_tune_carrier = make_label(s_tune_ring, "", F_BODY, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_tune_carrier, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tune_carrier, RING_R * 2 - 20);
    lv_obj_set_pos(s_tune_carrier, 10, 100);

    s_tune_state = make_label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_tune_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tune_state, CONTENT_W);
    lv_obj_set_pos(s_tune_state, MARGIN, 316);

    lv_obj_t *note = make_para(s,
        "The loop already playing keeps going until the new one is ready.",
        F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(note, MARGIN, 344);

    lv_obj_t *hint2 = make_label(s, "swipe down for now playing",
                                 F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(hint2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint2, CONTENT_W);
    lv_obj_set_pos(hint2, MARGIN, EN_SCREEN_H - 26);
}

static void tick_tune(void)
{
    if (s_current != EN_SCREEN_TUNE) return;

    uint32_t accent = en_band_color(en_engine_band());
    lv_obj_set_style_bg_color(s_tune_ring, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_border_width(s_tune_ring, 2, 0);
    lv_obj_set_style_border_color(s_tune_ring, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(s_tune_ring, LV_OPA_60, 0);

    char buf[48];
    fmt_fixed(buf, en_engine_beat(), 2);
    lv_label_set_text(s_tune_beat, buf);

    buf[0] = 0;
    fmt_fixed(buf, en_engine_carrier(), 1);
    str_cat(buf, " Hz", sizeof buf);
    lv_label_set_text(s_tune_carrier, buf);

    lv_label_set_text(s_tune_state,
                      en_engine_is_retuning() ? "retuning" : "");
}

/* ---- timer --------------------------------------------------------------- */

static void on_sleep_choice(lv_event_t *e)
{
    wake_screen();
    int i = (int)(lv_uintptr_t)lv_event_get_user_data(e);
    P.sleep_timer_s = (int)SLEEP_CHOICES[i];
    prefs_save();
    en_engine_set_sleep_timer((uint32_t)P.sleep_timer_s);
    refresh_timer();
}

static void on_blankplay(lv_event_t *e)
{
    wake_screen();
    P.blank_while_playing = lv_obj_has_state(lv_event_get_target_obj(e),
                                             LV_STATE_CHECKED) ? 1 : 0;
    prefs_save();
}

/* A settings-style row: full width, tall enough to hit, label plus a slot on
   the right for a value or a control. */
static lv_obj_t *setting_row(lv_obj_t *parent, int y, const char *title,
                             const char *sub)
{
    lv_obj_t *row = lv_obj_create(parent);
    flat(row, C_BG);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, CONTENT_W, sub ? 56 : TAP_MIN);
    lv_obj_set_pos(row, MARGIN, y);

    lv_obj_t *t = make_label(row, title, F_BODY, C_TEXT);
    lv_obj_set_pos(t, 0, sub ? 6 : 12);
    if (sub) {
        lv_obj_t *sl = make_label(row, sub, F_CAPTION, C_TEXT_MUTE);
        lv_obj_set_pos(sl, 0, 29);
    }
    return row;
}

static void build_timer(void)
{
    lv_obj_t *s = make_screen();
    s_screen[EN_SCREEN_TIMER] = s;

    static const nav_t nav = { EN_SCREEN_SETTINGS, EN_SCREEN_TUNE };
    lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, (void *)&nav);

    lv_obj_t *title = make_label(s, "Sleep Timer", F_TITLE, C_TEXT);
    lv_obj_set_pos(title, MARGIN, 12);

    lv_obj_t *sub = make_para(s, "Fades out, then stops.",
                              F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(sub, MARGIN, 40);

    make_hairline(s, 62, MARGIN, CONTENT_W);

    for (int i = 0; i < 6; i++) {
        lv_obj_t *row = lv_obj_create(s);
        flat(row, C_BG);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, CONTENT_W, TAP_MIN);
        lv_obj_set_pos(row, MARGIN, 70 + i * (TAP_MIN + 1));
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_sleep_choice, LV_EVENT_CLICKED,
                            (void *)(lv_uintptr_t)i);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_SURFACE),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        lv_obj_t *l = make_label(row, SLEEP_LABELS[i], F_BODY, C_TEXT);
        lv_obj_set_pos(l, 0, 12);

        lv_obj_t *chk = make_label(row, "", F_BODY, C_TEXT);
        lv_obj_set_style_text_align(chk, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(chk, 40);
        lv_obj_set_pos(chk, CONTENT_W - 40, 12);

        make_hairline(row, TAP_MIN, 0, CONTENT_W);
        s_timer_rows[i] = row;
    }

    lv_obj_t *row = setting_row(s, 70 + 6 * (TAP_MIN + 1) + 10,
                                "Blank screen",
                                "Audio keeps running");
    s_blankplay_sw = lv_switch_create(row);
    lv_obj_set_size(s_blankplay_sw, 44, 26);
    lv_obj_set_pos(s_blankplay_sw, CONTENT_W - 44, 12);
    lv_obj_add_event_cb(s_blankplay_sw, on_blankplay, LV_EVENT_VALUE_CHANGED,
                        NULL);

    s_sleep_lbl = make_label(s, "", F_CAPTION, C_TEXT_DIM);
    /* The countdown sits beside the title, not under it: the subtitle needs the
       full width, and a live number belongs in the header anyway. */
    lv_obj_set_style_text_align(s_sleep_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_sleep_lbl, 100);
    lv_obj_set_pos(s_sleep_lbl, EN_SCREEN_W - MARGIN - 100, 18);

    lv_obj_t *hint = make_label(s, "swipe down for live tune",
                                F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, CONTENT_W);
    lv_obj_set_pos(hint, MARGIN, EN_SCREEN_H - 26);
}

static void refresh_timer(void)
{
    for (int i = 0; i < 6; i++) {
        bool on = (int)SLEEP_CHOICES[i] == P.sleep_timer_s;
        lv_obj_t *label = lv_obj_get_child(s_timer_rows[i], 0);
        lv_obj_t *check = lv_obj_get_child(s_timer_rows[i], 1);
        uint32_t accent = en_band_color(en_engine_band());
        if (label) lv_obj_set_style_text_color(label,
                       lv_color_hex(on ? C_TEXT : C_TEXT_DIM), 0);
        if (check) {
            lv_label_set_text(check, on ? LV_SYMBOL_OK : "");
            lv_obj_set_style_text_color(check, lv_color_hex(accent), 0);
        }
    }
    if (s_blankplay_sw) {
        if (P.blank_while_playing)
            lv_obj_add_state(s_blankplay_sw, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(s_blankplay_sw, LV_STATE_CHECKED);
    }
    if (s_sleep_lbl) {
        uint32_t left = en_engine_sleep_remaining();
        if (left) {
            char buf[32];
            buf[0] = 0;
            str_cat(buf, "stops in ", sizeof buf);
            char t[16];
            fmt_time(t, left);
            str_cat(buf, t, sizeof buf);
            lv_label_set_text(s_sleep_lbl, buf);
        } else {
            lv_label_set_text(s_sleep_lbl, "");
        }
    }
}

/* ---- settings ------------------------------------------------------------ */

static void on_volume(lv_event_t *e)
{
    wake_screen();
    int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
    P.volume = v;
    if (P.volume > P.volume_cap) P.volume = P.volume_cap;
    en_audio_set_volume(P.volume);
    prefs_save();
    refresh_settings();
}

static void on_reduce_motion(lv_event_t *e)
{
    wake_screen();
    P.reduce_motion = lv_obj_has_state(lv_event_get_target_obj(e),
                                       LV_STATE_CHECKED) ? 1 : 0;
    prefs_save();
}

static void on_blank_delay(lv_event_t *e)
{
    wake_screen();
    static const int CHOICES[] = { 0, 15, 30, 60, 120 };
    int i = 0;
    for (; i < 5; i++) if (CHOICES[i] == P.blank_delay_s) break;
    i = (i + 1) % 5;
    P.blank_delay_s = CHOICES[i];
    prefs_save();
    refresh_settings();
    (void)e;
}

static void build_settings(void)
{
    lv_obj_t *s = make_screen();
    s_screen[EN_SCREEN_SETTINGS] = s;

    static const nav_t nav = { EN_SCREEN_COUNT, EN_SCREEN_TIMER };
    lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, (void *)&nav);

    lv_obj_t *title = make_label(s, "Settings", F_TITLE, C_TEXT);
    lv_obj_set_pos(title, MARGIN, 12);
    make_hairline(s, 46, MARGIN, CONTENT_W);

    int y = 58;

    lv_obj_t *vrow = setting_row(s, y, "Volume", "Starts moderate on purpose");
    s_vol_lbl = make_label(vrow, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_vol_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_vol_lbl, 44);
    lv_obj_set_pos(s_vol_lbl, CONTENT_W - 44, 8);
    y += 58;

    s_vol_slider = lv_slider_create(s);
    lv_obj_set_size(s_vol_slider, CONTENT_W, 8);
    lv_obj_set_pos(s_vol_slider, MARGIN, y);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_add_event_cb(s_vol_slider, on_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(C_SURFACE_2),
                              LV_PART_MAIN);
    lv_obj_set_style_radius(s_vol_slider, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_vol_slider, 2, LV_PART_INDICATOR);
    /* The knob stays visually modest; the touch target is widened instead, so
       the control is still comfortably grabbable on an 8 px track. */
    lv_obj_set_style_pad_all(s_vol_slider, 9, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_vol_slider, 18);
    y += 34;

    lv_obj_t *brow = setting_row(s, y, "Blank screen after", NULL);
    lv_obj_add_flag(brow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(brow, on_blank_delay, LV_EVENT_CLICKED, NULL);
    s_blank_lbl = make_label(brow, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_blank_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_blank_lbl, 70);
    lv_obj_set_pos(s_blank_lbl, CONTENT_W - 70, 14);
    make_hairline(s, y + TAP_MIN, MARGIN, CONTENT_W);
    y += TAP_MIN + 10;

    lv_obj_t *mrow = setting_row(s, y, "Reduce motion",
                                 "Stops the ring pulsing");
    s_motion_sw = lv_switch_create(mrow);
    lv_obj_set_size(s_motion_sw, 44, 26);
    lv_obj_set_pos(s_motion_sw, CONTENT_W - 44, 14);
    lv_obj_add_event_cb(s_motion_sw, on_reduce_motion, LV_EVENT_VALUE_CHANGED,
                        NULL);
    y += 66;

    make_hairline(s, y, MARGIN, CONTENT_W);
    y += 12;

    lv_obj_t *note = make_para(s,
        "Binaural needs headphones: the beat is the difference between "
        "the ears. Isochronic works on speakers.",
        F_CAPTION, C_TEXT_DIM, CONTENT_W);
    lv_obj_set_pos(note, MARGIN, y);
    y += 74;

    lv_obj_t *about = make_para(s,
        "Tones are generated on the device. No health claims are made.",
        F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(about, MARGIN, y);

    lv_obj_t *hint = make_label(s, "swipe down for sleep timer",
                                F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, CONTENT_W);
    lv_obj_set_pos(hint, MARGIN, EN_SCREEN_H - 26);
}

static void refresh_settings(void)
{
    char buf[24];
    buf[0] = 0;
    put_uint(buf, 0, (uint32_t)P.volume, 1);
    str_cat(buf, "%", sizeof buf);
    if (s_vol_lbl) lv_label_set_text(s_vol_lbl, buf);
    if (s_vol_slider) lv_slider_set_value(s_vol_slider, P.volume, LV_ANIM_OFF);

    if (s_blank_lbl) {
        if (P.blank_delay_s == 0) {
            lv_label_set_text(s_blank_lbl, "Never");
        } else {
            buf[0] = 0;
            put_uint(buf, 0, (uint32_t)P.blank_delay_s, 1);
            str_cat(buf, " s", sizeof buf);
            lv_label_set_text(s_blank_lbl, buf);
        }
    }
    if (s_motion_sw) {
        if (P.reduce_motion) lv_obj_add_state(s_motion_sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_motion_sw, LV_STATE_CHECKED);
    }

    uint32_t accent = en_band_color(en_engine_band());
    if (s_vol_slider)
        lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(accent),
                                  LV_PART_INDICATOR);
}

/* ---- first run ----------------------------------------------------------- */

static void on_first_run_ok(lv_event_t *e)
{
    (void)e;
    P.seen_first_run = 1;
    prefs_save();
    en_ui_goto(EN_SCREEN_LIBRARY, true);
}

static void build_first_run(void)
{
    lv_obj_t *s = make_screen();
    s_screen[EN_SCREEN_FIRSTRUN] = s;

    lv_obj_t *title = make_label(s, "Entrain", F_TITLE, C_TEXT);
    lv_obj_set_pos(title, MARGIN, 60);

    lv_obj_t *body = make_para(s,
        "Put headphones on.\n\n"
        "A binaural beat is the difference between two tones, one in each "
        "ear. On speakers the two mix in the air and the effect is gone.\n\n"
        "Isochronic presets pulse a single tone instead, and do work on "
        "speakers.\n\n"
        "Volume starts moderate. Long sessions are the reason to leave it "
        "there.",
        F_CAPTION, C_TEXT_DIM, CONTENT_W);
    lv_obj_set_pos(body, MARGIN, 100);

    lv_obj_t *btn = lv_obj_create(s);
    flat(btn, C_SURFACE_2);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, CONTENT_W, TAP_MIN + 6);
    lv_obj_set_pos(btn, MARGIN, EN_SCREEN_H - 80);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_first_run_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = make_label(btn, "Got it", F_BODY, C_TEXT);
    lv_obj_center(bl);
}

void en_ui_set_first_run(bool first_run)
{
    P.seen_first_run = first_run ? 0 : 1;
}

void en_ui_set_blanking(bool enabled)
{
    s_blanking_allowed = enabled;
    if (!enabled) wake_screen();
}

void en_ui_set_tab(int tab)
{
    if (tab < 0 || tab > 2) return;
    s_tab = tab;
    if (s_list) refresh_library();
}

/* ---- lifecycle ----------------------------------------------------------- */

void en_ui_init(void)
{
    /* Re-theme before building anything. The default theme's blue would look
       like a different app next to this palette, and re-skinning every themed
       widget by hand afterwards is worse than setting it once. */
    lv_display_t *disp = lv_display_get_default();
    if (disp)
        lv_theme_default_init(disp, lv_color_hex(C_ACCENT),
                              lv_color_hex(C_TEXT_DIM), true, F_BODY);

    prefs_load();
    en_engine_init();
    en_audio_set_volume(P.volume);

    build_library();
    build_now();
    build_tune();
    build_timer();
    build_settings();
    build_first_run();

    refresh_library();
    refresh_settings();
    refresh_timer();

    s_last_touch_ms = en_sys_millis();

    if (!P.seen_first_run) {
        s_current = EN_SCREEN_FIRSTRUN;
        lv_screen_load(s_screen[EN_SCREEN_FIRSTRUN]);
    } else {
        s_current = EN_SCREEN_LIBRARY;
        lv_screen_load(s_screen[EN_SCREEN_LIBRARY]);
        /* Resume whatever was playing last, ready to start but silent until
           the user asks — waking to sudden audio would be hostile. */
        if (P.last_source == EN_SRC_PRESET) s_tab = 0;
        else if (P.last_source == EN_SRC_PROGRAM) s_tab = 1;
        refresh_library();
    }
}

void en_ui_tick(void)
{
    en_engine_tick();

    static uint32_t last_slow;
    uint32_t now = en_sys_millis();

    tick_pulse();
    tick_now();
    tick_tune();

    /* The battery and the sleep countdown do not need 60 Hz. */
    if (now - last_slow > 1000) {
        last_slow = now;
        int pct = en_sys_battery_percent();
        if (s_battery_lbl && pct >= 0) {
            char buf[16];
            buf[0] = 0;
            put_uint(buf, 0, (uint32_t)pct, 1);
            str_cat(buf, "%", sizeof buf);
            lv_label_set_text(s_battery_lbl, buf);
        }
    }

    blank_tick();
}

void en_ui_key(en_key_t key)
{
    wake_screen();
    switch (key) {
    case EN_KEY_VOL_UP:
        P.volume += 5;
        if (P.volume > P.volume_cap) P.volume = P.volume_cap;
        en_audio_set_volume(P.volume);
        prefs_save();
        refresh_settings();
        break;
    case EN_KEY_VOL_DOWN:
        P.volume -= 5;
        if (P.volume < 0) P.volume = 0;
        en_audio_set_volume(P.volume);
        prefs_save();
        refresh_settings();
        break;
    case EN_KEY_PLAY_PAUSE:
        en_engine_toggle_pause();
        break;
    case EN_KEY_BACK:
        if (s_current == EN_SCREEN_LIBRARY) en_sys_request_exit();
        else en_ui_goto(EN_SCREEN_LIBRARY, true);
        break;
    case EN_KEY_NEXT_PROGRAM: {
        int n = 0;
        en_programs(&n);
        if (n <= 0) break;
        s_prog_cycle = (s_prog_cycle + 1) % n;
        if (en_engine_play_program(s_prog_cycle)) {
            P.last_source = EN_SRC_PROGRAM;
            P.last_index = s_prog_cycle;
            prefs_save();
            en_engine_set_sleep_timer((uint32_t)P.sleep_timer_s);
            s_tab = 1;
            en_ui_goto(EN_SCREEN_NOW, true);
        }
        break;
    }
    }
}

void en_ui_shutdown(void)
{
    /* Stop first. Leaving a loop playing after the app is gone is the single
       worst thing this app could do. */
    en_engine_stop(600);
    prefs_save();
    en_sys_backlight(100);
    en_engine_shutdown();
}

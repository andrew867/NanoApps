/*
 * ui.c — the Radio+ interface.
 *
 * 240 x 432. A tall narrow portrait, which decides the whole layout: the
 * frequency takes the top because it is what you glance at, the station and its
 * radio text take the middle because that is what you actually read, and the
 * transport sits at the bottom where a thumb reaches without moving the hand.
 *
 * Rules the screens keep to:
 *
 *   Flat fills and hairlines. No gradients, no shadows, no rounded panels
 *   pretending to be physical. A 240-wide screen has no room to spend on
 *   decoration, and the iPod's own interface does not either.
 *
 *   Colour means something. Cyan is signal, violet is RDS, amber is a traffic
 *   announcement, rose is recording, green is the live buffer. Nothing is
 *   coloured to look nice; if a thing is coloured it is because its state
 *   matters, and the rest is grey.
 *
 *   Nothing smaller than 44 px is tappable, and 12 px of margin everywhere.
 *
 *   The frequency never moves. It is the one element that must be findable
 *   without looking, so it keeps its position and its size whatever else the
 *   screen is doing - no reflowing to make room for a station name.
 *
 * Everything drawn comes from rp_model and nothing else, so the same screens
 * render on a desktop with no tuner attached.
 */

#include "ui.h"
#include "../build_stamp.h"
#include "model.h"

#include "lvgl/lvgl.h"

#include "core/affollow.h"
#include "core/fmreg.h"
#include "core/scan.h"
#include "core/timer.h"

/* ---- palette -------------------------------------------------------------- */

#define C_BG        0x08090D
#define C_SURFACE   0x101219
#define C_SURFACE_2 0x171B25
#define C_HAIRLINE  0x232937
#define C_TEXT      0xEDEFF4
#define C_TEXT_DIM  0x8B92A0
#define C_TEXT_MUTE 0x545B69

#define C_SIGNAL    0x22D3EE   /* signal, stereo lock */
#define C_RDS       0xA78BFA   /* anything decoded from RDS */
#define C_REC       0xF43F5E   /* recording */
#define C_TA        0xF59E0B   /* traffic announcement */
#define C_LIVE      0x34D399   /* the live buffer */

#define F_FREQ    (&lv_font_montserrat_48)
#define F_STATION (&lv_font_montserrat_24)
#define F_UNIT    (&lv_font_montserrat_20)
#define F_TITLE   (&lv_font_montserrat_20)
#define F_BODY    (&lv_font_montserrat_16)
#define F_CAPTION (&lv_font_montserrat_14)

#define MARGIN    12
#define CONTENT_W (RP_SCREEN_W - 2 * MARGIN)
#define TAP_MIN   44

/* ---- state ---------------------------------------------------------------- */

static lv_obj_t *s_screen[RP_SCREEN_COUNT];
static rp_screen_t s_current = RP_SCREEN_NOW;

/* Now Playing */
static lv_obj_t *s_freq, *s_unit, *s_ps, *s_pty, *s_rt;
static lv_obj_t *s_seg[14], *s_rssi_lbl, *s_stereo, *s_ta_badge;
static lv_obj_t *s_af_badge;

/* The band scan's state. Up here with the other cross-screen state rather
   than beside the scan code: the dial draws the band profile it collects, and
   the dial is built earlier in this file. */
static en_scan_t s_scan;
static lv_obj_t *s_w_title, *s_w_artist;

/* Show or hide, in one call. There is no such helper in this file yet and
   three of these read better than six flag calls. */
static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *s_live_fill, *s_live_lbl, *s_rec_dot, *s_rec_lbl;
static lv_obj_t *s_band_lbl, *s_rec_btn_lbl, *s_tl_lbl, *s_tr_lbl;
static lv_obj_t *s_clock;
static lv_obj_t *s_keep_btn, *s_keep_lbl;

/* the simple screen */
static lv_obj_t *s_simple_grid, *s_simple_empty;
static lv_obj_t *s_set_simple, *s_set_wide;
static lv_obj_t *s_preset_note;

/* Forward declarations, because a control on one screen now changes another:
   flagging a preset repaints the simple screen, and turning a screen on
   repaints the settings row and the page dots. */
static void refresh_simple(void);
static void refresh_presets(void);
static void refresh_settings(void);
static void refresh_dots(void);
static void rebuild_order(void);

/* the landscape readout */
static lv_obj_t *s_w_freq, *s_w_unit, *s_w_clock, *s_w_date;
static lv_obj_t *s_w_ps, *s_w_pty, *s_w_rt, *s_w_state;
static lv_obj_t *s_w_seg[20];
static lv_obj_t *s_live_btn, *s_live_head, *s_bar_row;
/* One row of dots per swipe screen, because each screen owns its own copy.
   Indexed [screen][dot]. Held in a table rather than found by walking a
   screen's children looking for small objects, which is what this used to do:
   the landscape readout has twenty 12 x 8 signal segments and that heuristic
   was one layout change away from painting page dots onto a meter. */
static lv_obj_t *s_dots[RP_SWIPE_MAX][RP_SWIPE_MAX];

/* Dial */
static lv_obj_t *s_dial_freq, *s_dial_grid, *s_dial_note;

/* Presets, library, settings */
static lv_obj_t *s_preset_list, *s_library_list;
static lv_obj_t *s_set_af, *s_set_af_note, *s_set_af_save;
static lv_obj_t *s_set_region, *s_set_std, *s_set_backend, *s_set_capture,
                *s_set_ta;
static lv_obj_t *s_adv_list;

/* The register being edited, and its live payload. */
static const en_fm_reg_t *s_reg;
static uint8_t s_reg_val[8];
static lv_obj_t *s_reg_title, *s_reg_name, *s_reg_doc, *s_reg_body,
                *s_reg_hex, *s_reg_revert;

/* ---- small builders ------------------------------------------------------- */

/* A flat panel: no border, no radius, no shadow. Everything in this interface
   is one of these or a label on one. */
static void flat(lv_obj_t *o, uint32_t colour)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);
    flat(o, colour);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static lv_obj_t *hairline(lv_obj_t *parent, int y)
{
    return panel(parent, MARGIN, y, CONTENT_W, 1, C_HAIRLINE);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    return l;
}

/* A label that wraps. LVGL will not wrap without an explicit width, and a label
   that silently runs off the right edge is the single easiest mistake to make
   on a screen this narrow - so paragraphs go through here. */
static lv_obj_t *para(lv_obj_t *parent, const char *text,
                      const lv_font_t *font, uint32_t colour, int w)
{
    lv_obj_t *l = label(parent, text, font, colour);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* A pill: small, uppercase, coloured text on a dark chip. Used for STEREO, RDS,
   TRAFFIC - states worth noticing but not worth a whole row. */
static lv_obj_t *pill(lv_obj_t *parent, int x, int y, const char *text,
                      uint32_t colour)
{
    lv_obj_t *p = lv_obj_create(parent);
    flat(p, C_SURFACE_2);
    lv_obj_set_style_radius(p, 3, 0);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_pad_hor(p, 7, 0);

    lv_obj_t *l = label(p, text, F_CAPTION, colour);
    lv_obj_center(l);
    return p;
}

static void pill_set(lv_obj_t *p, bool on, uint32_t colour)
{
    if (!p) return;
    lv_obj_t *l = lv_obj_get_child(p, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(on ? C_SURFACE_2 : C_SURFACE), 0);
    if (l)
        lv_obj_set_style_text_color(l, lv_color_hex(on ? colour : C_TEXT_MUTE), 0);
}

/* A full-width tappable row. The whole row is the target, not just the text -
   44 px tall, which is the smallest thing a finger hits reliably. */
static lv_obj_t *row(lv_obj_t *parent, int y, int h, lv_event_cb_t cb,
                     void *user)
{
    lv_obj_t *r = panel(parent, 0, y, RP_SCREEN_W, h, C_BG);
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_SURFACE), LV_STATE_PRESSED);
    }
    return r;
}

/* ---- formatting ----------------------------------------------------------- */

static void put_uint(char *out, uint32_t v, int pad)
{
    char tmp[12];
    int n = 0, i = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n < pad) tmp[n++] = '0';
    while (n) out[i++] = tmp[--n];
    out[i] = 0;
}

static void cat(char *dst, const char *src, int cap)
{
    int i = 0;
    while (dst[i] && i < cap - 1) i++;
    while (*src && i < cap - 1) dst[i++] = *src++;
    dst[i] = 0;
}

/* 98500 -> "98.5". One decimal is what a dial shows and what people say. */
static void fmt_mhz(char *out, int cap, uint32_t khz)
{
    char a[12], b[4];
    put_uint(a, khz / 1000u, 0);
    put_uint(b, (khz % 1000u) / 100u, 0);
    out[0] = 0;
    cat(out, a, cap);
    cat(out, ".", cap);
    cat(out, b, cap);
}

/* Milliseconds to m:ss, which is how long a recording or a buffer ever is. */
/* The station's clock as HH:MM, or empty when no 4A group has arrived. Note
   what is NOT done here: nothing is inferred from the device's own clock. A
   blank means the station sends no time, and showing the device's time in its
   place would be a different fact wearing this one's clothes. */
static void fmt_ct(char *out, int cap, const en_rds_t *r)
{
    out[0] = 0;
    if (!r->ct_valid) return;
    put_uint(out, r->ct_hour, 2);
    cat(out, ":", cap);
    char m[8];
    put_uint(m, r->ct_minute, 2);
    cat(out, m, cap);
}

static void fmt_time(char *out, int cap, uint32_t ms)
{
    char a[12], b[4];
    uint32_t s = ms / 1000u;
    put_uint(a, s / 60u, 0);
    put_uint(b, s % 60u, 2);
    out[0] = 0;
    cat(out, a, cap);
    cat(out, ":", cap);
    cat(out, b, cap);
}

/* ---- navigation ----------------------------------------------------------- */

/*
 * The swipe order, built at run time because two of its screens are optional.
 *
 * Kept as an explicit list rather than as "the enum order, skipping the ones
 * that are off": the position of a screen in the sequence is what the dots
 * count and what a gesture steps through, and deriving that twice from two
 * different rules is how they come to disagree.
 */
static rp_screen_t s_order[RP_SWIPE_MAX];
static int         s_order_n;

static void rebuild_order(void)
{
    s_order_n = 0;
    if (rp_model.simple_screen) s_order[s_order_n++] = RP_SCREEN_SIMPLE;
    s_order[s_order_n++] = RP_SCREEN_NOW;
    if (rp_model.wide_screen)   s_order[s_order_n++] = RP_SCREEN_WIDE;
    s_order[s_order_n++] = RP_SCREEN_DIAL;
    s_order[s_order_n++] = RP_SCREEN_PRESETS;
    s_order[s_order_n++] = RP_SCREEN_LIBRARY;
    s_order[s_order_n++] = RP_SCREEN_SETTINGS;
}

int rp_ui_swipe_count(void) { return s_order_n; }

rp_screen_t rp_ui_swipe_at(int i)
{
    if (i < 0 || i >= s_order_n) return RP_SCREEN_NOW;
    return s_order[i];
}

/* Where `s` sits in the sequence, or -1 if it is not in it. */
static int swipe_pos_of(rp_screen_t s)
{
    for (int i = 0; i < s_order_n; i++)
        if (s_order[i] == s) return i;
    return -1;
}

static void build_dots(int screen, lv_obj_t *parent)
{
    /* RP_SWIPE_MAX of them, always. How many are actually shown depends on
       settings and can change while the app runs, and creating them lazily
       would mean creating widgets during a screen change. The extras are
       hidden in refresh_dots. */
    const int gap = 14;
    for (int i = 0; i < RP_SWIPE_MAX; i++) {
        lv_obj_t *d = panel(parent, i * gap, RP_SCREEN_H - 18, 4, 4,
                            C_TEXT_MUTE);
        lv_obj_set_style_radius(d, 2, 0);
        s_dots[screen][i] = d;
    }
}

static void refresh_dots(void)
{
    const int gap = 14;
    const int n = s_order_n;
    const int x0 = (RP_SCREEN_W - (n - 1) * gap) / 2 - 2;
    const int here = swipe_pos_of(s_current);

    if ((int)s_current >= RP_SWIPE_MAX) return;
    lv_obj_t **row_dots = s_dots[(int)s_current];

    for (int i = 0; i < RP_SWIPE_MAX; i++) {
        if (!row_dots[i]) continue;
        if (i >= n) { lv_obj_add_flag(row_dots[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(row_dots[i], LV_OBJ_FLAG_HIDDEN);

        bool on = (i == here);
        /* Re-centred every time, because the row narrows when a screen is
           turned off and dots left where they were would sit off to one side. */
        lv_obj_set_pos(row_dots[i], x0 + i * gap, RP_SCREEN_H - 18);
        lv_obj_set_style_bg_color(row_dots[i],
                                  lv_color_hex(on ? C_TEXT : C_TEXT_MUTE), 0);
        lv_obj_set_size(row_dots[i], on ? 6 : 4, on ? 6 : 4);
    }
}

static void on_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

    int at = swipe_pos_of(s_current);
    if (at < 0) return;          /* not a swipe screen; it has its own way out */

    /* Left and right move through the sequence; the register screens are
       excluded because they belong to settings rather than to it. */
    if (dir == LV_DIR_LEFT)  at++;
    else if (dir == LV_DIR_RIGHT) at--;
    else return;

    if (at < 0) at = 0;
    if (at > s_order_n - 1) at = s_order_n - 1;
    rp_ui_show(rp_ui_swipe_at(at));
}

/* ---- the simple screen ---------------------------------------------------
 *
 * Six buttons and nothing else. Not a cut-down version of Now Playing - a
 * different answer to a different question. Everything else in this app is for
 * finding out what the radio is doing; this is for pressing the station you
 * always press without reading anything.
 *
 * Which six is the user's choice, made on the Presets screen, and it is a
 * choice rather than "the first six" precisely because the interesting
 * presets are rarely the lowest frequencies.
 */

static void on_simple_pick(lv_event_t *e)
{
    rp_act_tune((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void build_simple(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_SIMPLE];

    lv_obj_t *cap = label(s, "PRESETS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    s_simple_grid = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 66, C_BG);

    s_simple_empty = para(s, "", F_BODY, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_simple_empty, MARGIN, 150);
}

static void refresh_simple(void)
{
    lv_obj_clean(s_simple_grid);

    const en_preset_t *pick[EN_SIMPLE_MAX];
    uint8_t n = en_presets_simple(&rp_model.presets, pick, EN_SIMPLE_MAX);

    if (!n) {
        lv_label_set_text(s_simple_empty,
            "No stations chosen yet.\n\n"
            "On the Presets screen, tap the dot beside a station to put it "
            "here.");
        return;
    }
    lv_label_set_text(s_simple_empty, "");

    /* Two columns of 104, three rows of 100. Every target is far larger than
       the 44 px minimum, which is the entire point of the screen. */
    const int bw = 104, bh = 100, gap = 8;
    char buf[64];

    for (uint8_t i = 0; i < n; i++) {
        int col = i % 2, rowi = i / 2;
        int x = MARGIN + col * (bw + gap);
        int y = 8 + rowi * (bh + gap);

        bool tuned = (pick[i]->khz == rp_model.khz);

        lv_obj_t *b = panel(s_simple_grid, x, y, bw, bh,
                            tuned ? C_SURFACE_2 : C_SURFACE);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, on_simple_pick, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)pick[i]->khz);

        /* The tuned one is marked by a bar rather than only by its fill: a
           slightly lighter panel is not a difference you can see across a
           room, which is the distance this screen is designed for. */
        if (tuned) panel(b, 0, 0, bw, 3, C_SIGNAL);

        fmt_mhz(buf, sizeof buf, pick[i]->khz);
        lv_obj_t *f = label(b, buf, F_STATION, C_TEXT);
        lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(f, bw);
        lv_obj_set_pos(f, 0, 22);

        lv_obj_t *nm = label(b, pick[i]->name[0] ? pick[i]->name : "",
                             F_CAPTION, C_RDS);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(nm, bw, 20);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(nm, 0, 58);
    }
}

/* ---- the landscape readout -----------------------------------------------
 *
 * Turn the device counter-clockwise - home button to the right - and this
 * reads the long way: the frequency and the station's own clock across the
 * top, its name under them, and the radio text given the whole width, which
 * is the one thing a 240 px portrait column can never do well. Sixty-four
 * characters of radio text is four cramped lines there and two comfortable
 * ones here.
 *
 * The DISPLAY is not rotated. The framebuffer, the touch mapping and every
 * other screen are exactly as they were; only the items on this screen turn.
 * That is also the only affordable way to do it - rotating one full-screen
 * layer would need a 432 x 240 x 4 buffer, 414 KB out of a 640 KB LVGL heap -
 * and it means the cost is proportional to the text actually shown rather
 * than to the whole panel.
 *
 * Everything below is positioned in LANDSCAPE coordinates: x along the long
 * edge, y across it, origin at the top-left as you hold it turned. The two
 * helpers convert. Getting that conversion wrong in one place and right in
 * another is the whole risk here, so nothing on this screen is positioned by
 * hand.
 */

#define WIDE_W RP_SCREEN_H      /* 432 across the long edge */
#define WIDE_H RP_SCREEN_W      /* 240 across the short one */

/* Landscape rect -> portrait rect. A rectangle turned ninety degrees is still
   an axis-aligned rectangle with its sides swapped, so panels need no
   transform at all - only text does. */
static void wide_box(int lx, int ly, int lw, int lh,
                     int *px, int *py, int *pw, int *ph)
{
    const int lcx = lx + lw / 2, lcy = ly + lh / 2;
    *pw = lh;
    *ph = lw;
    *px = RP_SCREEN_W - lcy - lh / 2;
    *py = lcx - lw / 2;
}

static lv_obj_t *wide_panel(lv_obj_t *parent, int lx, int ly, int lw, int lh,
                            uint32_t colour)
{
    int px, py, pw, ph;
    wide_box(lx, ly, lw, lh, &px, &py, &pw, &ph);
    return panel(parent, px, py, pw, ph, colour);
}

/* A label laid out in landscape and drawn turned. The object keeps its natural
   horizontal box - so wrapping, eliding and alignment all work normally - and
   only the drawing is rotated, about the box's own centre. */
static lv_obj_t *wide_label(lv_obj_t *parent, int lx, int ly, int lw, int lh,
                            const char *txt, const lv_font_t *f, uint32_t col)
{
    const int lcx = lx + lw / 2, lcy = ly + lh / 2;

    lv_obj_t *o = label(parent, txt, f, col);
    lv_obj_set_size(o, lw, lh);
    lv_obj_set_pos(o, RP_SCREEN_W - lcy - lw / 2, lcx - lh / 2);

    lv_obj_set_style_transform_pivot_x(o, lw / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, lh / 2, 0);
    lv_obj_set_style_transform_rotation(o, 900, 0);   /* tenths of a degree */
    return o;
}

static void build_wide(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_WIDE];

    /* The frequency, given the height it can only have this way round. */
    s_w_freq = wide_label(s, 16, 14, 220, 54, "--.-", F_FREQ, C_TEXT);
    s_w_unit = wide_label(s, 16, 68, 220, 20, "MHz", F_CAPTION, C_TEXT_MUTE);

    /* The station's clock, from group 4A, at the same weight as the
       frequency - it is the other number you would look across a room to
       read. Blank when the station sends no time. */
    s_w_clock = wide_label(s, 250, 14, 166, 54, "", F_FREQ, C_RDS);
    lv_obj_set_style_text_align(s_w_clock, LV_TEXT_ALIGN_RIGHT, 0);

    s_w_date = wide_label(s, 250, 68, 166, 20, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_w_date, LV_TEXT_ALIGN_RIGHT, 0);

    wide_panel(s, 16, 94, WIDE_W - 32, 1, C_HAIRLINE);

    s_w_ps = wide_label(s, 16, 100, 240, 28, "", F_STATION, C_RDS);
    s_w_pty = wide_label(s, 260, 106, 156, 20, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_w_pty, LV_TEXT_ALIGN_RIGHT, 0);

    /*
     * Radio text, across the whole long edge and given three lines.
     *
     * Two at 16 pt was the first attempt and it clipped: 400 px of this face
     * holds about thirty-two characters, so a full 64-character message needs
     * more than two lines and the tail was simply cut off. Three lines of the
     * caption face hold the longest message the standard allows with room to
     * spare, which is the point of having the long edge at all.
     */
    s_w_rt = wide_label(s, 16, 134, WIDE_W - 32, 58, "", F_CAPTION, C_TEXT_DIM);
    lv_label_set_long_mode(s_w_rt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_w_rt, 3, 0);

    /*
     * RadioText+, when the station sends it: the title and the artist pulled
     * out of that same text as separate fields.
     *
     * Laid over the radio text rather than beside it. The two are the same
     * characters - RT+ is markers INTO the RT buffer, not a second message -
     * so showing both would be showing the same words twice, and the tidy
     * version is the one worth the long edge. The raw text comes back the
     * moment RT+ stops arriving.
     */
    s_w_title = wide_label(s, 16, 134, WIDE_W - 32, 30, "", F_STATION, C_TEXT);
    lv_label_set_long_mode(s_w_title, LV_LABEL_LONG_DOT);
    s_w_artist = wide_label(s, 16, 166, WIDE_W - 32, 24, "", F_BODY, C_RDS);
    lv_label_set_long_mode(s_w_artist, LV_LABEL_LONG_DOT);

    /* Signal, as plain rectangles - no transform needed for those. */
    for (int i = 0; i < 20; i++)
        s_w_seg[i] = wide_panel(s, 16 + i * 12, 206, 8, 12, C_SURFACE_2);

    s_w_state = wide_label(s, 280, 202, 136, 20, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_w_state, LV_TEXT_ALIGN_RIGHT, 0);
}

static void refresh_wide(void)
{
    char buf[96], n[16];

    fmt_mhz(buf, sizeof buf, rp_model.khz);
    lv_label_set_text(s_w_freq, rp_model.khz ? buf : "--.-");

    fmt_ct(buf, sizeof buf, &rp_model.rds);
    lv_label_set_text(s_w_clock, buf);

    /* The date comes from the same group as the time, so it is shown only
       when that group has arrived - never reconstructed from anything else. */
    buf[0] = 0;
    if (rp_model.rds.ct_valid) {
        put_uint(buf, rp_model.rds.ct_year, 0);
        cat(buf, "-", sizeof buf);
        put_uint(n, rp_model.rds.ct_month, 2);
        cat(buf, n, sizeof buf);
        cat(buf, "-", sizeof buf);
        put_uint(n, rp_model.rds.ct_day, 2);
        cat(buf, n, sizeof buf);
    }
    lv_label_set_text(s_w_date, buf);

    lv_label_set_text(s_w_ps, rp_model.rds.ps_valid ? rp_model.rds.ps : "");

    /* Tidy or raw, never both: RT+ is markers into the radio text, so showing
       the two together would be showing the same words twice. */
    bool tidy = rp_model.rds.rt_title_valid || rp_model.rds.rt_artist_valid;
    show(s_w_title, tidy);
    show(s_w_artist, tidy);
    show(s_w_rt, !tidy);
    if (tidy) {
        lv_label_set_text(s_w_title,
                          rp_model.rds.rt_title_valid ? rp_model.rds.rt_title
                                                      : "");
        lv_label_set_text(s_w_artist,
                          rp_model.rds.rt_artist_valid ? rp_model.rds.rt_artist
                                                       : "");
    }

    buf[0] = 0;
    if (rp_model.rds.pi_valid)
        cat(buf, en_rds_pty_name(rp_model.rds.pty, rp_model.rds.rbds),
            sizeof buf);
    lv_label_set_text(s_w_pty, buf);

    if (!rp_model.tuner_ok && rp_model.tuner_note) {
        lv_label_set_text(s_w_rt, rp_model.tuner_note);
        lv_obj_set_style_text_color(s_w_rt, lv_color_hex(C_TA), 0);
    } else {
        lv_label_set_text(s_w_rt, rp_model.rds.rt_valid ? rp_model.rds.rt : "");
        lv_obj_set_style_text_color(s_w_rt, lv_color_hex(C_TEXT_DIM), 0);
    }

    int lit = (int)((rp_model.rssi * 20u) / 96u);
    if (lit > 20) lit = 20;
    for (int i = 0; i < 20; i++) {
        uint32_t c = C_SURFACE_2;
        if (i < lit) c = (i < 5) ? C_TA : C_SIGNAL;
        lv_obj_set_style_bg_color(s_w_seg[i], lv_color_hex(c), 0);
    }

    /* One line for everything that is a state rather than a reading. */
    buf[0] = 0;
    if (rp_model.recording) cat(buf, "REC  ", sizeof buf);
    if (rp_model.rds.ta)    cat(buf, "TRAFFIC  ", sizeof buf);
    if (rp_model.stereo)    cat(buf, "STEREO", sizeof buf);
    lv_label_set_text(s_w_state, buf);
    lv_obj_set_style_text_color(s_w_state,
                                lv_color_hex(rp_model.recording ? C_REC
                                                                : C_TEXT_MUTE),
                                0);
}

/* ---- Now Playing ---------------------------------------------------------- */

/* True when the headphones are carrying the radio as it happens. Everything
   contextual on this screen turns on this one question. */
static bool at_live(void)
{
    return !rp_model.play_file && rp_model.behind_ms == 0;
}

static void on_left(lv_event_t *e)
{
    (void)e;
    if (at_live()) rp_act_seek(false);
    else rp_act_nudge(-15000);
}

static void on_right(lv_event_t *e)
{
    (void)e;
    if (at_live()) rp_act_seek(true);
    else rp_act_nudge(15000);
}

static void on_middle(lv_event_t *e)
{
    (void)e;
    if (at_live()) rp_act_record_toggle();
    else rp_act_pause_toggle();
}

static void on_go_live(lv_event_t *e) { (void)e; rp_act_play_live(); }

/* How long the KEEP button says it saved something, in refresh ticks. The
   model gains a recording in the library and nothing else changes on this
   screen, so without a word here the tap looks like it did nothing. */
static uint8_t s_kept_for;

static void on_keep(lv_event_t *e)
{
    (void)e;
    if (!rp_model.capture_ok || !rp_model.live_ms) return;
    rp_act_save_live(rp_model.live_ms);
    s_kept_for = 8;
}

/* Tapping the buffer bar seeks to that point, which is how every buffered
   radio behaves and saves inventing a control for entering the scrub. */
static void on_bar_tap(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);

    int32_t x = p.x - MARGIN;
    if (x < 0) x = 0;
    if (x > CONTENT_W) x = CONTENT_W;

    if (rp_model.play_file) {
        if (rp_model.play_len_ms)
            rp_act_nudge((int32_t)((int64_t)x * rp_model.play_len_ms / CONTENT_W)
                         - (int32_t)rp_model.play_pos_ms);
        return;
    }

    /* The bar runs oldest on the left to live on the right, so distance from
       the right edge is how far behind the tap is. */
    uint32_t span = rp_model.behind_max_ms;
    if (!span) return;
    uint32_t behind = (uint32_t)((int64_t)(CONTENT_W - x) * span / CONTENT_W);
    rp_act_nudge((int32_t)rp_model.behind_ms - (int32_t)behind);
}

/*
 * Tap the stereo pill to cycle auto -> mono -> stereo.
 *
 * Forced mono is the one worth having: a weak station is steadier in mono
 * than blending in and out of stereo every few seconds, and that judgement is
 * the listener's rather than the chip's.
 *
 * The pill then shows two things at once, deliberately. Its LABEL is the mode
 * that was asked for; its COLOUR stays driven by the chip's own "stereo
 * active" flag. Which way round the manual-select bit runs is read off the
 * bit's name in the register table rather than pinned down by the bring-up
 * sequence, so being able to ask for mono and see the pill stay lit is how
 * that gets found out - immediately, and by looking.
 */
/* ---- following the station ------------------------------------------------
 *
 * Pumped from the frame callback like the scan and the recording timer, so it
 * keeps working while the user is on another screen - which is the entire
 * point of a feature for driving out of range.
 */
static void af_pump(void)
{
    static uint32_t last_ms;
    if (!rp_model.af.enabled) { last_ms = lv_tick_get(); return; }

    uint32_t now = lv_tick_get();
    uint32_t dt = now - last_ms;
    last_ms = now;
    if (dt > 1000) dt = 0;      /* first tick, or a clock that jumped */

    uint32_t khz = 0;
    if (en_af_tick(&rp_model.af, dt, rp_model.khz, rp_model.rssi,
                   &rp_model.rds, &khz) == EN_AF_GOTO)
        rp_act_tune_quiet(khz);
}

/*
 * Tapping the badge turns following off.
 *
 * The badge is the control on purpose. The situation this feature can get
 * wrong is standing near a transmitter advertising frequencies that are not
 * what it says they are, and in that situation the thing showing you it is
 * happening should be the thing that stops it - not a switch three screens
 * away in a settings list.
 */
static void on_af_tap(lv_event_t *e)
{
    (void)e;
    rp_act_af_follow(!rp_model.af.enabled);
}

static void on_stereo_tap(lv_event_t *e)
{
    (void)e;
    rp_act_stereo_mode((uint8_t)((rp_model.stereo_mode + 1u) % 3u));
}

static const char *stereo_mode_text(uint8_t mode)
{
    switch (mode) {
    case EN_FM_STEREO_MONO:   return "MONO";
    case EN_FM_STEREO_STEREO: return "ST FORCED";
    default:                  return "STEREO";
    }
}

static void build_now(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_NOW];

    /* Status strip. Band and standard on the left, recording on the right -
       the two things that are true of the whole screen rather than of any one
       element on it. */
    s_band_lbl = label(s, "FM", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(s_band_lbl, MARGIN, 10);

    /* The station's own clock, from group 4A. It was being decoded and thrown
       away. Shown only once a 4A has actually arrived, so an empty space here
       means the station sends no time rather than that the time is midnight.

       Right-aligned rather than centred: centred put it straight through
       "Americas RBDS", and the band label is the one thing on this strip whose
       width depends on data. It shares the slot with the recording readout and
       yields to it - see refresh_now - because while you are recording, that
       the recording is running matters more than what time it is. */
    s_clock = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_clock, 56);
    lv_obj_set_pos(s_clock, RP_SCREEN_W - MARGIN - 56, 10);

    s_rec_dot = panel(s, RP_SCREEN_W - MARGIN - 52, 14, 7, 7, C_REC);
    lv_obj_set_style_radius(s_rec_dot, 4, 0);
    lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);

    s_rec_lbl = label(s, "0:00", F_CAPTION, C_REC);
    lv_obj_set_pos(s_rec_lbl, RP_SCREEN_W - MARGIN - 40, 10);
    lv_obj_add_flag(s_rec_lbl, LV_OBJ_FLAG_HIDDEN);

    hairline(s, 32);

    /* The frequency. Fixed position and fixed size - it is the one thing that
       must be findable without reading, so nothing below is allowed to move
       it. The unit sits on its baseline rather than beside its centre. */
    s_freq = label(s, "--.-", F_FREQ, C_TEXT);
    lv_obj_set_style_text_align(s_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_freq, CONTENT_W - 40);
    lv_obj_set_pos(s_freq, MARGIN, 46);

    s_unit = label(s, "MHz", F_UNIT, C_TEXT_MUTE);
    lv_obj_set_pos(s_unit, RP_SCREEN_W - MARGIN - 42, 82);

    /* Station name, from RDS. Falls back to nothing rather than to a
       placeholder: an empty line reads as "not yet", where "Unknown" reads as
       a decode that failed. */
    s_ps = para(s, "", F_STATION, C_RDS, CONTENT_W);
    lv_obj_set_style_text_align(s_ps, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ps, MARGIN, 118);

    s_pty = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_pty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_pty, CONTENT_W);
    lv_obj_set_pos(s_pty, MARGIN, 152);

    hairline(s, 178);

    /* Radio text. Three lines is enough for almost every message and keeps the
       block from pushing the transport around when a station is chatty. */
    s_rt = para(s, "", F_CAPTION, C_TEXT_DIM, CONTENT_W);
    lv_obj_set_pos(s_rt, MARGIN, 190);
    lv_obj_set_style_text_line_space(s_rt, 4, 0);
    lv_label_set_long_mode(s_rt, LV_LABEL_LONG_WRAP);

    /* Signal. A segmented meter rather than a number, because the number means
       nothing to anyone and the shape means everything - you can see at a
       glance whether moving the headphone cable helped. */
    /* Pills first, on their own row. They were beside the meter and overlapped
       it - the meter is 176 px wide and TRAFFIC ran off the right edge - which
       is the sort of thing only a rendered screen shows you. */
    s_af_badge = pill(s, MARGIN + 150, 258, "AF", C_RDS);
    lv_obj_add_flag(s_af_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_af_badge, on_af_tap, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_af_badge, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);

    s_stereo = pill(s, MARGIN, 258, "STEREO", C_SIGNAL);
    /* A pill is normally a readout. This one is also a control, so it gets a
       target the size of the chip rather than the size of the text. */
    lv_obj_add_flag(s_stereo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_stereo, on_stereo_tap, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_stereo, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    s_ta_badge = pill(s, MARGIN + 76, 258, "TRAFFIC", C_TA);

    for (int i = 0; i < 14; i++) {
        s_seg[i] = panel(s, MARGIN + i * 12, 288, 8, 14, C_SURFACE_2);
        lv_obj_set_style_radius(s_seg[i], 1, 0);
    }
    s_rssi_lbl = label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(s_rssi_lbl, MARGIN, 306);

    hairline(s, 322);

    /* The live buffer. Shown as how much has been captured rather than as a
       scrubber with a handle: there is nothing to drag to yet, and a control
       that looks draggable and is not is worse than a readout. */
    s_tl_lbl = label(s, "", F_CAPTION, C_LIVE);
    lv_obj_set_pos(s_tl_lbl, MARGIN, 328);

    /* Narrow enough to clear both buttons to its right. It used to stop only
       at LIVE; KEEP now sits beside LIVE and the readout ran underneath it. */
    s_live_lbl = label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(s_live_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_live_lbl, CONTENT_W - 108);
    lv_obj_set_pos(s_live_lbl, MARGIN, 328);

    /*
     * Keep the buffer. rp_act_save_live has worked since the capture backend
     * landed and nothing ever called it, so "record the thing that already
     * happened" - the one feature a timeshift buffer exists for - was written
     * and unreachable.
     *
     * It saves the whole buffer rather than asking how much. The buffer is
     * bounded at a few minutes by the setting that allocated it, and a length
     * picker is a decision to make afterwards, in a file manager, not in the
     * two seconds before the moment falls off the end.
     */
    s_keep_btn = panel(s, RP_SCREEN_W - MARGIN - 100, 322, 50, 26, C_SURFACE_2);
    lv_obj_set_style_radius(s_keep_btn, 3, 0);
    lv_obj_add_flag(s_keep_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_keep_btn, on_keep, LV_EVENT_CLICKED, 0);
    lv_obj_set_ext_click_area(s_keep_btn, 12);
    s_keep_lbl = label(s_keep_btn, "KEEP", F_CAPTION, C_REC);
    lv_obj_center(s_keep_lbl);

    /* Shown only when it would do something. A LIVE button that is already
       live is a button that teaches you it does nothing. */
    s_live_btn = panel(s, RP_SCREEN_W - MARGIN - 46, 322, 46, 26, C_SURFACE_2);
    lv_obj_set_style_radius(s_live_btn, 3, 0);
    lv_obj_add_flag(s_live_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_live_btn, on_go_live, LV_EVENT_CLICKED, 0);
    lv_obj_set_ext_click_area(s_live_btn, 12);
    lv_obj_center(label(s_live_btn, "LIVE", F_CAPTION, C_LIVE));

    /* The bar is inside a taller row so the whole strip is tappable - a 3 px
       target would be a decoration, not a control. */
    s_bar_row = panel(s, 0, 344, RP_SCREEN_W, 22, C_BG);
    lv_obj_add_flag(s_bar_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_bar_row, on_bar_tap, LV_EVENT_CLICKED, 0);

    panel(s_bar_row, MARGIN, 8, CONTENT_W, 3, C_SURFACE_2);
    s_live_fill = panel(s_bar_row, MARGIN, 8, 0, 3, C_LIVE);
    s_live_head = panel(s_bar_row, MARGIN, 4, 3, 11, C_TEXT);

    /* Transport. Three targets across the full width, each well over the
       minimum, with the record button given the middle and the most weight. */
    const int ty = 362, th = 50;

    lv_obj_t *dn = panel(s, MARGIN, ty, 64, th, C_SURFACE);
    lv_obj_add_flag(dn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dn, on_left, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(dn, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    s_tl_lbl = label(dn, LV_SYMBOL_PREV, F_BODY, C_TEXT);
    lv_obj_center(s_tl_lbl);

    lv_obj_t *rec = panel(s, MARGIN + 72, ty, 72, th, C_SURFACE);
    lv_obj_add_flag(rec, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rec, on_middle, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(rec, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    s_rec_btn_lbl = label(rec, "REC", F_BODY, C_REC);
    lv_obj_center(s_rec_btn_lbl);

    lv_obj_t *up = panel(s, MARGIN + 152, ty, 64, th, C_SURFACE);
    lv_obj_add_flag(up, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(up, on_right, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    s_tr_lbl = label(up, LV_SYMBOL_NEXT, F_BODY, C_TEXT);
    lv_obj_center(s_tr_lbl);
}

static void refresh_now(void)
{
    char buf[96];

    fmt_mhz(buf, sizeof buf, rp_model.khz);
    lv_label_set_text(s_freq, rp_model.khz ? buf : "--.-");

    /* Band and standard. Worth showing because they change what everything
       else means - a programme type is a different word in each standard. */
    buf[0] = 0;
    cat(buf, rp_model.region ? rp_model.region->name : "FM", sizeof buf);
    cat(buf, "  ", sizeof buf);
    cat(buf, rp_model.rds.rbds ? "RBDS" : "RDS", sizeof buf);
    lv_label_set_text(s_band_lbl, buf);

    fmt_ct(buf, sizeof buf, &rp_model.rds);
    lv_label_set_text(s_clock, rp_model.recording ? "" : buf);

    lv_label_set_text(s_ps, rp_model.rds.ps_valid ? rp_model.rds.ps : "");

    /* Programme type, and the refinement if the station sends one. */
    buf[0] = 0;
    if (rp_model.rds.pi_valid) {
        cat(buf, en_rds_pty_name(rp_model.rds.pty, rp_model.rds.rbds),
            sizeof buf);
        if (rp_model.rds.ptyn_valid) {
            cat(buf, "  ", sizeof buf);
            cat(buf, rp_model.rds.ptyn, sizeof buf);
        }
    }
    lv_label_set_text(s_pty, buf);

    /* With no tuner there is no radio text and never will be, so the space
       says why instead of staying blank. A dead radio that explains nothing is
       indistinguishable from a broken app. */
    if (!rp_model.tuner_ok && rp_model.tuner_note) {
        lv_label_set_text(s_rt, rp_model.tuner_note);
        lv_obj_set_style_text_color(s_rt, lv_color_hex(C_TA), 0);
    } else {
        lv_label_set_text(s_rt, rp_model.rds.rt_valid ? rp_model.rds.rt : "");
        lv_obj_set_style_text_color(s_rt, lv_color_hex(C_TEXT_DIM), 0);
    }

    /* The meter. RSSI is reported over the full byte range but everything
       real lives in the bottom third, so the scale is compressed to where the
       signal actually is rather than wasting most of the bar. */
    int lit = (int)((rp_model.rssi * 14u) / 96u);
    if (lit > 14) lit = 14;
    for (int i = 0; i < 14; i++) {
        uint32_t c = C_SURFACE_2;
        if (i < lit) c = (i < 4) ? C_TA : C_SIGNAL;   /* weak reads amber */
        lv_obj_set_style_bg_color(s_seg[i], lv_color_hex(c), 0);
    }

    buf[0] = 0;
    cat(buf, "RSSI ", sizeof buf);
    char n[12];
    put_uint(n, rp_model.rssi, 0);
    cat(buf, n, sizeof buf);
    if (rp_model.snr) {
        cat(buf, "   SNR ", sizeof buf);
        put_uint(n, (uint32_t)(rp_model.snr < 0 ? -rp_model.snr : rp_model.snr), 0);
        if (rp_model.snr < 0) cat(buf, "-", sizeof buf);
        cat(buf, n, sizeof buf);
    }
    lv_label_set_text(s_rssi_lbl, buf);

    {
        /* Label says what was asked for; colour says what the chip is doing.
           Forced mono is called out in amber because it is a choice that
           stays made, and one the user should be able to see they made. */
        lv_obj_t *sl = lv_obj_get_child(s_stereo, 0);
        if (sl) lv_label_set_text(sl, stereo_mode_text(rp_model.stereo_mode));
        pill_set(s_stereo, rp_model.stereo,
                 rp_model.stereo_mode == EN_FM_STEREO_AUTO ? C_SIGNAL : C_TA);
    }

    if (s_af_badge) {
        /* The label says what it is doing - watching, weak, trying, waiting -
           so a retune the listener did not ask for has something on screen
           that already explained itself. */
        lv_obj_t *al = lv_obj_get_child(s_af_badge, 0);
        if (al) lv_label_set_text(al, en_af_state_text(&rp_model.af));
        pill_set(s_af_badge, rp_model.af.enabled,
                 rp_model.af.phase == EN_AF_TRYING ? C_TA : C_RDS);
    }
    pill_set(s_ta_badge, rp_model.rds.ta, C_TA);

    /* The buffer strip. How much has been captured, and where in it the
       headphones are - which is the same picture whether that is the live edge
       or ten minutes back. */
    bool live = at_live();

    int w = 0, head = 0;
    if (rp_model.play_file) {
        if (rp_model.play_len_ms)
            head = (int)((uint64_t)rp_model.play_pos_ms * CONTENT_W
                         / rp_model.play_len_ms);
        w = CONTENT_W;
    } else {
        if (rp_model.live_cap_ms)
            w = (int)((uint64_t)rp_model.live_ms * CONTENT_W
                      / rp_model.live_cap_ms);
        head = w;
        if (rp_model.behind_max_ms && rp_model.behind_ms)
            head = w - (int)((uint64_t)rp_model.behind_ms * w
                             / rp_model.behind_max_ms);
    }
    if (w > CONTENT_W) w = CONTENT_W;
    if (head < 0) head = 0;
    if (head > CONTENT_W) head = CONTENT_W;

    lv_obj_set_width(s_live_fill, w);
    lv_obj_set_pos(s_live_head, MARGIN + head - 1, 4);
    lv_obj_set_style_bg_color(s_live_head,
                              lv_color_hex(live ? C_LIVE : C_TEXT), 0);

    /* Left of the row says what is playing; right says how much there is. */
    if (rp_model.play_file) {
        lv_label_set_text(s_tl_lbl, "");
        buf[0] = 0;
        fmt_time(n, sizeof n, rp_model.play_pos_ms);
        cat(buf, n, sizeof buf);
        cat(buf, " / ", sizeof buf);
        fmt_time(n, sizeof n, rp_model.play_len_ms);
        cat(buf, n, sizeof buf);
        lv_label_set_text(s_live_lbl, buf);
    } else if (!rp_model.capture_ok) {
        lv_label_set_text(s_live_lbl, "no capture");
    } else if (live) {
        buf[0] = 0;
        fmt_time(n, sizeof n, rp_model.live_ms);
        cat(buf, n, sizeof buf);
        cat(buf, " buffered", sizeof buf);
        lv_label_set_text(s_live_lbl, buf);
    } else {
        buf[0] = 0;
        cat(buf, "-", sizeof buf);
        fmt_time(n, sizeof n, rp_model.behind_ms);
        cat(buf, n, sizeof buf);
        cat(buf, " behind", sizeof buf);
        lv_label_set_text(s_live_lbl, buf);
    }

    if (live || !rp_model.capture_ok)
        lv_obj_add_flag(s_live_btn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(s_live_btn, LV_OBJ_FLAG_HIDDEN);

    /* KEEP needs something in the buffer to keep, and is pointless while a
       recording is already running - that is capturing the same audio. */
    if (rp_model.capture_ok && rp_model.live_ms && !rp_model.recording &&
        !rp_model.play_file)
        lv_obj_remove_flag(s_keep_btn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_keep_btn, LV_OBJ_FLAG_HIDDEN);

    if (s_kept_for) {
        s_kept_for--;
        lv_label_set_text(s_keep_lbl, "SAVED");
        lv_obj_set_style_text_color(s_keep_lbl, lv_color_hex(C_LIVE), 0);
    } else {
        lv_label_set_text(s_keep_lbl, "KEEP");
        lv_obj_set_style_text_color(s_keep_lbl, lv_color_hex(C_REC), 0);
    }

    /* No capture means no buffer to scrub and nothing to record. The strip and
       the record button are hidden rather than left as controls that quietly
       do nothing when pressed. */
    if (rp_model.capture_ok) lv_obj_remove_flag(s_bar_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bar_row, LV_OBJ_FLAG_HIDDEN);

    /* Recording. The dot and the elapsed time appear together and only while
       recording; a permanent readout showing 0:00 would be noise. */
    if (rp_model.recording) {
        lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rec_lbl, LV_OBJ_FLAG_HIDDEN);
        buf[0] = 0;
        if (rp_model.ta_recording) cat(buf, "TA ", sizeof buf);
        fmt_time(n, sizeof n, rp_model.rec_ms);
        cat(buf, n, sizeof buf);
        lv_label_set_text(s_rec_lbl, buf);
    } else {
        lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rec_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* The transport means different things depending on where the headphones
       are, so it says so rather than leaving the user to find out. */
    if (live) {
        lv_label_set_text(s_tl_lbl, LV_SYMBOL_PREV);
        lv_label_set_text(s_tr_lbl, LV_SYMBOL_NEXT);
        if (rp_model.capture_ok) {
            lv_label_set_text(s_rec_btn_lbl,
                              rp_model.recording ? "STOP" : "REC");
            lv_obj_set_style_text_color(s_rec_btn_lbl, lv_color_hex(C_REC), 0);
        } else {
            /* Greyed and inert, not hidden: the button leaving a hole would
               make the row look broken rather than limited. */
            lv_label_set_text(s_rec_btn_lbl, "REC");
            lv_obj_set_style_text_color(s_rec_btn_lbl,
                                        lv_color_hex(C_TEXT_MUTE), 0);
        }
    } else {
        lv_label_set_text(s_tl_lbl, LV_SYMBOL_LEFT " 15");
        lv_label_set_text(s_tr_lbl, "15 " LV_SYMBOL_RIGHT);
        lv_label_set_text(s_rec_btn_lbl,
                          rp_model.play_paused ? LV_SYMBOL_PLAY
                                               : LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(s_rec_btn_lbl, lv_color_hex(C_TEXT), 0);
    }
}

/* ---- Dial ----------------------------------------------------------------- */

static void on_step_down(lv_event_t *e) { (void)e; rp_act_step(false); }
static void on_step_up(lv_event_t *e)   { (void)e; rp_act_step(true); }
static void on_preset_here(lv_event_t *e) { (void)e; rp_act_preset_toggle(); }

static void build_dial(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_DIAL];

    lv_obj_t *cap = label(s, "TUNE", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    s_dial_freq = label(s, "--.-", F_FREQ, C_TEXT);
    lv_obj_set_style_text_align(s_dial_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_dial_freq, CONTENT_W);
    lv_obj_set_pos(s_dial_freq, MARGIN, 52);

    s_dial_note = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_dial_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_dial_note, CONTENT_W);
    lv_obj_set_pos(s_dial_note, MARGIN, 118);

    /* A strip of the band around where we are. Not interactive - it is an
       orientation aid, showing whether there is anywhere to go next. */
    s_dial_grid = panel(s, 0, 150, RP_SCREEN_W, 46, C_BG);

    hairline(s, 210);

    /* Single steps, distinct from the seek on the Now screen: seek jumps to
       the next station, step moves one channel whether there is anything
       there or not. Both are wanted, and confusing them is annoying. */
    lv_obj_t *dn = panel(s, MARGIN, 240, CONTENT_W / 2 - 6, TAP_MIN + 18,
                         C_SURFACE);
    lv_obj_add_flag(dn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dn, on_step_down, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(dn, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    lv_obj_center(label(dn, "-", F_TITLE, C_TEXT));

    lv_obj_t *up = panel(s, MARGIN + CONTENT_W / 2 + 6, 240,
                         CONTENT_W / 2 - 6, TAP_MIN + 18, C_SURFACE);
    lv_obj_add_flag(up, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(up, on_step_up, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    lv_obj_center(label(up, "+", F_TITLE, C_TEXT));

    lv_obj_t *save = panel(s, MARGIN, 322, CONTENT_W, TAP_MIN + 18, C_SURFACE);
    lv_obj_add_flag(save, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save, on_preset_here, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(C_SURFACE_2), LV_STATE_PRESSED);
    lv_obj_center(label(save, "Save as preset", F_BODY, C_TEXT));

}

static void refresh_dial(void)
{
    char buf[64], n[16];
    fmt_mhz(buf, sizeof buf, rp_model.khz);
    lv_label_set_text(s_dial_freq, rp_model.khz ? buf : "--.-");

    buf[0] = 0;
    if (rp_model.region) {
        fmt_mhz(n, sizeof n, rp_model.region->low_khz);
        cat(buf, n, sizeof buf);
        cat(buf, " - ", sizeof buf);
        fmt_mhz(n, sizeof n, rp_model.region->high_khz);
        cat(buf, n, sizeof buf);
        cat(buf, " MHz", sizeof buf);
        cat(buf, "   ", sizeof buf);
        put_uint(n, rp_model.region->step_khz, 0);
        cat(buf, n, sizeof buf);
        cat(buf, " kHz", sizeof buf);

        /* Said once, where the picture is: a sweep that jumped between
           stations knows nothing about the space between them, and a strip
           that looked continuous would be claiming otherwise. */
        if (s_scan.profile_n && s_scan.phase != EN_SCAN_SWEEP)
            cat(buf, s_scan.profile_sparse ? "   stations only" : "   scanned",
                sizeof buf);
    }
    lv_label_set_text(s_dial_note, buf);

    /* Redraw the band strip: a tick per preset, and a marker where we are, so
       the strip says something rather than being decoration. */
    lv_obj_clean(s_dial_grid);
    if (!rp_model.region) return;

    uint32_t lo = rp_model.region->low_khz, hi = rp_model.region->high_khz;
    if (hi <= lo) return;

    /*
     * The band profile, if a scan has been run: one bar per channel, as tall
     * as the signal there was.
     *
     * Drawn under everything else and in the surface colour, so it reads as
     * ground rather than as data competing with the preset ticks. The bars
     * are one pixel wide and drawn only where the profile has something,
     * which after a seeking sweep is only at the stations - joining those up
     * would draw valleys nobody measured.
     */
    if (s_scan.profile_n) {
        /* Standing on the hairline at y = 22 and growing upward, so the
           landscape and the preset ticks share one baseline and read as one
           strip rather than two things at different heights. */
        const int base = 22, max_h = 18;
        for (uint16_t i = 0; i < s_scan.profile_n; i++) {
            uint8_t v = s_scan.profile[i];
            if (!v) continue;
            int h = (v * max_h) / 255;
            if (h < 1) h = 1;
            int x = MARGIN + (int)((uint32_t)i * (uint32_t)CONTENT_W /
                                   s_scan.profile_n);
            /* As wide as a channel is on screen, so the floor reads as a
               floor rather than as a dotted line with gaps in it. */
            int w = CONTENT_W / (int)s_scan.profile_n;
            if (w < 1) w = 1;
            panel(s_dial_grid, x, base - h, w, h,
                  s_scan.profile_sparse ? C_RDS : C_TEXT_MUTE);
        }
    }

    panel(s_dial_grid, MARGIN, 22, CONTENT_W, 1, C_HAIRLINE);

    for (uint8_t i = 0; i < rp_model.presets.count; i++) {
        uint32_t k = rp_model.presets.list[i].khz;
        if (k < lo || k > hi) continue;
        int x = MARGIN + (int)((uint64_t)(k - lo) * CONTENT_W / (hi - lo));
        panel(s_dial_grid, x, 14, 1, 9, C_RDS);
    }

    if (rp_model.khz >= lo && rp_model.khz <= hi) {
        int x = MARGIN + (int)((uint64_t)(rp_model.khz - lo) * CONTENT_W
                               / (hi - lo));
        panel(s_dial_grid, x - 1, 8, 3, 22, C_SIGNAL);
    }
}

/* ---- band scan ------------------------------------------------------------
 *
 * The state machine is in core/scan.c and knows nothing about tuners or
 * screens; this is the part that turns "it wants to be on 90.1" into a tune
 * and "it is 40% through" into a bar.
 *
 * Pumped from rp_ui_tick, which runs whatever screen is showing, so walking
 * away from the Presets screen mid-scan does not abandon it - which matters,
 * because the whole point of a scan is that you can stop watching it.
 */

static uint32_t  s_scan_last_ms;
static uint8_t   s_scan_added;
static uint8_t   s_scan_note_for;   /* refresh ticks the result stays up */

/* An RSSI a station has to beat. The tuner reports 0..255 and an empty
   channel on this chip sits in the low teens, so this is well clear of the
   noise floor without being so high that a weak local is missed. */
#define RP_SCAN_RSSI 90

static bool scan_running(void)
{
    return s_scan.phase == EN_SCAN_SWEEP || s_scan.phase == EN_SCAN_NAMING;
}

static void scan_pump(void)
{
    if (!scan_running()) return;

    uint32_t now = lv_tick_get();
    uint32_t dt = now - s_scan_last_ms;
    s_scan_last_ms = now;
    /* First tick after starting, or a clock that jumped. Charge nothing
       rather than a garbage interval that would skip a channel. */
    if (dt > 1000) dt = 0;

    uint32_t khz = 0;
    switch (en_scan_tick(&s_scan, dt, rp_model.khz, rp_model.rssi,
                         &rp_model.rds, &khz)) {
    case EN_SCAN_TUNE:
        rp_act_tune_quiet(khz);
        break;
    case EN_SCAN_SEEK:
        /* The chip crosses the empty band for us. If the platform cannot,
           say so once and the sweep steps the rest in software rather than
           waiting out a timeout on every station. */
        if (!rp_act_seek_quiet(true)) en_scan_seek_failed(&s_scan);
        break;
    default:
        break;
    }

    if (s_scan.phase == EN_SCAN_DONE) {
        rp_act_squelch(false);
        s_scan_added = en_scan_commit(&s_scan, &rp_model.presets);
        rp_act_presets_save();
        s_scan_note_for = 60;
        s_scan.phase = EN_SCAN_IDLE;
        /* Back where the user was, and through the saving path this time so
           the frequency they are left on is the one that persists. */
        rp_act_tune(s_scan.resume_khz);
    }
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    if (scan_running()) {
        /* Cancel. Put the tuner back before anything else, so a cancelled
           scan costs nothing but the time it ran for. */
        rp_act_squelch(false);
        en_scan_stop(&s_scan);
        rp_act_tune(s_scan.resume_khz);
        s_scan_added = 0;
        s_scan_note_for = 0;
        return;
    }
    if (!rp_model.region) return;

    s_scan_last_ms = lv_tick_get();
    s_scan_added = 0;
    s_scan_note_for = 0;

    /*
     * Quiet for the duration.
     *
     * A sweep crosses two hundred channels of hiss on its way to twenty
     * stations, and the naming pass then hops between those every few seconds -
     * neither is anything anybody wants to hear. This mutes at the tuner's own
     * MANUAL_MUTE bit, which matters here specifically: muting by closing the
     * capture PCM would stop the IIS2 clock, and RDS is the entire point of the
     * naming pass.
     *
     * Both ways out of a scan clear it - completion above, cancel just up
     * there - and it is a separate flag from the user's own mute so that
     * finishing a scan cannot unmute a radio the user silenced before starting
     * one.
     */
    rp_act_squelch(true);

    if (en_scan_start(&s_scan, rp_model.region, rp_model.khz, RP_SCAN_RSSI,
                      rp_model.can_seek))
        rp_act_tune_quiet(s_scan.khz);
    else
        rp_act_squelch(false);   /* it never started; do not leave it muted */
}

/* ---- Presets --------------------------------------------------------------- */

static void on_preset_pick(lv_event_t *e)
{
    uint32_t khz = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    rp_act_tune(khz);
    rp_ui_show(RP_SCREEN_NOW);
}

/* How long the "simple screen is full" note stays up, in refresh ticks. */
static uint8_t s_simple_full_for;

static lv_obj_t *s_scan_btn, *s_scan_label, *s_scan_fill;

static void on_preset_simple(lv_event_t *e)
{
    uint32_t khz = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    /* A refusal here means the grid is full, and it has to be said: a tick box
       that silently declines to tick is the most confusing control there is. */
    if (!rp_act_simple_toggle(khz)) s_simple_full_for = 10;

    refresh_presets();
    refresh_simple();
}

static void build_presets(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_PRESETS];
    lv_obj_t *cap = label(s, "PRESETS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    s_preset_list = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 152, C_BG);
    lv_obj_add_flag(s_preset_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_preset_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_preset_list, LV_SCROLLBAR_MODE_OFF);

    /* A line under the list for the one thing this screen has to say back. */
    s_preset_note = label(s, "", F_CAPTION, C_TA);
    lv_obj_set_size(s_preset_note, CONTENT_W, 18);
    lv_label_set_long_mode(s_preset_note, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_preset_note, MARGIN, RP_SCREEN_H - 108);

    /* Scan, across the bottom. Filling this list by hand is tuning to a
       station, deciding, saving, and doing that twenty times. */
    s_scan_btn = panel(s, MARGIN, RP_SCREEN_H - 86, CONTENT_W, TAP_MIN,
                       C_SURFACE);
    lv_obj_add_flag(s_scan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scan_btn, on_scan, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_scan_btn, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    s_scan_label = label(s_scan_btn, "Scan band", F_BODY, C_TEXT);
    lv_obj_center(s_scan_label);

    /* The progress bar is the button's own background growing across it,
       rather than a separate widget above it. There is no room on this screen
       for a bar and a button, and a button that fills up as it works says the
       same thing in the space of one. */
    s_scan_fill = panel(s_scan_btn, 0, 0, 0, TAP_MIN, C_SIGNAL);
    lv_obj_move_background(s_scan_fill);
}

static void refresh_presets(void)
{
    lv_obj_clean(s_preset_list);

    if (!rp_model.presets.count) {
        lv_obj_t *e = label(s_preset_list, "No presets yet", F_BODY,
                            C_TEXT_MUTE);
        lv_obj_set_pos(e, MARGIN, 20);
        return;
    }

    int y = 0;
    char buf[64];
    for (uint8_t i = 0; i < rp_model.presets.count; i++) {
        const en_preset_t *p = &rp_model.presets.list[i];

        lv_obj_t *r = row(s_preset_list, y, 52, on_preset_pick,
                          (void *)(uintptr_t)p->khz);

        /* Frequency on the left in the same typeface as the dial, name on the
           right - so the column of numbers scans vertically. */
        fmt_mhz(buf, sizeof buf, p->khz);
        lv_obj_t *f = label(r, buf, F_TITLE, C_TEXT);
        lv_obj_set_pos(f, MARGIN + 34, 12);

        lv_obj_t *nm = label(r, p->name[0] ? p->name : "", F_BODY, C_RDS);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(nm, CONTENT_W - 110);
        lv_obj_set_pos(nm, MARGIN + 110, 8);

        lv_obj_t *ty = label(r, en_rds_pty_name(p->pty, p->rbds),
                             F_CAPTION, C_TEXT_MUTE);
        lv_obj_set_style_text_align(ty, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(ty, CONTENT_W - 110);
        lv_obj_set_pos(ty, MARGIN + 110, 30);

        /*
         * The simple-screen mark, on its own target at the left of the row.
         *
         * A separate object rather than a gesture on the row, because the row
         * already means "tune to this" and a long press that sometimes means
         * something else is a control you have to be told about. This one is
         * visible whether or not it is set, so the affordance is there before
         * you know what it does.
         */
        lv_obj_t *mark = panel(r, 0, 4, 34, 44, C_BG);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mark, on_preset_simple, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)p->khz);
        lv_obj_t *dot = panel(mark, 11, 17, 11, 11,
                              p->simple ? C_SIGNAL : C_SURFACE_2);
        lv_obj_set_style_radius(dot, 6, 0);

        panel(r, MARGIN, 51, CONTENT_W, 1, C_HAIRLINE);
        y += 52;
    }

    /* The note line, in priority order: a refusal, then a scan in progress,
       then what a finished scan found. Only one of them at a time - it is one
       line, and the most recent thing the user did is the one worth saying. */
    if (s_simple_full_for) {
        s_simple_full_for--;
        lv_label_set_text(s_preset_note, "Simple screen is full - six at most");
    } else if (scan_running()) {
        char m[16];
        buf[0] = 0;
        fmt_mhz(m, sizeof m, s_scan.khz);
        cat(buf, m, sizeof buf);
        cat(buf, s_scan.phase != EN_SCAN_SWEEP ? "   reading names   "
                 : (s_scan.use_seek ? "   seeking   " : "   scanning   "),
            sizeof buf);
        put_uint(m, s_scan.n_hits, 0);
        cat(buf, m, sizeof buf);
        cat(buf, " found", sizeof buf);
        lv_label_set_text(s_preset_note, buf);
    } else if (s_scan_note_for) {
        s_scan_note_for--;
        buf[0] = 0;
        put_uint(buf, s_scan_added, 0);
        cat(buf, s_scan_added == 1 ? " station saved" : " stations saved",
            sizeof buf);
        if (s_scan.overflowed)
            cat(buf, " - list full", sizeof buf);
        lv_label_set_text(s_preset_note, buf);
    } else {
        lv_label_set_text(s_preset_note, "");
    }

    if (s_scan_btn) {
        lv_label_set_text(s_scan_label,
                          scan_running() ? "Stop" : "Scan band");
        int w = scan_running()
                    ? (CONTENT_W * en_scan_percent(&s_scan)) / 100
                    : 0;
        lv_obj_set_width(s_scan_fill, w);
    }
}

/* ---- the recording timer --------------------------------------------------
 *
 * The clock comes from RDS group 4A, which is the only one this device has:
 * there is no RTC. That is why a start time is offered with a caveat printed
 * next to it and a length is not - see core/timer.h.
 */

static lv_obj_t *s_rec_limit_btn, *s_rec_limit_lbl;
static lv_obj_t *s_rec_at_btn, *s_rec_at_lbl;
static lv_obj_t *s_rec_timer_note;

/* Minutes since local midnight, from the broadcast clock. The transmitted
   time is UTC with a local offset in half hours beside it, and the offset is
   signed - a station in Newfoundland sends -5 for a half-hour zone. */
static bool clock_minutes(int *out)
{
    if (!rp_model.rds.ct_valid) return false;
    int m = rp_model.rds.ct_hour * 60 + rp_model.rds.ct_minute;
    m += rp_model.rds.ct_offset * 30;
    while (m < 0) m += 24 * 60;
    m %= 24 * 60;
    *out = m;
    return true;
}

static void rectimer_pump(void)
{
    int now = 0;
    bool have = clock_minutes(&now);

    switch (en_rectimer_tick(&rp_model.rectimer, have, now,
                             rp_model.recording, rp_model.rec_ms)) {
    case EN_REC_START:
        if (!rp_model.recording) rp_act_record_toggle();
        break;
    case EN_REC_STOP:
        if (rp_model.recording) rp_act_record_toggle();
        break;
    default:
        break;
    }
}

static void on_rec_limit(lv_event_t *e)
{
    (void)e;
    uint8_t at = 0;
    for (uint8_t i = 0; i < EN_REC_LIMITS_COUNT; i++)
        if (EN_REC_LIMITS[i] == rp_model.rectimer.limit_min) { at = i; break; }
    at = (uint8_t)((at + 1) % EN_REC_LIMITS_COUNT);
    rp_act_set_rec_limit(EN_REC_LIMITS[at]);
}

/* Fifteen-minute steps, wrapping through the day and off the end back to
   "none". Arbitrary minutes would need a second control and this device has
   no keyboard; a quarter of an hour is the granularity broadcast schedules
   actually use. */
static void on_rec_at(lv_event_t *e)
{
    (void)e;
    int at = rp_model.rectimer.at_min;
    if (at == EN_REC_AT_NONE) at = 0;
    else {
        at += 15;
        if (at >= 24 * 60) at = EN_REC_AT_NONE;
    }
    rp_act_set_rec_at((int16_t)at);
}

static void on_rec_at_back(lv_event_t *e)
{
    (void)e;
    int at = rp_model.rectimer.at_min;
    if (at == EN_REC_AT_NONE) at = 24 * 60 - 15;
    else {
        at -= 15;
        if (at < 0) at = EN_REC_AT_NONE;
    }
    rp_act_set_rec_at((int16_t)at);
}

/* ---- Recordings ------------------------------------------------------------ */

static void on_library_pick(lv_event_t *e)
{
    rp_act_play_file((const char *)lv_event_get_user_data(e));
    rp_ui_show(RP_SCREEN_NOW);
}

static void build_library(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_LIBRARY];
    lv_obj_t *cap = label(s, "RECORDINGS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    s_library_list = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 176, C_BG);
    lv_obj_add_flag(s_library_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_library_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_library_list, LV_SCROLLBAR_MODE_OFF);

    hairline(s, RP_SCREEN_H - 132);

    lv_obj_t *tcap = label(s, "TIMER", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(tcap, MARGIN, RP_SCREEN_H - 126);

    /* Length on the left, start time on the right, both cycling on tap. The
       start time also steps backwards on its own narrow target, because
       reaching 07:45 by tapping forward ninety-six times is not a control. */
    s_rec_limit_btn = panel(s, MARGIN, RP_SCREEN_H - 104, CONTENT_W / 2 - 4,
                            TAP_MIN, C_SURFACE);
    lv_obj_add_flag(s_rec_limit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rec_limit_btn, on_rec_limit, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_rec_limit_btn, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    s_rec_limit_lbl = label(s_rec_limit_btn, "", F_BODY, C_TEXT);
    lv_obj_center(s_rec_limit_lbl);

    s_rec_at_btn = panel(s, MARGIN + CONTENT_W / 2 + 4, RP_SCREEN_H - 104,
                         CONTENT_W / 2 - 4 - 30, TAP_MIN, C_SURFACE);
    lv_obj_add_flag(s_rec_at_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rec_at_btn, on_rec_at, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(s_rec_at_btn, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    s_rec_at_lbl = label(s_rec_at_btn, "", F_BODY, C_TEXT);
    lv_obj_center(s_rec_at_lbl);

    lv_obj_t *back = panel(s, MARGIN + CONTENT_W - 26, RP_SCREEN_H - 104, 26,
                           TAP_MIN, C_SURFACE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_rec_at_back, LV_EVENT_CLICKED, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_SURFACE_2),
                              LV_STATE_PRESSED);
    lv_obj_center(label(back, "-", F_BODY, C_TEXT_DIM));

    /* Two lines: what will happen, and - when a start time is set - the
       caveat, because a scheduled recording on a device with no real-time
       wake is a promise this app cannot keep on its own. */
    s_rec_timer_note = label(s, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_size(s_rec_timer_note, CONTENT_W, 36);
    lv_obj_set_pos(s_rec_timer_note, MARGIN, RP_SCREEN_H - 54);
}

static void refresh_timer_controls(void)
{
    if (!s_rec_limit_btn) return;

    char buf[96], n[16];

    if (rp_model.rectimer.limit_min) {
        buf[0] = 0;
        put_uint(n, rp_model.rectimer.limit_min, 0);
        cat(buf, n, sizeof buf);
        cat(buf, " min", sizeof buf);
        lv_label_set_text(s_rec_limit_lbl, buf);
    } else {
        lv_label_set_text(s_rec_limit_lbl, "No limit");
    }

    int at = rp_model.rectimer.at_min;
    if (at == EN_REC_AT_NONE) {
        lv_label_set_text(s_rec_at_lbl, "No start");
    } else {
        buf[0] = 0;
        put_uint(n, (uint32_t)(at / 60), 0);
        if (at / 60 < 10) cat(buf, "0", sizeof buf);
        cat(buf, n, sizeof buf);
        cat(buf, ":", sizeof buf);
        put_uint(n, (uint32_t)(at % 60), 0);
        if (at % 60 < 10) cat(buf, "0", sizeof buf);
        cat(buf, n, sizeof buf);
        lv_label_set_text(s_rec_at_lbl, buf);
    }

    int now = 0;
    bool have = clock_minutes(&now);
    buf[0] = 0;
    if (at == EN_REC_AT_NONE) {
        cat(buf, rp_model.rectimer.limit_min
                     ? "Recordings stop at the length above."
                     : "Recordings run until you stop them.",
            sizeof buf);
    } else if (!have) {
        /* The honest version. The clock is the station's, not the device's. */
        cat(buf, "Waiting for the station clock. No RDS time, no start.",
            sizeof buf);
    } else {
        int until = en_rectimer_until(&rp_model.rectimer, have, now);
        put_uint(n, (uint32_t)(until < 0 ? 0 : until), 0);
        cat(buf, "Starts in ", sizeof buf);
        cat(buf, n, sizeof buf);
        cat(buf, " min. Radio+ must stay open.", sizeof buf);
    }
    lv_label_set_text(s_rec_timer_note, buf);
}

static void refresh_library(void)
{
    refresh_timer_controls();
    lv_obj_clean(s_library_list);

    if (!rp_model.library_count) {
        lv_obj_t *e = label(s_library_list, "Nothing recorded yet", F_BODY,
                            C_TEXT_MUTE);
        lv_obj_set_pos(e, MARGIN, 20);
        return;
    }

    int y = 0;
    for (uint8_t i = 0; i < rp_model.library_count; i++) {
        lv_obj_t *r = row(s_library_list, y, 46, on_library_pick,
                          rp_model.library[i]);

        /* One line, truncated. LV_LABEL_LONG_DOT needs a height as well as a
           width - given only a width it wraps instead, and the second line
           landed on whatever was below it. */
        lv_obj_t *nm = label(r, rp_model.library[i], F_BODY, C_TEXT);
        lv_obj_set_size(nm, CONTENT_W, 20);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(nm, MARGIN, 13);

        panel(r, MARGIN, 45, CONTENT_W, 1, C_HAIRLINE);
        y += 46;
    }
}

/* ---- Settings -------------------------------------------------------------- */

static void on_ta_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_ta_record(!rp_model.ta_record);
}

static void on_af_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_af_follow(!rp_model.af.enabled);
}

/* How long the AF-to-presets result stays on the row, in refresh ticks. */
static uint8_t s_af_saved_for;
static uint8_t s_af_saved_n;

/*
 * Turn the station's own alternate-frequency list into presets.
 *
 * A neat trick rather than a workaround: the station has just told us every
 * frequency it can be heard on, which is a better preset list for a journey
 * than anything built by turning a dial - and unlike a band scan it costs
 * nothing and takes no time, because the list already arrived.
 */
static void on_af_presets(lv_event_t *e)
{
    (void)e;
    s_af_saved_n = rp_act_af_to_presets();
    s_af_saved_for = 40;
}

static void on_simple_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_show_simple(!rp_model.simple_screen);
    rebuild_order();
    refresh_settings();
    refresh_dots();
}

static void on_wide_toggle(lv_event_t *e)
{
    (void)e;
    rp_act_show_wide(!rp_model.wide_screen);
    rebuild_order();
    refresh_settings();
    refresh_dots();
}

static void on_open_advanced(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_ADVANCED);
}

static void on_region_next(lv_event_t *e)
{
    (void)e;
    /* Cycle rather than open a picker: six entries is too few to justify a
       screen of its own, and cycling keeps the setting visible while it
       changes. */
    uint8_t at = 0;
    for (uint8_t i = 0; i < en_region_count; i++)
        if (&en_regions[i] == rp_model.region) { at = i; break; }
    rp_act_set_region(&en_regions[(at + 1u) % en_region_count]);
}

static lv_obj_t *setting_row(lv_obj_t *parent, int y, int h, const char *name,
                             const char *sub, lv_event_cb_t cb)
{
    lv_obj_t *r = row(parent, y, h, cb, 0);

    lv_obj_t *n = label(r, name, F_BODY, C_TEXT);
    /* Centred in a short row, but pinned to the top of a tall one - a tall
       row is tall because it carries a value below the name, and centring the
       name there puts it straight through that value. */
    lv_obj_set_pos(n, MARGIN, (sub || h > 56) ? 8 : (h - 20) / 2);

    if (sub) {
        lv_obj_t *sl = para(r, sub, F_CAPTION, C_TEXT_MUTE, CONTENT_W);
        lv_obj_set_pos(sl, MARGIN, 30);
    }

    /* Drawn from the height the row actually is. Deriving it from a default
       instead put the rule through the middle of the two tall rows. */
    panel(r, MARGIN, h - 1, CONTENT_W, 1, C_HAIRLINE);
    return r;
}

/* A value shown at the right of a row, on the name's line. */
static lv_obj_t *row_value(lv_obj_t *r, uint32_t colour)
{
    lv_obj_t *v = label(r, "", F_BODY, colour);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(v, CONTENT_W);
    lv_obj_set_pos(v, MARGIN, 8);
    return v;
}

static void build_settings(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_SETTINGS];
    lv_obj_t *cap = label(s, "SETTINGS", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(cap, MARGIN, 10);
    hairline(s, 32);

    /* Scrolls, because the content has outgrown the screen. The alternative
       was squeezing the two backend rows until the sysfs path truncated again,
       and a path that cannot be read is worth less than a scroll. */
    lv_obj_t *body = panel(s, 0, 40, RP_SCREEN_W, RP_SCREEN_H - 66, C_BG);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    s = body;

    int y = 4;

    lv_obj_t *r = setting_row(s, y, 48, "Region", 0, on_region_next);
    s_set_region = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_region, MARGIN, 14);
    y += 48;

    r = setting_row(s, y, 48, "RDS standard", 0, 0);
    s_set_std = row_value(r, C_RDS);
    lv_obj_set_pos(s_set_std, MARGIN, 14);
    y += 48;

    /* These two keep their text because it is not a description of a control -
       it is which driver and which device, which is the answer to why the
       radio is or is not working and cannot be seen any other way. */
    r = setting_row(s, y, 88, "Tuner", 0, 0);
    s_set_backend = para(r, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_set_backend, MARGIN, 30);
    y += 88;

    r = setting_row(s, y, 88, "Capture", 0, 0);
    s_set_capture = para(r, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_set_capture, MARGIN, 30);
    y += 88;

    r = setting_row(s, y, 48, "Record traffic", 0, on_ta_toggle);
    s_set_ta = row_value(r, C_TA);
    lv_obj_set_pos(s_set_ta, MARGIN, 14);
    y += 48;

    /* Following the station across transmitters. Off by default, and the
       badge on Now Playing is the other way to turn it off - which is the one
       that matters, because the moment you want it off is the moment you are
       watching it do something wrong. */
    r = setting_row(s, y, 62, "Follow station (AF)", 0, on_af_toggle);
    s_set_af = row_value(r, C_RDS);
    lv_obj_set_pos(s_set_af, MARGIN, 14);
    s_set_af_note = para(r, "Only moves to a matching station ID, and only if "
                            "it is stronger.", F_CAPTION, C_TEXT_MUTE,
                         CONTENT_W);
    lv_obj_set_pos(s_set_af_note, MARGIN, 32);
    y += 62;

    r = setting_row(s, y, 48, "Save alternates as presets", 0, on_af_presets);
    s_set_af_save = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_af_save, MARGIN, 14);
    y += 48;

    /* The two optional screens. Both add a page to the swipe sequence, which
       is why they are a setting and not always on: a sequence you have learned
       should not grow a page because someone shipped a feature. */
    r = setting_row(s, y, 48, "Simple screen", 0, on_simple_toggle);
    s_set_simple = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_simple, MARGIN, 14);
    y += 48;

    r = setting_row(s, y, 48, "Landscape readout", 0, on_wide_toggle);
    s_set_wide = row_value(r, C_SIGNAL);
    lv_obj_set_pos(s_set_wide, MARGIN, 14);
    y += 48;

    r = setting_row(s, y, 48, "Advanced", 0, on_open_advanced);
    lv_obj_t *chev = label(r, LV_SYMBOL_RIGHT, F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_style_text_align(chev, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(chev, CONTENT_W);
    lv_obj_set_pos(chev, MARGIN, 16);

    /* Last item in the scroll rather than pinned to the bottom, so it cannot
       land on top of whatever the last row happens to be. */
    /* The real build stamp, not __DATE__: that expands when THIS file is
       compiled, so a binary relinked after a change elsewhere would carry a
       date older than itself. This one comes from the makefile and is the
       same string in every app, alongside the commit it was built from. */
    lv_obj_t *stamp = label(s, en_build_version(), F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(stamp, MARGIN, y + 60);
}

static void refresh_settings(void)
{
    lv_label_set_text(s_set_region,
                      rp_model.region ? rp_model.region->name : "-");
    lv_label_set_text(s_set_std, rp_model.rds.rbds ? "RBDS" : "RDS");
    lv_label_set_text(s_set_backend,
                      rp_model.backend ? rp_model.backend : "not detected");
    lv_label_set_text(s_set_capture,
                      rp_model.capture_backend ? rp_model.capture_backend
                                               : "not started");
    lv_label_set_text(s_set_ta, rp_model.ta_record ? "On" : "Off");

    /* The state text, not just on/off: "AF TRY" on a settings row is how you
       find out the radio is mid-attempt while you are looking at settings. */
    lv_label_set_text(s_set_af, rp_model.af.enabled
                                    ? en_af_state_text(&rp_model.af) : "Off");
    lv_obj_set_style_text_color(s_set_af,
        lv_color_hex(rp_model.af.enabled ? C_RDS : C_TEXT_MUTE), 0);

    if (s_af_saved_for) {
        char b[48];
        b[0] = 0;
        put_uint(b, s_af_saved_n, 0);
        cat(b, s_af_saved_n == 1 ? " added" : " added", sizeof b);
        lv_label_set_text(s_set_af_save, b);
        s_af_saved_for--;
    } else {
        char b[48];
        b[0] = 0;
        put_uint(b, rp_model.rds.af_count, 0);
        cat(b, rp_model.rds.af_count == 1 ? " on offer" : " on offer",
            sizeof b);
        lv_label_set_text(s_set_af_save,
                          rp_model.rds.af_count ? b : "none advertised");
    }
    lv_label_set_text(s_set_simple, rp_model.simple_screen ? "On" : "Off");
    lv_obj_set_style_text_color(s_set_simple,
        lv_color_hex(rp_model.simple_screen ? C_SIGNAL : C_TEXT_MUTE), 0);
    lv_label_set_text(s_set_wide, rp_model.wide_screen ? "On" : "Off");
    lv_obj_set_style_text_color(s_set_wide,
        lv_color_hex(rp_model.wide_screen ? C_SIGNAL : C_TEXT_MUTE), 0);
    lv_obj_set_style_text_color(s_set_ta,
                                lv_color_hex(rp_model.ta_record ? C_TA
                                                                : C_TEXT_MUTE),
                                0);
}

/* ---- Advanced: the register explorer --------------------------------------- */

static void on_register_pick(lv_event_t *e)
{
    rp_ui_open_register((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void on_advanced_back(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_SETTINGS);
}

static void build_advanced(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_ADVANCED];

    lv_obj_t *back = panel(s, 0, 0, RP_SCREEN_W, 40, C_BG);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_advanced_back, LV_EVENT_CLICKED, 0);
    lv_obj_t *b = label(back, LV_SYMBOL_LEFT "  REGISTERS", F_CAPTION,
                        C_TEXT_MUTE);
    lv_obj_set_pos(b, MARGIN, 12);
    hairline(s, 40);

    /* No separate notice line: when raw access is unavailable the reason goes
       into the list itself, so an empty notice never leaves a gap above it. */
    s_adv_list = panel(s, 0, 50, RP_SCREEN_W, RP_SCREEN_H - 70, C_BG);
    lv_obj_add_flag(s_adv_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_adv_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_adv_list, LV_SCROLLBAR_MODE_OFF);
}

static void refresh_advanced(void)
{
    lv_obj_clean(s_adv_list);

    /* The whole screen is generated from the register table, which is why
       supporting every register is a table entry rather than a screen each. */
    if (!rp_model.can_raw) {
        lv_obj_t *no = label(s_adv_list, "Not available here", F_BODY, C_TA);
        lv_obj_set_pos(no, MARGIN, 8);
        lv_obj_t *why = para(s_adv_list,
            "Only where a driver owns the transport.",
            F_CAPTION, C_TEXT_MUTE, CONTENT_W);
        lv_obj_set_pos(why, MARGIN, 32);
        return;
    }

    int y = 0;
    char buf[80];
    for (uint8_t i = 0; i < en_fm_reg_count; i++) {
        const en_fm_reg_t *g = &en_fm_regs[i];
        lv_obj_t *r = row(s_adv_list, y, 54, on_register_pick,
                          (void *)(uintptr_t)g->addr);

        /* Address in hex, because that is how the datasheet and every other
           tool refer to it, and matching them is the whole point of this
           screen existing. */
        static const char hex[] = "0123456789ABCDEF";
        buf[0] = '0'; buf[1] = 'x';
        buf[2] = hex[(g->addr >> 4) & 0xF];
        buf[3] = hex[g->addr & 0xF];
        buf[4] = 0;
        lv_obj_t *a = label(r, buf, F_BODY, C_SIGNAL);
        lv_obj_set_pos(a, MARGIN, 8);

        lv_obj_t *nm = label(r, g->name, F_CAPTION, C_TEXT);
        lv_obj_set_width(nm, CONTENT_W - 52);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(nm, MARGIN + 52, 10);

        /* Access, and the honest caveats: a length nobody verified, and a
           register the documentation says must be read before it is written. */
        buf[0] = 0;
        cat(buf, (g->flags & EN_FM_R) ? "R" : "-", sizeof buf);
        cat(buf, (g->flags & EN_FM_W) ? "W" : "-", sizeof buf);
        if (g->flags & EN_FM_RMW) cat(buf, "  read-modify-write", sizeof buf);
        if (g->flags & EN_FM_TBD) cat(buf, "  length unverified", sizeof buf);

        lv_obj_t *fl = label(r, buf, F_CAPTION,
                             (g->flags & EN_FM_TBD) ? C_TA : C_TEXT_MUTE);
        lv_obj_set_pos(fl, MARGIN + 52, 30);

        panel(r, MARGIN, 53, CONTENT_W, 1, C_HAIRLINE);
        y += 54;
    }
}

/* ---- one register, field by field ------------------------------------------ */

/*
 * Every control here is generated from the field descriptors in fmreg.c. A
 * bitmap becomes a list of toggles named by its bits, an enum becomes a list of
 * options, and everything else becomes a slider bounded by the range the table
 * declares. Nothing about any particular register is written down twice.
 */

static void reg_push(void)
{
    if (!s_reg) return;
    rp_act_reg_write(s_reg->addr, s_reg_val, s_reg->write_len);
}

/* user_data packs which field and which bit, since a toggle needs both. */
#define PACK(f, b)  ((void *)(uintptr_t)(((uint32_t)(f) << 8) | (uint8_t)(b)))
#define UNPACK_F(p) ((uint8_t)(((uintptr_t)(p) >> 8) & 0xFF))
#define UNPACK_B(p) ((uint8_t)((uintptr_t)(p) & 0xFF))

static void on_bit_toggle(lv_event_t *e)
{
    void *u = lv_event_get_user_data(e);
    if (!s_reg) return;

    const en_fm_field_t *f = &s_reg->fields[UNPACK_F(u)];
    uint32_t mask = f->bits[UNPACK_B(u)].mask;

    int32_t v = en_fm_field_get(f, s_reg_val, s_reg->write_len);
    v = (int32_t)(((uint32_t)v) ^ mask);

    if (en_fm_field_set(f, s_reg_val, s_reg->write_len, v)) reg_push();
    rp_ui_open_register(s_reg->addr);      /* redraw with the new state */
}

static void on_enum_pick(lv_event_t *e)
{
    void *u = lv_event_get_user_data(e);
    if (!s_reg) return;

    const en_fm_field_t *f = &s_reg->fields[UNPACK_F(u)];
    int32_t v = (int32_t)f->vals[UNPACK_B(u)].value;

    if (en_fm_field_set(f, s_reg_val, s_reg->write_len, v)) reg_push();
    rp_ui_open_register(s_reg->addr);
}

static void on_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    void *u = lv_event_get_user_data(e);
    if (!s_reg) return;

    const en_fm_field_t *f = &s_reg->fields[UNPACK_F(u)];
    if (en_fm_field_set(f, s_reg_val, s_reg->write_len,
                        (int32_t)lv_slider_get_value(sl)))
        reg_push();

    /* Only the readout is updated here. Rebuilding the screen mid-drag would
       destroy the slider under the finger. */
    lv_obj_t *val = lv_obj_get_child(lv_obj_get_parent(sl), 1);
    if (val) {
        char b[16];
        put_uint(b, (uint32_t)lv_slider_get_value(sl), 0);
        lv_label_set_text(val, b);
    }
}

static void on_reg_back(lv_event_t *e)
{
    (void)e;
    rp_ui_show(RP_SCREEN_ADVANCED);
}

static void on_reg_revert(lv_event_t *e)
{
    (void)e;
    if (!s_reg) return;
    rp_act_reg_revert(s_reg->addr);
    rp_ui_open_register(s_reg->addr);
}

/* A row whose right-hand chip is filled when the thing is on. Used for both
   bitmap bits and enum options, because "this one is selected" reads the same
   either way. */
static int reg_choice(lv_obj_t *parent, int y, const char *name, bool on,
                      lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = row(parent, y, 44, cb, user);

    lv_obj_t *l = label(r, name, F_CAPTION, on ? C_TEXT : C_TEXT_DIM);
    lv_obj_set_width(l, CONTENT_W - 40);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, MARGIN + 8, 14);

    lv_obj_t *chip = panel(r, RP_SCREEN_W - MARGIN - 24, 15, 14, 14,
                           on ? C_SIGNAL : C_SURFACE_2);
    lv_obj_set_style_radius(chip, 3, 0);

    panel(r, MARGIN + 8, 43, CONTENT_W - 8, 1, C_HAIRLINE);
    return y + 44;
}

static int reg_slider(lv_obj_t *parent, int y, const en_fm_field_t *f,
                      uint8_t fi)
{
    lv_obj_t *r = row(parent, y, 66, 0, 0);

    lv_obj_t *n = label(r, f->name, F_CAPTION, C_TEXT);
    lv_obj_set_pos(n, MARGIN + 8, 8);

    /* Child index 1: on_slider finds the readout this way rather than caching
       a pointer per field. */
    int32_t cur = en_fm_field_get(f, s_reg_val, s_reg->write_len);
    char b[16];
    put_uint(b, (uint32_t)(cur < 0 ? -cur : cur), 0);
    lv_obj_t *val = label(r, b, F_CAPTION, C_SIGNAL);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(val, CONTENT_W - 16);
    lv_obj_set_pos(val, MARGIN + 8, 8);

    lv_obj_t *sl = lv_slider_create(r);
    lv_obj_set_size(sl, CONTENT_W - 16, 6);
    lv_obj_set_pos(sl, MARGIN + 8, 36);
    lv_slider_set_range(sl, f->min, f->max);
    lv_slider_set_value(sl, cur, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(sl, lv_color_hex(C_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_SIGNAL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 2, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 18);
    lv_obj_add_event_cb(sl, on_slider, LV_EVENT_VALUE_CHANGED, PACK(fi, 0));

    panel(r, MARGIN + 8, 65, CONTENT_W - 8, 1, C_HAIRLINE);
    return y + 66;
}

static void build_register(void)
{
    lv_obj_t *s = s_screen[RP_SCREEN_REGISTER];

    lv_obj_t *back = panel(s, 0, 0, RP_SCREEN_W - 76, 40, C_BG);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_reg_back, LV_EVENT_CLICKED, 0);
    lv_obj_t *b = label(back, LV_SYMBOL_LEFT "  REGISTERS", F_CAPTION,
                        C_TEXT_MUTE);
    lv_obj_set_pos(b, MARGIN, 12);

    /* Only shown when this register has actually been overridden - an always
       present Revert invites undoing something that was never done. */
    s_reg_revert = panel(s, RP_SCREEN_W - 76, 0, 76, 40, C_BG);
    lv_obj_add_flag(s_reg_revert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_reg_revert, on_reg_revert, LV_EVENT_CLICKED, 0);
    lv_obj_t *rv = label(s_reg_revert, "REVERT", F_CAPTION, C_TA);
    lv_obj_set_pos(rv, 12, 12);

    /* The address in large type on the left - it is short, and it is what you
       match against a datasheet - with the raw value opposite it. */
    s_reg_title = label(s, "", F_TITLE, C_SIGNAL);
    lv_obj_set_pos(s_reg_title, MARGIN, 46);

    s_reg_hex = label(s, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_reg_hex, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_reg_hex, CONTENT_W);
    lv_obj_set_pos(s_reg_hex, MARGIN, 52);

    /* The name underneath, at a size that fits. Given a height as well as a
       width, so LV_LABEL_LONG_DOT truncates rather than wrapping. */
    s_reg_name = label(s, "", F_BODY, C_TEXT);
    lv_obj_set_size(s_reg_name, CONTENT_W, 20);
    lv_label_set_long_mode(s_reg_name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_reg_name, MARGIN, 76);

    s_reg_doc = para(s, "", F_CAPTION, C_TEXT_MUTE, CONTENT_W);
    lv_obj_set_pos(s_reg_doc, MARGIN, 102);

    hairline(s, 146);
    s_reg_body = panel(s, 0, 152, RP_SCREEN_W, RP_SCREEN_H - 162, C_BG);
    lv_obj_add_flag(s_reg_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_reg_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_reg_body, LV_SCROLLBAR_MODE_OFF);
}

void rp_ui_open_register(uint8_t addr)
{
    s_reg = en_fm_reg_find(addr);
    if (!s_reg) return;

    /* Start from what the chip currently holds, so editing one field of a
       packed register leaves its neighbours where they were. Registers that
       cannot be read start from an override if there is one, and from zero if
       not - which is stated on screen rather than left to be discovered. */
    for (uint8_t i = 0; i < 8; i++) s_reg_val[i] = 0;
    bool live = (s_reg->flags & EN_FM_R)
             && rp_act_reg_read(addr, s_reg_val,
                                s_reg->write_len ? s_reg->write_len
                                                 : s_reg->read_len);

    char buf[96];
    static const char hex[] = "0123456789ABCDEF";

    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(addr >> 4) & 0xF];
    buf[3] = hex[addr & 0xF];
    buf[4] = 0;
    lv_label_set_text(s_reg_title, buf);
    lv_label_set_text(s_reg_name, s_reg->name);

    /* The raw value, because anyone on this screen is cross-referencing a
       datasheet and wants the number the fields add up to. */
    buf[0] = 0;
    if (s_reg->write_len) {
        cat(buf, "= 0x", sizeof buf);
        char h[3] = { 0, 0, 0 };
        for (uint8_t i = 0; i < s_reg->write_len; i++) {
            h[0] = hex[(s_reg_val[i] >> 4) & 0xF];
            h[1] = hex[s_reg_val[i] & 0xF];
            cat(buf, h, sizeof buf);
        }
    }
    if (!live && (s_reg->flags & EN_FM_R)) cat(buf, "   (not read)", sizeof buf);
    lv_label_set_text(s_reg_hex, buf);
    lv_label_set_text(s_reg_doc, s_reg->doc ? s_reg->doc : "");

    if (rp_act_reg_overridden(addr))
        lv_obj_remove_flag(s_reg_revert, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_reg_revert, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_reg_body);
    int y = 0;

    if (!s_reg->nfields) {
        lv_obj_t *n = label(s_reg_body, "No fields described", F_CAPTION,
                            C_TEXT_MUTE);
        lv_obj_set_pos(n, MARGIN, 8);
    }

    for (uint8_t i = 0; i < s_reg->nfields; i++) {
        const en_fm_field_t *f = &s_reg->fields[i];

        /* A register that cannot be written gets its values shown and no
           controls - offering a slider that does nothing would be a lie. */
        bool editable = (s_reg->flags & EN_FM_W) != 0;

        if ((f->flags & EN_FMF_BITMAP) && f->bits) {
            lv_obj_t *h = label(s_reg_body, f->name, F_CAPTION, C_TEXT_MUTE);
            lv_obj_set_pos(h, MARGIN, y + 8);
            y += 30;

            int32_t v = en_fm_field_get(f, s_reg_val, s_reg->write_len
                                                    ? s_reg->write_len
                                                    : s_reg->read_len);
            for (uint8_t b = 0; b < f->nbits; b++)
                y = reg_choice(s_reg_body, y, f->bits[b].name,
                               (((uint32_t)v) & f->bits[b].mask) != 0,
                               editable ? on_bit_toggle : 0, PACK(i, b));

        } else if ((f->flags & EN_FMF_ENUM) && f->vals) {
            lv_obj_t *h = label(s_reg_body, f->name, F_CAPTION, C_TEXT_MUTE);
            lv_obj_set_pos(h, MARGIN, y + 8);
            y += 30;

            int32_t v = en_fm_field_get(f, s_reg_val, s_reg->write_len
                                                    ? s_reg->write_len
                                                    : s_reg->read_len);
            for (uint8_t k = 0; k < f->nvals; k++)
                y = reg_choice(s_reg_body, y, f->vals[k].name,
                               (uint32_t)v == f->vals[k].value,
                               editable ? on_enum_pick : 0, PACK(i, k));

        } else if (editable) {
            y = reg_slider(s_reg_body, y, f, i);

        } else {
            /* Read-only: the name and its value, nothing to touch. */
            lv_obj_t *r = row(s_reg_body, y, 44, 0, 0);
            lv_obj_t *n = label(r, f->name, F_CAPTION, C_TEXT_DIM);
            lv_obj_set_pos(n, MARGIN, 14);

            int32_t v = en_fm_field_get(f, s_reg_val, s_reg->read_len);
            char vb[16];
            if (v < 0) { vb[0] = '-'; put_uint(vb + 1, (uint32_t)(-v), 0); }
            else put_uint(vb, (uint32_t)v, 0);

            lv_obj_t *vl = label(r, vb, F_CAPTION, C_SIGNAL);
            lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_width(vl, CONTENT_W);
            lv_obj_set_pos(vl, MARGIN, 14);
            panel(r, MARGIN, 43, CONTENT_W, 1, C_HAIRLINE);
            y += 44;
        }
    }

    lv_screen_load(s_screen[RP_SCREEN_REGISTER]);
    s_current = RP_SCREEN_REGISTER;
}

/* ---- lifecycle ------------------------------------------------------------- */

static void build_screen_shell(rp_screen_t which)
{
    lv_obj_t *s = lv_obj_create(NULL);
    flat(s, C_BG);
    lv_obj_set_size(s, RP_SCREEN_W, RP_SCREEN_H);
    lv_obj_add_event_cb(s, on_gesture, LV_EVENT_GESTURE, 0);
    s_screen[which] = s;
}

void rp_ui_build_all(void)
{
    for (int i = 0; i < RP_SCREEN_COUNT; i++) build_screen_shell((rp_screen_t)i);

    build_simple();
    build_now();
    build_wide();
    build_dial();
    build_presets();
    build_library();
    build_settings();
    build_advanced();
    build_register();

    rebuild_order();

    /* Dots go on every screen that can be in the swipe order, including the
       optional ones - which are built whether or not they are currently in
       it, so that turning one on is a settings change and not a construction
       job in the middle of a gesture. */
    for (int i = 0; i < RP_SWIPE_MAX; i++)
        build_dots(i, s_screen[i]);

    refresh_simple();
    refresh_now();
    refresh_wide();
    refresh_dial();
    refresh_presets();
    refresh_library();
    refresh_settings();
    refresh_advanced();
}

/* ---- the boot screen ------------------------------------------------------ */

/*
 * Deliberately built by hand rather than through build_screen_shell and the
 * rest. It has to exist before anything else does - that is its entire job -
 * and anything it shared with the real screens would be one more thing that
 * has to work before the first pixel appears.
 */
static lv_obj_t *s_boot, *s_boot_step, *s_boot_fail, *s_boot_dots;
static uint8_t   s_boot_n;

void rp_ui_boot(const char *step)
{
    if (!s_boot) {
        s_boot = lv_obj_create(NULL);
        flat(s_boot, C_BG);
        lv_obj_remove_flag(s_boot, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = label(s_boot, "Radio+", F_STATION, C_TEXT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(name, CONTENT_W);
        lv_obj_set_pos(name, MARGIN, 150);

        /* A rule under the name, so the screen reads as a screen rather than
           as two labels that happen to be there. */
        panel(s_boot, MARGIN + 60, 190, CONTENT_W - 120, 1, C_HAIRLINE);

        s_boot_step = label(s_boot, "", F_CAPTION, C_TEXT_DIM);
        lv_obj_set_style_text_align(s_boot_step, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(s_boot_step, CONTENT_W, 20);
        lv_label_set_long_mode(s_boot_step, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_boot_step, MARGIN, 206);

        /* Whatever went wrong first, kept under the running step. */
        s_boot_fail = label(s_boot, "", F_CAPTION, C_TA);
        lv_obj_set_style_text_align(s_boot_fail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(s_boot_fail, CONTENT_W, 36);
        lv_label_set_long_mode(s_boot_fail, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s_boot_fail, MARGIN, 232);

        /* Progress without a percentage. There is no way to know how long the
           tuner will take, and a bar that lies about it is worse than a row of
           dots that only claims something is still happening. */
        s_boot_dots = label(s_boot, "", F_CAPTION, C_SIGNAL);
        lv_obj_set_style_text_align(s_boot_dots, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_boot_dots, CONTENT_W);
        lv_obj_set_pos(s_boot_dots, MARGIN, 280);

        lv_screen_load(s_boot);
    }

    if (step) lv_label_set_text(s_boot_step, step);

    if (s_boot_n < 12) s_boot_n++;
    char dots[16];
    uint8_t i = 0;
    for (; i < s_boot_n && i < 12; i++) dots[i] = '.';
    dots[i] = 0;
    lv_label_set_text(s_boot_dots, dots);

    /* Synchronous. The caller is about to block for seconds inside a driver,
       and a frame queued behind that is a frame nobody sees. */
    lv_refr_now(NULL);
}

void rp_ui_boot_failed(const char *what)
{
    if (!s_boot_fail || !what) return;
    /* Only the first one. Later failures are usually consequences of it, and
       the screen has room for the cause rather than the cascade. */
    if (lv_label_get_text(s_boot_fail)[0]) return;
    lv_label_set_text(s_boot_fail, what);
    lv_refr_now(NULL);
}

void rp_ui_init(void)
{
    /* Re-theme first. The default theme's blue would look like a different
       application next to this palette, and re-skinning every widget by hand
       afterwards is worse than setting it once. */
    lv_display_t *d = lv_display_get_default();
    if (d)
        lv_theme_default_init(d, lv_color_hex(C_SIGNAL),
                              lv_color_hex(C_RDS), true, F_BODY);

    rp_ui_build_all();
    rp_ui_show(RP_SCREEN_NOW);
}

void rp_ui_show(rp_screen_t which)
{
    if (which >= RP_SCREEN_COUNT || !s_screen[which]) return;
    s_current = which;

    /* Restyle this screen's own row of dots. */
    if (which < RP_SWIPE_MAX) refresh_dots();

    lv_screen_load(s_screen[which]);
}

rp_screen_t rp_ui_current(void) { return s_current; }

void rp_ui_scan_start_for_preview(void) { on_scan(NULL); }

void rp_ui_tick(void)
{
    /* Before the screens, and not inside their cases: a scan and a timed
       recording both have to keep running when the user swipes away from the
       screen that set them going. */
    scan_pump();
    rectimer_pump();
    af_pump();

    switch (s_current) {
    case RP_SCREEN_SIMPLE:   refresh_simple();   break;
    case RP_SCREEN_NOW:      refresh_now();      break;
    case RP_SCREEN_WIDE:     refresh_wide();     break;
    case RP_SCREEN_DIAL:     refresh_dial();     break;
    case RP_SCREEN_PRESETS:  refresh_presets();  break;
    case RP_SCREEN_LIBRARY:  refresh_library();  break;
    case RP_SCREEN_SETTINGS: refresh_settings(); break;
    case RP_SCREEN_ADVANCED: break;   /* static once built */
    default: break;
    }
}

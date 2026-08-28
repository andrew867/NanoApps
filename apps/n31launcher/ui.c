/*
 * ui.c — the launcher screens. See ui.h for the navigation model.
 *
 * 240 x 432 and no touchscreen, so everything here is shaped by having three
 * buttons down one side and nothing to point with.
 *
 * The home screen does not scroll. Three tiles, one per button, always in the
 * same place: the two apps that are always installed, and a way into everything
 * else. A fixed screen can be used without looking at it, which is the whole
 * point of a launcher you reach for by feel. The list lives one screen deeper,
 * where scrolling costs nothing because you are already looking.
 *
 * In the list, the selected row is the only bright thing on the screen. With no
 * pointer, the selection IS the cursor, so it is drawn like one - filled
 * background, full accent, white name - and everything else sits back at half
 * strength.
 *
 * Same palette as Radio+ deliberately. Two apps by the same hand on the same
 * device that look like strangers to each other is a worse result than either
 * of them looking slightly plain.
 */

#include "ui.h"
#include "apps.h"

#include "lvgl/lvgl.h"

#include <stdio.h>

#define C_BG        0x08090D
#define C_SURFACE   0x101219
#define C_SURFACE_2 0x171B25
#define C_HAIRLINE  0x232937
#define C_TEXT      0xEDEFF4
#define C_TEXT_DIM  0x8B92A0
#define C_TEXT_MUTE 0x545B69

#define C_RADIO  0x22D3EE
#define C_EXTRAS 0xA78BFA
#define C_MUSIC  0x34D399
#define C_WARN   0xFB923C

#define F_BIG     (&lv_font_montserrat_24)
#define F_NAME    (&lv_font_montserrat_20)
#define F_BODY    (&lv_font_montserrat_16)
#define F_CAPTION (&lv_font_montserrat_14)

#define MARGIN    12
#define CONTENT_W (N31_SCREEN_W - 2 * MARGIN)

#define HEADER_H  38

/* home */
#define TILE_TOP  52
#define TILE_H    106

/* extras */
#define LIST_TOP  44
#define ROW_H     66
#define ROWS      5                       /* 5 * 66 = 330, ends at y = 374 */
#define BAR_X     (N31_SCREEN_W - 5)
#define BAR_W     3

static uint32_t dim(uint32_t c) { return (c >> 1) & 0x7F7F7Fu; }

/* ---- building blocks ------------------------------------------------------ */

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

/* A label that must not wrap. LV_LABEL_LONG_DOT needs a height as well as a
   width - given only a width it wraps to a second line, and that line lands on
   whatever is below it. */
static lv_obj_t *fitted(lv_obj_t *parent, const char *text,
                        const lv_font_t *font, uint32_t colour,
                        int x, int y, int w, int h)
{
    lv_obj_t *l = label(parent, text, font, colour);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *centred(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t colour,
                         int y, int h)
{
    lv_obj_t *l = label(parent, text, font, colour);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(l, CONTENT_W, h);
    lv_obj_set_pos(l, MARGIN, y);
    return l;
}

static lv_obj_t *header(lv_obj_t *screen, lv_obj_t **status_out)
{
    lv_obj_t *t = label(screen, "N31", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_pos(t, MARGIN, 14);

    if (status_out) {
        lv_obj_t *s = label(screen, "", F_CAPTION, C_TEXT_MUTE);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(s, CONTENT_W, 18);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, MARGIN, 14);
        *status_out = s;
    }

    panel(screen, MARGIN, HEADER_H, CONTENT_W, 1, C_HAIRLINE);
    return t;
}

static lv_obj_t *footer(lv_obj_t *screen, const char *text)
{
    return centred(screen, text, F_CAPTION, C_TEXT_MUTE, N31_SCREEN_H - 26, 18);
}

/* ---- home ----------------------------------------------------------------- */

typedef struct {
    lv_obj_t *root;
    lv_obj_t *tagline;
} tile_t;

static lv_obj_t *s_home;
static lv_obj_t *s_home_status;
static tile_t    s_tile[3];

/*
 * One home tile.
 *
 * The accent appears three times and always means the same thing: a bar down
 * the left edge, the icon block, and the key chip. On a screen with no pointer
 * the colour is what ties "this tile" to "that button", so it is used for
 * identity and never for decoration.
 *
 * The key chip sits under the icon rather than beside the name. Beside it left
 * 66 px for a 20 pt name, and every one of them overflowed into the chip.
 */
static void build_tile(tile_t *t, lv_obj_t *screen, int y, const char *name,
                       const char *tagline, const char *glyph,
                       const char *key, uint32_t accent)
{
    t->root = panel(screen, 0, y, N31_SCREEN_W, TILE_H, C_BG);

    panel(t->root, 0, 0, 4, TILE_H, accent);

    lv_obj_t *icon = panel(t->root, MARGIN + 4, 14, 58, 58, C_SURFACE);
    lv_obj_set_style_radius(icon, 6, 0);
    lv_obj_center(label(icon, glyph, F_BIG, accent));

    lv_obj_t *chip = panel(t->root, MARGIN + 4, 76, 58, 20, C_SURFACE_2);
    lv_obj_set_style_radius(chip, 3, 0);
    lv_obj_center(label(chip, key, F_CAPTION, accent));

    const int rx = MARGIN + 74;
    const int rw = N31_SCREEN_W - rx - MARGIN;

    fitted(t->root, name, F_NAME, C_TEXT, rx, 30, rw, 26);
    t->tagline = fitted(t->root, tagline, F_CAPTION, C_TEXT_DIM, rx, 58, rw, 18);

    panel(t->root, MARGIN, TILE_H - 1, CONTENT_W, 1, C_HAIRLINE);
}

static void build_home(void)
{
    s_home = lv_obj_create(NULL);
    flat(s_home, C_BG);
    lv_obj_set_size(s_home, N31_SCREEN_W, N31_SCREEN_H);

    header(s_home, &s_home_status);

    /* Drawn in button order - volume up, play, volume down - so the tile you
       want is where your thumb already is. */
    build_tile(&s_tile[N31_TILE_RADIO],  s_home, TILE_TOP,
               "Radio+", "FM, RDS, recording", "FM", "VOL +", C_RADIO);
    build_tile(&s_tile[N31_TILE_EXTRAS], s_home, TILE_TOP + TILE_H,
               "Extra apps", "", "•••", "PLAY", C_EXTRAS);
    build_tile(&s_tile[N31_TILE_MUSIC],  s_home, TILE_TOP + 2 * TILE_H,
               "TinyPod", "Music", "TP", "VOL -", C_MUSIC);
}

void n31_ui_home(void)
{
    /* The extras tile says how many there are, because the answer is usually
       none and finding that out by pressing is a wasted trip. */
    char t[32];
    if (n31_app_count > N31_BUILTIN_COUNT) {
        int n = (int)n31_app_count - N31_BUILTIN_COUNT;
        snprintf(t, sizeof t, "%d on the volume", n);
    } else {
        snprintf(t, sizeof t, "None yet");
    }
    lv_label_set_text(s_tile[N31_TILE_EXTRAS].tagline, t);

    lv_screen_load(s_home);
}

void n31_ui_home_hot(int tile, bool on)
{
    if (tile < 0 || tile > 2) return;

    /* A press has to be visible before the screen is handed to the app, or a
       button that takes a moment to start something looks like a button that
       did nothing. */
    lv_obj_set_style_bg_color(s_tile[tile].root,
                              lv_color_hex(on ? C_SURFACE : C_BG), 0);
}

/* ---- extras --------------------------------------------------------------- */

typedef struct {
    lv_obj_t *root;
    lv_obj_t *edge;
    lv_obj_t *icon;
    lv_obj_t *glyph;
    lv_obj_t *name;
    lv_obj_t *tagline;
} row_t;

static lv_obj_t *s_extras;
static lv_obj_t *s_extras_status;
static lv_obj_t *s_extras_foot;
static lv_obj_t *s_bar_track;
static lv_obj_t *s_bar_thumb;
static lv_obj_t *s_none;          /* the modal card */
static lv_obj_t *s_none_head;
static lv_obj_t *s_none_body;
static lv_obj_t *s_none_hint;
static row_t     s_row[ROWS];

static int  s_selected;
static int  s_first;              /* first app shown in row 0 */
static bool s_opening;

static void build_row(row_t *r, lv_obj_t *screen, int y)
{
    r->root = panel(screen, 0, y, N31_SCREEN_W, ROW_H, C_BG);
    r->edge = panel(r->root, 0, 0, 4, ROW_H, C_BG);

    r->icon = panel(r->root, MARGIN + 4, 11, 44, 44, C_SURFACE);
    lv_obj_set_style_radius(r->icon, 6, 0);
    r->glyph = label(r->icon, "", F_NAME, C_TEXT);
    lv_obj_center(r->glyph);

    const int rx = MARGIN + 58;
    const int rw = BAR_X - rx - 6;

    r->name    = fitted(r->root, "", F_NAME,    C_TEXT,     rx, 12, rw, 24);
    r->tagline = fitted(r->root, "", F_CAPTION, C_TEXT_DIM, rx, 38, rw, 18);

    panel(r->root, MARGIN, ROW_H - 1, CONTENT_W, 1, C_HAIRLINE);
}

static void fill_row(row_t *r, int app_index)
{
    if (app_index < (int)n31_extra_first || app_index >= (int)n31_app_count) {
        lv_obj_add_flag(r->root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(r->root, LV_OBJ_FLAG_HIDDEN);

    const n31_app_t *a = &n31_apps[app_index];
    const bool sel = (app_index == s_selected);

    /* Opening is a brighter still version of selected, not a different colour:
       the row that lights up has to be unmistakably the row that was chosen. */
    lv_obj_set_style_bg_color(r->root,
        lv_color_hex(sel ? (s_opening ? C_SURFACE_2 : C_SURFACE) : C_BG), 0);
    lv_obj_set_style_bg_color(r->edge,
        lv_color_hex(sel ? a->accent : dim(a->accent)), 0);
    lv_obj_set_style_bg_color(r->icon,
        lv_color_hex(sel ? C_SURFACE_2 : C_SURFACE), 0);

    lv_label_set_text(r->glyph, a->glyph);
    lv_obj_set_style_text_color(r->glyph,
        lv_color_hex(sel ? a->accent : dim(a->accent)), 0);
    lv_obj_center(r->glyph);

    lv_label_set_text(r->name, a->name);
    lv_obj_set_style_text_color(r->name,
        lv_color_hex(sel ? C_TEXT : C_TEXT_DIM), 0);

    lv_label_set_text(r->tagline, a->tagline);
    lv_obj_set_style_text_color(r->tagline,
        lv_color_hex(sel ? C_TEXT_DIM : C_TEXT_MUTE), 0);

    /* An app with nothing to say about itself gets its name centred rather
       than sitting high with a gap under it. */
    if (a->tagline[0]) {
        lv_obj_set_pos(r->name, MARGIN + 58, 12);
        lv_obj_remove_flag(r->tagline, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(r->name, MARGIN + 58, 21);
        lv_obj_add_flag(r->tagline, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* The indicator only appears when there is something off screen to indicate.
   Measured against the discovered apps, not every app: the builtins are on the
   home screen and are not in this list. */
static void update_scrollbar(void)
{
    const int n = (int)n31_extra_count;
    const bool need = n > ROWS;
    show(s_bar_track, need);
    show(s_bar_thumb, need);
    if (!need) return;

    const int track = ROW_H * ROWS;
    int h = track * ROWS / n;
    if (h < 18) h = 18;

    const int offset = s_first - (int)n31_extra_first;
    const int span = n - ROWS;                       /* > 0 here */
    lv_obj_set_pos(s_bar_thumb, BAR_X,
                   LIST_TOP + (track - h) * offset / span);
    lv_obj_set_size(s_bar_thumb, BAR_W, h);
}

static void build_extras(void)
{
    s_extras = lv_obj_create(NULL);
    flat(s_extras, C_BG);
    lv_obj_set_size(s_extras, N31_SCREEN_W, N31_SCREEN_H);

    header(s_extras, &s_extras_status);

    for (int i = 0; i < ROWS; i++)
        build_row(&s_row[i], s_extras, LIST_TOP + i * ROW_H);

    /* After the rows, not before: rows are full width and LVGL draws in
       creation order, so a bar made first is a bar painted over. */
    s_bar_track = panel(s_extras, BAR_X, LIST_TOP, BAR_W, ROW_H * ROWS,
                        C_SURFACE);
    s_bar_thumb = panel(s_extras, BAR_X, LIST_TOP, BAR_W, 18, C_TEXT_MUTE);

    /* A card, not a message centred on an empty screen. It has appeared over
       the list and it wants a keypress, and an edge and a raised surface are
       what say so. */
    s_none = panel(s_extras, MARGIN, 100, CONTENT_W, 204, C_SURFACE);
    lv_obj_set_style_radius(s_none, 8, 0);
    lv_obj_set_style_border_width(s_none, 1, 0);
    lv_obj_set_style_border_color(s_none, lv_color_hex(C_HAIRLINE), 0);
    lv_obj_set_style_border_opa(s_none, LV_OPA_COVER, 0);

    s_none_head = label(s_none, "", F_NAME, C_TEXT);
    lv_obj_set_style_text_align(s_none_head, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_none_head, CONTENT_W - 24, 28);
    lv_obj_set_pos(s_none_head, 12, 20);

    s_none_body = label(s_none, "", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_style_text_align(s_none_body, LV_TEXT_ALIGN_CENTER, 0);
    /* Three lines at 14 pt. The longest of these messages needs all three, and
       a label given fewer simply clips - it does not shrink the text. */
    lv_obj_set_size(s_none_body, CONTENT_W - 24, 56);
    lv_obj_set_pos(s_none_body, 12, 56);

    s_none_hint = label(s_none, "", F_CAPTION, C_EXTRAS);
    lv_obj_set_style_text_align(s_none_hint, LV_TEXT_ALIGN_CENTER, 0);
    /* Left to wrap, and given the three lines that produces. Hand-breaking
       this text guessed the width wrong twice: the explicit newline stayed put
       while the words around it wrapped anyway, so the last line fell off the
       bottom of the card both times. */
    lv_obj_set_size(s_none_hint, CONTENT_W - 24, 64);
    lv_obj_set_pos(s_none_hint, 12, 120);

    s_extras_foot = footer(s_extras, "");
}

void n31_ui_extras(int selected, bool disk_mounted)
{
    if (n31_extra_count == 0) {
        for (int i = 0; i < ROWS; i++) show(s_row[i].root, false);
        show(s_bar_track, false);
        show(s_bar_thumb, false);
        show(s_none, true);

        if (!disk_mounted) {
            lv_label_set_text(s_none_head, "No apps");
            lv_label_set_text(s_none_body,
                              "The internal volume has\nnot been brought up.");
            lv_label_set_text(s_none_hint,
                              "Press PLAY to attempt mount "
                              "and rescan for apps");
            lv_label_set_text(s_extras_foot, "HOME back");
        } else {
            lv_label_set_text(s_none_head, "Nothing found :(");
            /* What is observable, not a diagnosis. An empty volume and a map
               that came up wrong look identical from here. */
            lv_label_set_text(s_none_body,
                              "The volume is mounted but\nn31os/apps is not\n"
                              "readable on it.");
            lv_label_set_text(s_none_hint, "Press PLAY to try again");
            lv_label_set_text(s_extras_foot, "HOME back");
        }

        lv_screen_load(s_extras);
        return;
    }

    show(s_none, false);
    lv_label_set_text(s_extras_foot, "PLAY open     HOME back");

    if (selected < (int)n31_extra_first) selected = (int)n31_extra_first;
    if (selected >= (int)n31_app_count)  selected = (int)n31_app_count - 1;
    s_selected = selected;

    /* Scroll only as far as it must, so the selection walking down the list
       does not jump the whole page under it. */
    if (s_selected < s_first) s_first = s_selected;
    if (s_selected >= s_first + ROWS) s_first = s_selected - ROWS + 1;

    int max_first = (int)n31_app_count - ROWS;
    if (max_first < (int)n31_extra_first) max_first = (int)n31_extra_first;
    if (s_first > max_first) s_first = max_first;
    if (s_first < (int)n31_extra_first) s_first = (int)n31_extra_first;

    for (int i = 0; i < ROWS; i++) fill_row(&s_row[i], s_first + i);
    update_scrollbar();

    lv_screen_load(s_extras);
}

void n31_ui_extras_opening(bool on)
{
    s_opening = on;
    for (int i = 0; i < ROWS; i++) fill_row(&s_row[i], s_first + i);
}

/* ---- mounting ------------------------------------------------------------- */

static lv_obj_t *s_mount;
static lv_obj_t *s_mount_head;
static lv_obj_t *s_mount_stage;
static lv_obj_t *s_mount_fill;
static lv_obj_t *s_mount_secs;
static lv_obj_t *s_mount_foot;

#define BAR_Y 190
#define BAR_H 6

static void build_mount(void)
{
    s_mount = lv_obj_create(NULL);
    flat(s_mount, C_BG);
    lv_obj_set_size(s_mount, N31_SCREEN_W, N31_SCREEN_H);

    header(s_mount, NULL);

    s_mount_head = centred(s_mount, "Mounting", F_NAME, C_TEXT, 150, 28);

    panel(s_mount, MARGIN, BAR_Y, CONTENT_W, BAR_H, C_SURFACE_2);
    s_mount_fill = panel(s_mount, MARGIN, BAR_Y, 1, BAR_H, C_EXTRAS);

    s_mount_stage = centred(s_mount, "", F_CAPTION, C_TEXT_DIM, BAR_Y + 18, 18);

    /* The elapsed time is the only thing that moves during the long gap in the
       middle, and without it a stalled bar and a slow one look the same. */
    s_mount_secs = centred(s_mount, "", F_CAPTION, C_TEXT_MUTE, BAR_Y + 40, 18);

    s_mount_foot = footer(s_mount, "");
}

void n31_ui_mounting(int pct, const char *text, int secs)
{
    lv_label_set_text(s_mount_head, "Mounting");
    lv_obj_set_style_text_color(s_mount_head, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_color(s_mount_fill, lv_color_hex(C_EXTRAS), 0);

    /* -1 from the driver means this phase has no known total. Drawing that as
       0% would pin the bar at the left edge through the longest part of the
       wait, which is exactly when it must not look stuck - so the track is
       filled faintly instead, saying "working, no idea how far". */
    if (pct < 0) {
        lv_obj_set_size(s_mount_fill, CONTENT_W, BAR_H);
        lv_obj_set_style_bg_color(s_mount_fill, lv_color_hex(C_SURFACE_2), 0);
    } else {
        if (pct > 100) pct = 100;
        int w = CONTENT_W * pct / 100;
        lv_obj_set_size(s_mount_fill, w < 1 ? 1 : w, BAR_H);
    }

    if (text) lv_label_set_text(s_mount_stage, text);

    char t[32];
    snprintf(t, sizeof t, "%d s", secs);
    lv_label_set_text(s_mount_secs, t);

    /* Cancel has to be advertised. A minute-long bar with no way out reads as
       a hang, and the user has no reason to guess that HOME still works. */
    lv_label_set_text(s_mount_foot, "HOME cancel");
    lv_screen_load(s_mount);
}

void n31_ui_mount_failed(const char *reason)
{
    lv_label_set_text(s_mount_head, "Could not mount");
    lv_obj_set_style_text_color(s_mount_head, lv_color_hex(C_WARN), 0);
    lv_obj_set_style_bg_color(s_mount_fill, lv_color_hex(C_WARN), 0);

    lv_label_set_text(s_mount_stage, reason ? reason : "");
    lv_label_set_text(s_mount_secs, "");
    lv_label_set_text(s_mount_foot, "PLAY retry     HOME back");
    lv_screen_load(s_mount);
}

/* ---- starting ------------------------------------------------------------- */

static lv_obj_t *s_start;
static lv_obj_t *s_start_name;
static lv_obj_t *s_start_rule;

static void build_starting(void)
{
    s_start = lv_obj_create(NULL);
    flat(s_start, C_BG);
    lv_obj_set_size(s_start, N31_SCREEN_W, N31_SCREEN_H);

    s_start_name = centred(s_start, "", F_BIG, C_TEXT, 186, 32);

    /* A rule in the app's own colour, so the screen that appears is visibly
       about the app that was chosen rather than a generic please-wait. */
    s_start_rule = panel(s_start, 90, 226, 60, 2, C_TEXT_MUTE);

    centred(s_start, "starting", F_CAPTION, C_TEXT_MUTE, 240, 18);
}

void n31_ui_starting(const char *name, uint32_t accent)
{
    lv_label_set_text(s_start_name, name ? name : "");
    lv_obj_set_style_bg_color(s_start_rule, lv_color_hex(accent), 0);
    lv_screen_load(s_start);
}

/* ---- shared --------------------------------------------------------------- */

void n31_ui_init(void)
{
    lv_display_t *d = lv_display_get_default();
    if (d)
        lv_theme_default_init(d, lv_color_hex(C_RADIO), lv_color_hex(C_TEXT_DIM),
                              true, F_BODY);

    build_home();
    build_extras();
    build_mount();
    build_starting();

    n31_ui_home();
}

void n31_ui_status(const char *text)
{
    const char *t = text ? text : "";
    if (s_home_status)   lv_label_set_text(s_home_status, t);
    if (s_extras_status) lv_label_set_text(s_extras_status, t);
}

void n31_ui_redraw(void)
{
    /* Called when an app exits. It has been drawing over the framebuffer, and
       LVGL believes the screen still holds what it last drew, so everything
       has to be marked dirty rather than only what changed. */
    lv_obj_t *s = lv_screen_active();
    if (s) lv_obj_invalidate(s);
}

/*
 * tg_menu_raw.c — see tg_menu_raw.h.
 */

#include "tg_menu_raw.h"

#include "../platform/tg_palette.h"
#include "../platform/tg_text.h"
#include "../platform/tg_util.h"

#include <string.h>

/* ---- layout ---------------------------------------------------------------
 * The nano's settings screens: a 44 px bar, 48 px rows, a hairline between
 * them, 12 px of margin. Written out, not derived. */
#define W        240
#define H        432
#define TITLE_H  44
#define ROW_H    48
#define MARGIN   12
#define LIST_Y   TITLE_H
#define LIST_H   (H - LIST_Y)
#define NOTE_H   22

#define EDGE_PX      24    /* a swipe that starts this close to the left edge is "back" */
#define SWIPE_PX     60    /* ...once it has travelled this far right */
#define DRAG_PX       8    /* travel past this is a drag, not a tap */
#define REPEAT_FIRST 380
#define REPEAT_NEXT   90

/* What a row is, which decides what is drawn at its right-hand end. */
enum { ROW_PLAIN, ROW_CHEVRON, ROW_TOGGLE_ON, ROW_TOGGLE_OFF, ROW_CHECKED, ROW_DIM, ROW_INFO };

/* ---- rows ----------------------------------------------------------------- */

static bool has_resume_row(const tg_menu_raw *m)
{
    return m->st->have_state && m->st->rom_path[0] && !m->st->have_game;
}

static unsigned roms_count(const tg_menu_raw *m)
{
    unsigned n = m->lib ? m->lib->n : 0;

    return n + (has_resume_row(m) ? 1 : 0) + 2;   /* + Settings, About */
}

static unsigned row_count(const tg_menu_raw *m)
{
    switch (m->page) {
    case TG_MR_PAGE_ROMS:     return roms_count(m);
    case TG_MR_PAGE_PAUSE:    return m->st->have_state ? 6 : 5;
    case TG_MR_PAGE_SETTINGS: return 5;
    case TG_MR_PAGE_PALETTE:  return tg_palette_count();
    case TG_MR_PAGE_ABOUT:    return 6;
    }
    return 0;
}

static void display_name(const char *file, char *out, size_t cap)
{
    const char *dot = tg_strrchr(file, '.');
    size_t n = dot ? (size_t)(dot - file) : strlen(file);

    if (n >= cap) n = cap - 1;
    memcpy(out, file, n);
    out[n] = 0;
}

/* The label and kind of row `i` on the current page. `value` is the text at
   the right (a palette name, say), empty when there is none. */
static int row_info(const tg_menu_raw *m, unsigned i, char *label, size_t cap,
                    char *value, size_t vcap)
{
    const tg_menu_state *st = m->st;

    value[0] = 0;
    switch (m->page) {
    case TG_MR_PAGE_ROMS: {
        unsigned n = m->lib ? m->lib->n : 0;
        unsigned k = i;

        if (has_resume_row(m)) {
            if (k == 0) {
                tg_strlcpy(label, "Resume ", cap);
                tg_strlcat(label, st->rom_title, cap);
                return ROW_CHEVRON;
            }
            k--;
        }
        if (k < n) {
            display_name(m->lib->name[k], label, cap);
            if (m->rom_size && m->rom_max > 0 && m->rom_size(k, m->user) > m->rom_max) {
                char num[16];

                tg_strlcpy(value, tg_utoa((unsigned long)(m->rom_size(k, m->user) / 1024), num, sizeof num), vcap);
                tg_strlcat(value, " KB", vcap);
                return ROW_DIM;
            }
            return ROW_PLAIN;
        }
        k -= n;
        tg_strlcpy(label, k == 0 ? "Settings" : "About", cap);
        return ROW_CHEVRON;
    }
    case TG_MR_PAGE_PAUSE: {
        static const char *const rows_state[6] = {
            "Resume", "Save state", "Load state", "Restart cartridge", "Settings", "Choose another game" };
        static const char *const rows_plain[5] = {
            "Resume", "Save state", "Restart cartridge", "Settings", "Choose another game" };
        const char *const *rows = st->have_state ? rows_state : rows_plain;

        tg_strlcpy(label, rows[i], cap);
        if (strcmp(rows[i], "Settings") == 0) return ROW_CHEVRON;
        return ROW_PLAIN;
    }
    case TG_MR_PAGE_SETTINGS:
        switch (i) {
        case 0: tg_strlcpy(label, "Palette", cap);
                tg_strlcpy(value, tg_palette_at(st->palette)->name, vcap);
                return ROW_CHEVRON;
        case 1: tg_strlcpy(label, "Smooth scaling", cap);
                return st->smooth ? ROW_TOGGLE_ON : ROW_TOGGLE_OFF;
        case 2: tg_strlcpy(label, "Tilt d-pad", cap);
                return st->tilt ? ROW_TOGGLE_ON : ROW_TOGGLE_OFF;
        case 3: tg_strlcpy(label, "Show counters", cap);
                return st->overlay ? ROW_TOGGLE_ON : ROW_TOGGLE_OFF;
        default: tg_strlcpy(label, "About", cap); return ROW_CHEVRON;
        }
    case TG_MR_PAGE_PALETTE:
        tg_strlcpy(label, tg_palette_at(i)->name, cap);
        return (unsigned)st->palette == i ? ROW_CHECKED : ROW_PLAIN;
    case TG_MR_PAGE_ABOUT: {
        const char *l;

        switch (i) {
        case 0: tg_strlcpy(label, "TinyGB", cap);
                tg_strlcpy(value, m->build ? m->build : "", vcap); return ROW_INFO;
        case 1: tg_strlcpy(label, "Core", cap);
                tg_strlcpy(value, m->core_name ? m->core_name : "-", vcap); return ROW_INFO;
        default:
            l = m->recent_log ? m->recent_log(i - 2) : NULL;
            tg_strlcpy(label, l ? l : (i == 2 ? "nothing to report" : ""), cap);
            return ROW_INFO;
        }
    }
    }
    label[0] = 0;
    return ROW_PLAIN;
}

static const char *page_title(const tg_menu_raw *m)
{
    switch (m->page) {
    case TG_MR_PAGE_ROMS:     return "TinyGB";
    case TG_MR_PAGE_PAUSE:    return m->st->rom_title[0] ? m->st->rom_title : "Paused";
    case TG_MR_PAGE_SETTINGS: return "Settings";
    case TG_MR_PAGE_PALETTE:  return "Palette";
    default:                  return "About";
    }
}

/* ---- drawing -------------------------------------------------------------- */

static void draw_check(const tg_surface *fb, int x, int y, uint32_t c)
{
    /* A tick, out of rectangles: the short stroke down-right, the long one
       up-right. 14 px wide, 10 tall. */
    for (int i = 0; i < 4; i++)  tg_surface_rect(fb, x + i,     y + 5 + i, 3, 3, c);
    for (int i = 0; i < 9; i++)  tg_surface_rect(fb, x + 4 + i, y + 8 - i, 3, 3, c);
}

static void draw_toggle(const tg_surface *fb, int x, int y, bool on,
                        const tg_menu_theme *th)
{
    /* A track with a knob: 40 x 22, the knob at the end that means what it
       says. The nano's own is the same shape. */
    tg_surface_rect(fb, x, y, 40, 22, on ? th->primary : th->dim);
    tg_surface_rect(fb, x + (on ? 20 : 2), y + 2, 18, 18, on ? th->on_primary : th->surface);
}

static void draw(tg_menu_raw *m, const tg_surface *fb)
{
    const tg_menu_theme *th = &m->theme;
    const tg_font *f = &tg_font_ui;
    const char *title = page_title(m);
    unsigned n = row_count(m);
    int y;
    unsigned fit;
    char tbuf[64];

    tg_surface_rect(fb, 0, 0, W, H, th->bg);

    /* The title bar. Back chevron on every page that has somewhere to go
       back to; on the pause page that is the game. */
    tg_surface_rect(fb, 0, 0, W, TITLE_H, th->surface);
    if (m->page != TG_MR_PAGE_ROMS)
        tg_text_draw(fb, f, MARGIN, (TITLE_H - f->h) / 2, "<", 1, th->primary);
    tg_strlcpy(tbuf, title, sizeof tbuf);
    fit = tg_text_fit(f, tbuf, W - 2 * (MARGIN + 2 * f->w), 1);
    tbuf[fit] = 0;
    tg_text_draw(fb, f, (W - (int)tg_text_width(f, tbuf, 1)) / 2,
                 (TITLE_H - f->h) / 2, tbuf, 1, th->text);
    tg_surface_rect(fb, 0, TITLE_H - 1, W, 1, th->dim);

    if (m->page == TG_MR_PAGE_ROMS && (!m->lib_ok || !m->lib || m->lib->n == 0)) {
        const tg_font *sf = &tg_font_small;
        int ty = LIST_Y + 16;

        tg_text_draw(fb, f, MARGIN, ty, "No cartridges yet.", 1, th->text);
        ty += f->h + 10;
        tg_text_draw(fb, sf, MARGIN, ty, "Put .gb files in", 1, th->dim);          ty += sf->h + 3;
        tg_text_draw(fb, sf, MARGIN, ty, "/Apps/Data/TinyGB/roms", 1, th->dim);   ty += sf->h + 3;
        tg_text_draw(fb, sf, MARGIN, ty, "in disk mode, then relaunch.", 1, th->dim);
        if (!m->lib_ok) {
            ty += sf->h + 10;
            tg_text_draw(fb, sf, MARGIN, ty, "(the folder could not be read)", 1, th->dim);
        }
        /* The list below still shows Settings and About, pushed down. */
        y = LIST_Y + 120 - m->scroll[m->page];
    } else {
        y = LIST_Y - m->scroll[m->page];
    }

    for (unsigned i = 0; i < n; i++, y += ROW_H) {
        char label[96], value[32];
        int kind;
        uint32_t fg = th->text;
        bool lit = (int)i == m->press_row || (int)i == m->sel[m->page];
        int right = W - MARGIN;

        if (y + ROW_H <= LIST_Y || y >= H) continue;

        kind = row_info(m, i, label, sizeof label, value, sizeof value);
        if (kind == ROW_INFO) lit = false;

        if (lit) {
            tg_surface_rect(fb, 0, y, W, ROW_H, (int)i == m->press_row ? th->primary : th->surface);
            if ((int)i == m->press_row) fg = th->on_primary;
        }
        tg_surface_rect(fb, MARGIN, y + ROW_H - 1, W - MARGIN, 1, th->dim);

        switch (kind) {
        case ROW_CHEVRON:
            right -= f->w;
            tg_text_draw(fb, f, right, y + (ROW_H - f->h) / 2, ">", 1, th->dim);
            right -= 6;
            break;
        case ROW_TOGGLE_ON:
        case ROW_TOGGLE_OFF:
            right -= 40;
            draw_toggle(fb, right, y + (ROW_H - 22) / 2, kind == ROW_TOGGLE_ON, th);
            right -= 8;
            break;
        case ROW_CHECKED:
            right -= 14;
            draw_check(fb, right, y + (ROW_H - 10) / 2, th->primary);
            right -= 8;
            break;
        case ROW_DIM:
            fg = th->dim;
            break;
        default:
            break;
        }

        if (value[0]) {
            const tg_font *vf = kind == ROW_INFO ? &tg_font_small : f;
            int vw = (int)tg_text_width(vf, value, 1);

            right -= vw;
            tg_text_draw(fb, vf, right, y + (ROW_H - vf->h) / 2, value, 1, th->dim);
            right -= 8;
        }

        {
            const tg_font *lf = (kind == ROW_INFO && i >= 2) ? &tg_font_small : f;

            fit = tg_text_fit(lf, label, (unsigned)(right - MARGIN), 1);
            label[fit] = 0;
            tg_text_draw(fb, lf, MARGIN, y + (ROW_H - lf->h) / 2, label, 1, fg);
        }
    }

    if (m->page == TG_MR_PAGE_ROMS && m->lib && m->lib->skipped) {
        char line[48], num[16];

        tg_strlcpy(line, "and ", sizeof line);
        tg_strlcat(line, tg_utoa(m->lib->skipped, num, sizeof num), sizeof line);
        tg_strlcat(line, " more not listed", sizeof line);
        tg_text_draw(fb, &tg_font_small, MARGIN, y + 8, line, 1, th->dim);
    }

    if (m->note[0]) {
        tg_surface_rect(fb, 0, H - NOTE_H, W, NOTE_H, th->surface);
        tg_text_draw(fb, &tg_font_small, 6, H - NOTE_H + 4, m->note, 1, th->text);
    }

    m->dirty = false;
}

/* ---- navigation ----------------------------------------------------------- */

static int max_scroll(const tg_menu_raw *m)
{
    int extra = (m->page == TG_MR_PAGE_ROMS && (!m->lib_ok || !m->lib || m->lib->n == 0)) ? 120 : 0;
    int total = (int)row_count(m) * ROW_H + extra - LIST_H;

    return total > 0 ? total : 0;
}

static int row_at(const tg_menu_raw *m, int y)
{
    int extra = (m->page == TG_MR_PAGE_ROMS && (!m->lib_ok || !m->lib || m->lib->n == 0)) ? 120 : 0;
    int rel;

    if (y < LIST_Y) return -1;
    rel = y - LIST_Y - extra + m->scroll[m->page];
    if (rel < 0) return -1;
    if ((unsigned)(rel / ROW_H) >= row_count(m)) return -1;
    return rel / ROW_H;
}

static void keep_sel_visible(tg_menu_raw *m)
{
    int top = m->sel[m->page] * ROW_H;
    int *sc = &m->scroll[m->page];

    if (top < *sc) *sc = top;
    if (top + ROW_H > *sc + LIST_H) *sc = top + ROW_H - LIST_H;
    if (*sc > max_scroll(m)) *sc = max_scroll(m);
    if (*sc < 0) *sc = 0;
}

static void go(tg_menu_raw *m, int page, bool push)
{
    if (push && m->depth < 4) m->stack[m->depth++] = m->page;
    m->page = page;
    m->press_row = -1;
    m->dirty = true;
}

static int go_back(tg_menu_raw *m)
{
    if (m->depth > 0) {
        m->page = m->stack[--m->depth];
        m->press_row = -1;
        m->dirty = true;
        return TG_MENU_RAW_NONE;
    }
    if (m->page == TG_MR_PAGE_PAUSE) return TG_MENU_RESUME;
    return TG_MENU_RAW_NONE;
}

static int activate(tg_menu_raw *m, int row)
{
    tg_menu_state *st = m->st;

    if (row < 0) return TG_MENU_RAW_NONE;
    m->sel[m->page] = row;
    m->dirty = true;

    switch (m->page) {
    case TG_MR_PAGE_ROMS: {
        unsigned n = m->lib ? m->lib->n : 0;
        unsigned k = (unsigned)row;

        if (has_resume_row(m)) {
            if (k == 0) return TG_MENU_RAW_RESUME_STATE;
            k--;
        }
        if (k < n) {
            if (m->rom_size && m->rom_max > 0 && m->rom_size(k, m->user) > m->rom_max) {
                tg_menu_raw_note(m, "too big for this build (1 MB max)");
                return TG_MENU_RAW_NONE;
            }
            tg_strlcpy(st->rom_path, m->lib->dir, sizeof st->rom_path);
            tg_strlcat(st->rom_path, "/", sizeof st->rom_path);
            tg_strlcat(st->rom_path, m->lib->name[k], sizeof st->rom_path);
            display_name(m->lib->name[k], st->rom_title, sizeof st->rom_title);
            return TG_MENU_PLAY;
        }
        k -= n;
        go(m, k == 0 ? TG_MR_PAGE_SETTINGS : TG_MR_PAGE_ABOUT, true);
        return TG_MENU_RAW_NONE;
    }
    case TG_MR_PAGE_PAUSE: {
        int r = row;

        if (!st->have_state && r >= 2) r++;      /* no Load state row: renumber */
        switch (r) {
        case 0: return TG_MENU_RESUME;
        case 1: return TG_MENU_SAVE_STATE;
        case 2: return TG_MENU_LOAD_STATE;
        case 3: return TG_MENU_RESET;
        case 4: go(m, TG_MR_PAGE_SETTINGS, true); return TG_MENU_RAW_NONE;
        default: return TG_MENU_RAW_CHOOSE;
        }
    }
    case TG_MR_PAGE_SETTINGS:
        switch (row) {
        case 0: go(m, TG_MR_PAGE_PALETTE, true); return TG_MENU_RAW_NONE;
        case 1: st->smooth = !st->smooth;   return TG_MENU_RAW_SETTINGS_CHANGED;
        case 2: st->tilt = !st->tilt;       return TG_MENU_RAW_SETTINGS_CHANGED;
        case 3: st->overlay = !st->overlay; return TG_MENU_RAW_SETTINGS_CHANGED;
        default: go(m, TG_MR_PAGE_ABOUT, true); return TG_MENU_RAW_NONE;
        }
    case TG_MR_PAGE_PALETTE:
        st->palette = (unsigned)row;
        go_back(m);
        return TG_MENU_RAW_SETTINGS_CHANGED;
    default:
        return TG_MENU_RAW_NONE;
    }
}

/* ---- the interface -------------------------------------------------------- */

void tg_menu_raw_init(tg_menu_raw *m, tg_menu_state *st, const tg_menu_theme *theme)
{
    memset(m, 0, sizeof *m);
    m->st = st;
    m->theme = *theme;
    m->press_row = -1;
    m->prev_down = true;        /* a finger already down when we open is not a tap */
    m->prev_keys = 3;
    m->dirty = true;
}

void tg_menu_raw_set_library(tg_menu_raw *m, const tg_rom_list *lib, bool ok)
{
    m->lib = lib;
    m->lib_ok = ok;
    if (m->sel[TG_MR_PAGE_ROMS] >= (int)roms_count(m)) m->sel[TG_MR_PAGE_ROMS] = 0;
    m->dirty = true;
}

void tg_menu_raw_open(tg_menu_raw *m, bool pause)
{
    m->depth = 0;
    m->page = pause ? TG_MR_PAGE_PAUSE : TG_MR_PAGE_ROMS;
    m->sel[TG_MR_PAGE_PAUSE] = 0;
    m->scroll[TG_MR_PAGE_PAUSE] = 0;
    m->press_row = -1;
    m->prev_down = true;
    m->prev_keys = 3;
    m->note[0] = 0;
    m->dirty = true;
}

void tg_menu_raw_note(tg_menu_raw *m, const char *text)
{
    tg_strlcpy(m->note, text ? text : "", sizeof m->note);
    m->dirty = true;
}

void tg_menu_raw_invalidate(tg_menu_raw *m) { m->dirty = true; }

int tg_menu_raw_page(const tg_menu_raw *m) { return m->page; }

int tg_menu_raw_tick(tg_menu_raw *m, const tg_surface *fb, int tx, int ty,
                     bool down, unsigned keys, uint32_t now_ms)
{
    int action = TG_MENU_RAW_NONE;

    /* ---- touch ---- */
    if (down && !m->prev_down) {
        m->down_x = tx;
        m->down_y = m->last_y = ty;
        m->moved = 0;
        m->scroll_at_down = m->scroll[m->page];
        m->press_row = row_at(m, ty);
        if (m->press_row >= 0) m->dirty = true;
    } else if (down && m->prev_down) {
        int dy = ty - m->last_y;

        m->moved += dy < 0 ? -dy : dy;
        m->last_y = ty;
        if (m->moved > DRAG_PX) {
            int sc = m->scroll_at_down - (ty - m->down_y);

            if (sc < 0) sc = 0;
            if (sc > max_scroll(m)) sc = max_scroll(m);
            if (sc != m->scroll[m->page] || m->press_row >= 0) {
                m->scroll[m->page] = sc;
                m->press_row = -1;         /* a drag is not a press */
                m->dirty = true;
            }
        }
    } else if (!down && m->prev_down) {
        int row = m->press_row;

        m->press_row = -1;
        m->dirty = true;

        if (m->down_x < EDGE_PX && tx - m->down_x > SWIPE_PX && m->moved <= 3 * DRAG_PX) {
            action = go_back(m);           /* swiped in from the left edge */
        } else if (m->moved <= DRAG_PX) {
            if (ty < TITLE_H) {
                if (tx < TITLE_H + MARGIN && m->page != TG_MR_PAGE_ROMS)
                    action = go_back(m);   /* the chevron */
            } else if (row >= 0 && row == row_at(m, ty)) {
                action = activate(m, row);
            }
        }
    }
    m->prev_down = down;

    /* ---- keys: edges, with repeat on the two that move ---- */
    {
        unsigned fresh = keys & ~m->prev_keys;
        bool repeat = false;

        if (keys && keys == m->prev_keys) {
            if (now_ms - m->key_down_ms >= REPEAT_FIRST &&
                now_ms - m->key_repeat_ms >= REPEAT_NEXT) {
                repeat = true;
                m->key_repeat_ms = now_ms;
            }
        } else if (keys) {
            m->key_down_ms = m->key_repeat_ms = now_ms;
        }
        if (fresh || repeat) {
            unsigned k = fresh ? fresh : keys;
            int n = (int)row_count(m);

            if ((k & 1) && m->sel[m->page] > 0) { m->sel[m->page]--; keep_sel_visible(m); m->dirty = true; }
            if ((k & 2) && m->sel[m->page] + 1 < n) { m->sel[m->page]++; keep_sel_visible(m); m->dirty = true; }
        }
        m->prev_keys = keys;
    }

    if (m->dirty) draw(m, fb);
    return action;
}

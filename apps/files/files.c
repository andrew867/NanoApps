/*
 * file_browser — drill into iPod filesystem directories, view text
 * files inline, copy / rename / delete.
 *
 * Three views:
 *   - List: header (path + Up / Volume buttons) and a scrollable list
 *     of entries (directories first, then files). Tap a directory to
 *     descend; tap a file to open a viewer. Long-press an entry to
 *     show file ops (delete, copy, rename).
 *   - Text viewer: read the file's first ~4 KB and show it.
 *   - Op confirm: confirmation modal for destructive ops.
 *
 * Browse roots (RetailOS volume ids):
 *   - FAT / Main (0): USB-visible music disk ("/" when Connected exports)
 *   - Root / Internal (1): internal system tree (Device/, GrapeFirmware, …)
 *   - Resources (4): bundled UI/sounds via hb_fs_dir_open_at
 *
 * Copy/paste: a single "clipboard" path + source volume. Tapping "Copy"
 * remembers path+vol; "Paste" writes into the current directory/volume
 * with the same basename (cross-volume OK — e.g. Root → FAT for grape).
 *
 * For binary files, the viewer warns and shows file size only — no
 * decoders for images / WAV yet. Tracked as a follow-up.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "lvgl/lvgl.h"

/* Up to MAX_ENTRIES entries cached in BSS (just name + is_dir, ~64 B
   each → 12 KB). The display only renders ROWS_PER_PAGE at a time as
   LVGL widgets so the heap doesn't blow up — see
   feedback_lvgl_widget_budget. Prev/Next page buttons appear in the
   header when there are more than one page worth. */
#define MAX_ENTRIES    400
#define ROWS_PER_PAGE  25
#define MAX_NAME_LEN  64
#define PREVIEW_BYTES 4096
#define VOL_MAIN      0   /* FAT / USB-visible */
#define VOL_ROOT      1   /* Internal root FS */
#define VOL_RES       4   /* Resources bundle */

typedef struct {
    char name[MAX_NAME_LEN];
    bool is_dir;
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static int     s_n_entries = 0;
static int     s_page = 0;
/* Set by the long-press detector so the CLICKED handler that LVGL
   fires immediately after the release knows to ignore the tap (and
   not "activate" the file the user was just trying to flag). */
static bool    s_suppress_next_click = false;
static char    s_cwd[256] = "/";
static int     s_vol = VOL_MAIN;

/* Clipboard (set by "Copy" action). Path + source volume. */
static char    s_clip_path[256] = {0};
static int     s_clip_vol = VOL_MAIN;
static bool    s_clip_set = false;

/* "Selected" entry for the action sheet. */
static int     s_sel_idx = -1;

typedef enum { VIEW_LIST, VIEW_TEXT, VIEW_ACTIONS, VIEW_RENAME } view_t;
static view_t  s_view = VIEW_LIST;

static lv_obj_t *s_scr;
static lv_obj_t *s_list_view;
static lv_obj_t *s_text_view;
static lv_obj_t *s_actions_view;
static lv_obj_t *s_rename_view;

static lv_obj_t *s_path_lbl;
static lv_obj_t *s_list_box;
static lv_obj_t *s_paste_btn;
static lv_obj_t *s_text_lbl;
static lv_obj_t *s_actions_title;
static lv_obj_t *s_rename_ta;

/* ---- string helpers ---- */

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void s_copy(char *dst, const char *src, int cap)
{
    int i = 0; while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void path_append(char *cwd, const char *name)
{
    int n = 0; while (cwd[n]) n++;
    if (n == 0 || cwd[n-1] != '/') { cwd[n++] = '/'; cwd[n] = 0; }
    int i = 0;
    while (name[i] && n < 254) cwd[n++] = name[i++];
    cwd[n] = 0;
}

static void path_parent(char *cwd)
{
    int n = 0; while (cwd[n]) n++;
    if (n <= 1) return;
    if (cwd[n-1] == '/') cwd[--n] = 0;
    while (n > 0 && cwd[n-1] != '/') cwd[--n] = 0;
    if (n == 0) { cwd[0] = '/'; cwd[1] = 0; }
    else if (n > 1) cwd[n-1] = 0;
}

static const char *base_name(const char *path)
{
    int n = 0; while (path[n]) n++;
    while (n > 0 && path[n-1] != '/') n--;
    return path + n;
}

/* ---- view machinery ---- */

static void show_list(void);
static void show_text(const char *path);
static void show_actions(int idx);
static void show_rename(int idx);

/* ---- scan ---- */

static int s_cmp(const entry_t *a, const entry_t *b)
{
    /* Directories first, then alpha (case-insensitive). */
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    const char *p = a->name, *q = b->name;
    while (*p && *q) {
        char ca = *p, cb = *q;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
        p++; q++;
    }
    return *p ? 1 : (*q ? -1 : 0);
}

static void sort_entries(void)
{
    for (int i = 1; i < s_n_entries; i++) {
        entry_t cur = s_entries[i];
        int j = i;
        while (j > 0 && s_cmp(&s_entries[j-1], &cur) > 0) {
            s_entries[j] = s_entries[j-1];
            j--;
        }
        s_entries[j] = cur;
    }
}

static void scan(void)
{
    s_n_entries = 0;
    hb_dir_t d;
    bool ok = hb_fs_dir_open_at(&d, s_cwd, false, (uint8_t)s_vol);
    if (!ok) return;
    char fn[MAX_NAME_LEN]; bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (fn[0] == '.') continue;
        if (s_n_entries >= MAX_ENTRIES) break;
        s_copy(s_entries[s_n_entries].name, fn, MAX_NAME_LEN);
        s_entries[s_n_entries].is_dir = is_dir;
        s_n_entries++;
    }
    hb_fs_dir_close(&d);
    sort_entries();
}

/* ---- list view ---- */

/* case-insensitive: does `name` end with "."+ext ? */
static bool name_has_ext(const char *name, const char *ext)
{
    int nl = 0, el = 0;
    while (name[nl]) nl++;
    while (ext[el]) el++;
    if (nl < el + 1 || name[nl - el - 1] != '.') return false;
    for (int i = 0; i < el; i++)
        if ((name[nl - el + i] | 0x20) != (ext[i] | 0x20)) return false;
    return true;
}

/* A scroll started in the list — eat the click that may land when the flick
   stops on a row (otherwise letting go "taps" that row). */
static void on_list_scroll_begin(lv_event_t *e)
{
    (void)e;
    s_suppress_next_click = true;
}

static void on_row_clicked(lv_event_t *e)
{
    if (s_suppress_next_click) {
        s_suppress_next_click = false;
        return;
    }
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_entries) return;
    entry_t *ent = &s_entries[idx];
    if (ent->is_dir) {
        path_append(s_cwd, ent->name);
        s_page = 0;
        scan();
        show_list();
    } else {
        char path[256]; s_copy(path, s_cwd, sizeof path);
        path_append(path, ent->name);
        if (name_has_ext(ent->name, "wav")) {
            /* Play from the volume we're browsing: the SFX loader's volume id must
             * match. 0 is the on-disk filesystem (the user's own .wav), 4 is the OS 
             * resource bundle. */
            if (s_vol == VOL_RES)
                hb_audio_play_wav(path, 0x4000);        /* resource bundle (vol 4) */
            else
                hb_audio_play_wav_main(path, 0x4000);   /* main filesystem (vol 0) */
        } else {
            show_text(path);                   /* default: text viewer            */
        }
    }
}

/* Long-press a row → action sheet (delete / copy / rename). */
static int s_lp_pressed_idx = -1;
static uint32_t s_lp_t_ms;

static void on_row_press(lv_event_t *e)
{
    s_lp_pressed_idx = (int)(uintptr_t)lv_event_get_user_data(e);
    s_lp_t_ms = hb_time_uptime_ms();
}
static void on_row_release(lv_event_t *e)
{
    (void)e;
    s_lp_pressed_idx = -1;
}

static void on_frame(void)
{
    if (s_lp_pressed_idx < 0) return;
    uint32_t now = hb_time_uptime_ms();
    if ((uint32_t)(now - s_lp_t_ms) >= 500u) {
        int idx = s_lp_pressed_idx;
        s_lp_pressed_idx = -1;
        s_suppress_next_click = true;     /* eat the upcoming CLICKED */
        show_actions(idx);
    }
}

static void on_up(lv_event_t *e)
{
    (void)e;
    path_parent(s_cwd);
    s_page = 0;
    scan();
    show_list();
}

static void on_vol_changed(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    /* 0=FAT (VOL_MAIN), 1=Root/Internal (VOL_ROOT), 2=Resources (VOL_RES). */
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel == 0)
        s_vol = VOL_MAIN;
    else if (sel == 1)
        s_vol = VOL_ROOT;
    else
        s_vol = VOL_RES;
    s_copy(s_cwd, "/", sizeof s_cwd);
    s_page = 0;
    scan();
    show_list();
}

static void on_page_prev(lv_event_t *e) { (void)e; if (s_page > 0) { s_page--; show_list(); } }
static void on_page_next(lv_event_t *e)
{
    (void)e;
    int n_pages = (s_n_entries + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (s_page + 1 < n_pages) { s_page++; show_list(); }
}

static void on_paste(lv_event_t *e)
{
    (void)e;
    if (!s_clip_set) return;
    /* Read clipboard file (source vol) into heap, write into cwd on current vol. */
    uint32_t sz = hb_fs_size_at(s_clip_path, s_clip_vol);
    if (sz == 0 || sz > 2 * 1024 * 1024) return;       /* skip huge files */
    char *buf = lv_malloc(sz);
    if (!buf) return;
    uint32_t rd = hb_fs_read_at(s_clip_path, buf, sz, s_clip_vol);
    if (rd == 0) { lv_free(buf); return; }
    char dest[256]; s_copy(dest, s_cwd, sizeof dest);
    path_append(dest, base_name(s_clip_path));
    hb_fs_write_at(dest, buf, rd, s_vol);
    lv_free(buf);
    scan();
    show_list();
}

/* ---- text viewer ---- */

static void on_text_back(lv_event_t *e) { (void)e; show_list(); }

static bool looks_binary(const char *buf, uint32_t n)
{
    /* Heuristic: more than ~5% non-printable, non-whitespace → binary. */
    int bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 0x20 || c >= 0x7F) bad++;
    }
    return n > 0 && bad * 20 > (int)n;
}

static void show_text(const char *path)
{
    s_view = VIEW_TEXT;
    static char text[PREVIEW_BYTES + 32];
    uint32_t total = hb_fs_size_at(path, s_vol);
    uint32_t rd = hb_fs_read_at(path, text, PREVIEW_BYTES, s_vol);
    text[rd] = 0;
    if (looks_binary(text, rd)) {
        /* Unsupported / binary file -> hex dump (folded in from the Toolbox hex
           viewer): offset + 16 bytes + ASCII, first 512 bytes. */
        static char hex[4096];
        static const char HX[] = "0123456789abcdef";
        uint32_t n = rd > 512 ? 512 : rd;
        int k = 0;
        for (uint32_t off = 0; off < n; off += 16) {
            for (int s = 12; s >= 0; s -= 4) hex[k++] = HX[(off >> s) & 0xf];
            hex[k++] = ':'; hex[k++] = ' ';
            for (int i = 0; i < 16; i++) {
                if (off + (uint32_t)i < n) {
                    uint8_t b = (uint8_t)text[off + i];
                    hex[k++] = HX[b >> 4]; hex[k++] = HX[b & 0xf]; hex[k++] = ' ';
                } else { hex[k++] = ' '; hex[k++] = ' '; hex[k++] = ' '; }
            }
            hex[k++] = ' ';
            for (int i = 0; i < 16 && off + (uint32_t)i < n; i++) {
                uint8_t b = (uint8_t)text[off + i];
                hex[k++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            }
            hex[k++] = '\n';
        }
        if (total > n) {
            const char *p = "...("; while (*p) hex[k++] = *p++;
            char tmp[12]; int ti = 0; uint32_t v = total;
            if (v == 0) tmp[ti++] = '0';
            while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
            while (ti) hex[k++] = tmp[--ti];
            p = " bytes)"; while (*p) hex[k++] = *p++;
        }
        hex[k] = 0;
        lv_label_set_text(s_text_lbl, hex);
    } else {
        /* Strip control bytes that aren't whitespace, in place. */
        for (uint32_t i = 0; i < rd; i++) {
            uint8_t c = (uint8_t)text[i];
            if (c == '\n' || c == '\r' || c == '\t') continue;
            if (c < 0x20 || c >= 0x7F) text[i] = '.';
        }
        lv_label_set_text(s_text_lbl, text);
    }
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_text_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_actions_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rename_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- actions ---- */

static void on_actions_close(lv_event_t *e) { (void)e; show_list(); }

static void on_action_delete(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0 || s_sel_idx >= s_n_entries) { show_list(); return; }
    entry_t *ent = &s_entries[s_sel_idx];
    char path[256]; s_copy(path, s_cwd, sizeof path);
    path_append(path, ent->name);
    if (ent->is_dir) hb_fs_rmrf_at(path, s_vol);
    else             hb_fs_remove_at(path, s_vol);
    scan();
    show_list();
}

static void on_action_copy(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0 || s_sel_idx >= s_n_entries) { show_list(); return; }
    entry_t *ent = &s_entries[s_sel_idx];
    if (ent->is_dir) { show_list(); return; }       /* dir copy unsupported */
    s_copy(s_clip_path, s_cwd, sizeof s_clip_path);
    path_append(s_clip_path, ent->name);
    s_clip_vol = s_vol;
    s_clip_set = true;
    show_list();
}

static void on_action_rename(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0 || s_sel_idx >= s_n_entries) { show_list(); return; }
    show_rename(s_sel_idx);
}

static void show_actions(int idx)
{
    s_sel_idx = idx;
    s_view = VIEW_ACTIONS;
    lv_label_set_text(s_actions_title, s_entries[idx].name);
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_text_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_actions_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rename_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- rename ---- */

static void on_rename_save(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0 || s_sel_idx >= s_n_entries) { show_list(); return; }
    const char *new_name = lv_textarea_get_text(s_rename_ta);
    int nlen = 0; while (new_name[nlen]) nlen++;
    if (nlen == 0) { show_list(); return; }
    entry_t *ent = &s_entries[s_sel_idx];
    if (ent->is_dir) { show_list(); return; }     /* dir rename unsupported */
    char src[256]; s_copy(src, s_cwd, sizeof src); path_append(src, ent->name);
    char dst[256]; s_copy(dst, s_cwd, sizeof dst); path_append(dst, new_name);
    /* Copy via heap, then remove source (same volume). */
    uint32_t sz = hb_fs_size_at(src, s_vol);
    if (sz == 0 || sz > 2 * 1024 * 1024) { show_list(); return; }
    char *buf = lv_malloc(sz);
    if (!buf) { show_list(); return; }
    uint32_t rd = hb_fs_read_at(src, buf, sz, s_vol);
    if (rd > 0 && hb_fs_write_at(dst, buf, rd, s_vol)) {
        hb_fs_remove_at(src, s_vol);
    }
    lv_free(buf);
    scan();
    show_list();
}

static void on_rename_cancel(lv_event_t *e) { (void)e; show_list(); }

static void show_rename(int idx)
{
    s_view = VIEW_RENAME;
    s_sel_idx = idx;
    lv_textarea_set_text(s_rename_ta, s_entries[idx].name);
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_text_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_actions_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_rename_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- list rebuild ---- */

static void show_list(void)
{
    s_view = VIEW_LIST;
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_text_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_actions_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rename_view, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_path_lbl, s_cwd);
    if (s_clip_set) lv_obj_clear_flag(s_paste_btn, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_paste_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_list_box);
    if (s_n_entries == 0) {
        lv_obj_t *empty = lv_label_create(s_list_box);
        lv_label_set_text(empty, "(empty directory)");
        lv_obj_set_style_text_color(empty, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    int n_pages = (s_n_entries + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (s_page >= n_pages) s_page = n_pages - 1;
    if (s_page < 0) s_page = 0;
    int start = s_page * ROWS_PER_PAGE;
    int end = start + ROWS_PER_PAGE;
    if (end > s_n_entries) end = s_n_entries;

    for (int i = start; i < end; i++) {
        entry_t *ent = &s_entries[i];
        lv_obj_t *row = lv_button_create(s_list_box);
        lv_obj_set_size(row, 224, 32);
        lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_surface()), 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_margin_bottom(row, 2, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, on_row_clicked,  LV_EVENT_CLICKED,  (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, on_row_press,    LV_EVENT_PRESSED,  (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, on_row_release,  LV_EVENT_RELEASED, NULL);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, ent->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
        lv_obj_set_style_text_color(icon,
            ent->is_dir ? lv_color_hex(hb_color_primary()) : lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *lab = lv_label_create(row);
        lv_label_set_text(lab, ent->name);
        lv_obj_set_style_text_color(lab, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lab, 188);
        lv_obj_align(lab, LV_ALIGN_LEFT_MID, 32, 0);
    }

    if (n_pages > 1) {
        /* Footer row with prev / page label / next. Lives inside the
           list_box so it scrolls naturally with the list content. */
        lv_obj_t *footer = lv_obj_create(s_list_box);
        lv_obj_set_size(footer, 224, 36);
        lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(footer, 0, 0);
        lv_obj_set_style_pad_all(footer, 0, 0);
        lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *prev = lv_button_create(footer);
        lv_obj_set_size(prev, 60, 28);
        lv_obj_align(prev, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(prev, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_add_event_cb(prev, on_page_prev, LV_EVENT_CLICKED, NULL);
        lv_obj_t *pl = lv_label_create(prev);
        lv_label_set_text(pl, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(pl, lv_color_hex(hb_color_text()), 0);
        lv_obj_center(pl);

        lv_obj_t *info = lv_label_create(footer);
        char buf[24]; int k = 0;
        const char *p = "Page "; while (*p) buf[k++] = *p++;
        char nb[8];
        int v = s_page + 1, ti = 0;
        if (v == 0) nb[ti++] = '0';
        while (v) { nb[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) buf[k++] = nb[--ti];
        buf[k++] = '/';
        v = n_pages; ti = 0;
        if (v == 0) nb[ti++] = '0';
        while (v) { nb[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) buf[k++] = nb[--ti];
        buf[k] = 0;
        lv_label_set_text(info, buf);
        lv_obj_set_style_text_color(info, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
        lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *next = lv_button_create(footer);
        lv_obj_set_size(next, 60, 28);
        lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(next, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_add_event_cb(next, on_page_next, LV_EVENT_CLICKED, NULL);
        lv_obj_t *nl = lv_label_create(next);
        lv_label_set_text(nl, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
        lv_obj_center(nl);
    }
}

/* ---- view construction ---- */

static void build_list_view(void)
{
    s_list_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_list_view, 240, 432);
    lv_obj_set_style_radius(s_list_view, 0, 0);
    lv_obj_set_pos(s_list_view, 0, 0);
    lv_obj_set_style_bg_color(s_list_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_view, 0, 0);
    lv_obj_set_style_pad_all(s_list_view, 0, 0);
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *up = lv_button_create(s_list_view);
    lv_obj_set_size(up, 32, 32);
    lv_obj_align(up, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(up, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(up, on_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ul = lv_label_create(up);
    lv_label_set_text(ul, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(ul, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(ul);

    /* Volume selector — FAT (USB), Root (internal), Resources. */
    lv_obj_t *vol = lv_dropdown_create(s_list_view);
    lv_dropdown_set_options(vol, "FAT\nRoot\nResources");
    lv_dropdown_set_selected(vol, 0);                 /* FAT (VOL_MAIN) */
    lv_obj_set_width(vol, 118);
    lv_obj_align(vol, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_event_cb(vol, on_vol_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_paste_btn = lv_button_create(s_list_view);
    lv_obj_set_size(s_paste_btn, 60, 32);
    lv_obj_align(s_paste_btn, LV_ALIGN_TOP_RIGHT, -126, 4);   /* left of the volume dropdown */
    lv_obj_set_style_bg_color(s_paste_btn, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(s_paste_btn, on_paste, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(s_paste_btn);
    lv_label_set_text(pl, "Paste");
    lv_obj_set_style_text_color(pl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(pl);
    lv_obj_add_flag(s_paste_btn, LV_OBJ_FLAG_HIDDEN);

    s_path_lbl = lv_label_create(s_list_view);
    lv_label_set_text(s_path_lbl, "/");
    lv_obj_set_style_text_color(s_path_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_path_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_path_lbl, 232);
    lv_label_set_long_mode(s_path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_path_lbl, LV_ALIGN_TOP_MID, 0, 42);

    s_list_box = lv_obj_create(s_list_view);
    lv_obj_set_size(s_list_box, 240, 432 - 70);
    lv_obj_align(s_list_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 6, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    /* Suppress the row tap that LVGL sometimes fires when a scroll/flick ends on a
       row, so letting go after a scroll doesn't accidentally open an item. */
    lv_obj_add_event_cb(s_list_box, on_list_scroll_begin, LV_EVENT_SCROLL_BEGIN, NULL);
}

static void build_text_view(void)
{
    s_text_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_text_view, 240, 432);
    lv_obj_set_style_radius(s_text_view, 0, 0);
    lv_obj_set_pos(s_text_view, 0, 0);
    lv_obj_set_style_bg_color(s_text_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_text_view, 0, 0);
    lv_obj_set_style_pad_all(s_text_view, 0, 0);
    lv_obj_clear_flag(s_text_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_text_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_text_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_text_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *scroll = lv_obj_create(s_text_view);
    lv_obj_set_size(scroll, 232, 380);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(0x111522), 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 8, 0);
    s_text_lbl = lv_label_create(scroll);
    lv_label_set_text(s_text_lbl, "");
    lv_obj_set_style_text_color(s_text_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_text_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_text_lbl, 216);
    lv_label_set_long_mode(s_text_lbl, LV_LABEL_LONG_WRAP);
}

static void build_actions_view(void)
{
    s_actions_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_actions_view, 220, 240);
    lv_obj_center(s_actions_view);
    lv_obj_set_style_bg_color(s_actions_view, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_color(s_actions_view, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_border_width(s_actions_view, 1, 0);
    lv_obj_set_style_radius(s_actions_view, 10, 0);
    lv_obj_set_style_pad_all(s_actions_view, 10, 0);
    lv_obj_clear_flag(s_actions_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_actions_view, LV_OBJ_FLAG_HIDDEN);

    s_actions_title = lv_label_create(s_actions_view);
    lv_label_set_text(s_actions_title, "");
    lv_obj_set_style_text_color(s_actions_title, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_actions_title, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_actions_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_actions_title, 200);
    lv_obj_align(s_actions_title, LV_ALIGN_TOP_MID, 0, 0);

    /* "Open / View" is unimplemented (cb=NULL) and skipped, so pack the real
       actions right under the title — at y=158 Delete used to collide with the
       bottom Cancel button. */
    struct { const char *t; uint32_t col; lv_event_cb_t cb; int y; } items[] = {
        { "Open / View",  0x14213d, NULL,              0   },
        { LV_SYMBOL_COPY "  Copy",      0x457b9d, on_action_copy,    36 },
        { "Rename",                     0x4a5060, on_action_rename,  78 },
        { LV_SYMBOL_TRASH "  Delete",   0xe63946, on_action_delete,  120 },
    };
    for (int i = 0; i < 4; i++) {
        if (!items[i].cb) continue;       /* skip non-implemented */
        lv_obj_t *b = lv_button_create(s_actions_view);
        lv_obj_set_size(b, 200, 36);
        lv_obj_set_pos(b, 0, items[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(items[i].col), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_add_event_cb(b, items[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, items[i].t);
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
        lv_obj_center(l);
    }

    lv_obj_t *close = lv_button_create(s_actions_view);
    lv_obj_set_size(close, 200, 30);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(close, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(close, on_actions_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(cl);
}

static void build_rename_view(void)
{
    s_rename_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_rename_view, 240, 432);
    lv_obj_set_style_radius(s_rename_view, 0, 0);
    lv_obj_set_pos(s_rename_view, 0, 0);
    lv_obj_set_style_bg_color(s_rename_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_rename_view, 0, 0);
    lv_obj_set_style_pad_all(s_rename_view, 0, 0);
    lv_obj_clear_flag(s_rename_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_rename_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_rename_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_rename_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(s_rename_view);
    lv_label_set_text(title, "Rename");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *save = lv_button_create(s_rename_view);
    lv_obj_set_size(save, 56, 32);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(save, on_rename_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    s_rename_ta = lv_textarea_create(s_rename_view);
    lv_obj_set_size(s_rename_ta, 240, 152);
    lv_obj_align(s_rename_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(s_rename_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_rename_ta, lv_color_hex(hb_color_text()), 0);
    lv_textarea_set_one_line(s_rename_ta, true);

    lv_obj_t *kb_box = lv_obj_create(s_rename_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_rename_ta);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_list_view();
    build_text_view();
    build_actions_view();
    build_rename_view();

    scan();
    show_list();
    hb_lv_set_frame_cb(on_frame);
}

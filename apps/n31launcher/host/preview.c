/*
 * preview.c - render every launcher screen headlessly.
 *
 * Same reason as everywhere else: laying out a 240 x 432 screen by imagining it
 * does not work, and a row that collides with the one below it is obvious in a
 * picture and invisible in source.
 *
 * The app list is filled in here rather than scanned, because this machine has
 * no n31os volume - and because the states worth looking at are the ones that
 * are awkward to arrange on the device: more apps than fit, a name too long for
 * its row, a volume that is not mounted, and one that is mounted and empty.
 */

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "../ui.h"
#include "../apps.h"

static uint32_t s_fb[N31_SCREEN_W * N31_SCREEN_H];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    (void)a; (void)p;
    lv_display_flush_ready(d);
}

static bool write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    const uint32_t data = (uint32_t)(w * h * 4);
    const uint32_t off = 54;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t total = off + data;
    memcpy(hdr + 2, &total, 4);
    memcpy(hdr + 10, &off, 4);
    uint32_t ihdr = 40;
    memcpy(hdr + 14, &ihdr, 4);
    int32_t ww = w, hh = h;
    memcpy(hdr + 18, &ww, 4);
    memcpy(hdr + 22, &hh, 4);
    uint16_t planes = 1, bpp = 32;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data, 4);

    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    for (int y = h - 1; y >= 0 && ok; y--)
        ok = fwrite(px + (size_t)y * w, 4, (size_t)w, f) == (size_t)w;
    fclose(f);
    return ok;
}

static const char *s_out = "shots";
static int s_n;

static void shot(const char *name)
{
    for (int i = 0; i < 8; i++) { lv_refr_now(NULL); lv_timer_handler(); }

    char path[512];
    snprintf(path, sizeof path, "%s/%d-%s.bmp", s_out, ++s_n, name);
    printf("  %s\n", write_bmp(path, s_fb, N31_SCREEN_W, N31_SCREEN_H)
                     ? path : "FAILED");
}

static void add(const char *name, const char *tagline, const char *glyph,
                uint32_t accent)
{
    n31_app_t *a = &n31_apps[n31_app_count++];
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->tagline, sizeof a->tagline, "%s", tagline);
    snprintf(a->glyph, sizeof a->glyph, "%s", glyph);
    a->accent = accent;
}

int main(int argc, char **argv)
{
    if (argc > 1) s_out = argv[1];

    lv_init();
    lv_display_t *d = lv_display_create(N31_SCREEN_W, N31_SCREEN_H);
    lv_display_set_buffers(d, s_fb, NULL, sizeof s_fb,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);

    n31_ui_init();

    /* The two factory apps always exist; everything after them is what the
       extras list shows. */
    add("Radio+",  "FM, RDS, recording", "FM", 0x22D3EE);
    add("TinyPod", "Music",              "TP", 0x34D399);
    n31_extra_first = n31_app_count;

    /* Nothing on the volume, which is the state of the device today. */
    n31_ui_home();
    shot("home-no-extras");

    n31_ui_extras(n31_extra_first, false);
    shot("modal-no-disk");

    n31_ui_extras(n31_extra_first, true);
    shot("modal-nothing-found");

    n31_ui_mounting(-1, "rebuilding map", 41);
    shot("mounting-indeterminate");

    n31_ui_mounting(62, "reading filesystem", 73);
    shot("mounting");

    n31_ui_mount_failed("no disk - the FTL did not bind");
    shot("mount-failed");

    n31_ui_starting("Radio+", 0x22D3EE);
    shot("starting");

    /* A volume's worth, including one name that cannot fit - to prove it
       elides rather than wrapping onto the row below - and one with nothing to
       say about itself. */
    add("Doom",    "Shareware episode",  "DM", 0xF43F5E);
    add("Entrain", "Binaural tones",     "EN", 0xA78BFA);
    add("Screenshot Utility", "on disk", "SU", 0xFBBF24);
    add("Minesweeper", "on disk",        "MI", 0x38BDF8);
    add("Level",   "",                   "LE", 0x4ADE80);
    add("Paint",   "on disk",            "PA", 0xF472B6);

    n31_ui_home();
    shot("home");

    /* Radio+ and TinyPod must NOT appear here - they have a button each on the
       home screen already. */
    n31_ui_extras(n31_extra_first, true);
    shot("extras");

    n31_ui_extras((int)n31_app_count - 1, true);
    shot("extras-end");

    n31_ui_extras((int)n31_extra_first + 2, true);
    shot("extras-middle");
    return 0;
}

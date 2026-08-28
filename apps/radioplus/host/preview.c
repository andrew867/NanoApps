/*
 * preview.c — render every screen headlessly and write it out as a BMP.
 *
 * No display, no SDL, no window server. LVGL draws into a plain memory buffer
 * and the buffer is written to disk, which means the whole interface can be
 * looked at from a build machine and, more importantly, that every screen can
 * be looked at *at once* rather than one at a time on hardware.
 *
 * That is the point of it. Laying out 240 x 432 by imagining it does not work.
 * A label that overflows its box, a row that collides with the one below it, a
 * hairline one pixel off - all of them are obvious in a picture and invisible
 * in source.
 *
 * BMP because it is twenty lines and no libraries; host/tobmp.py turns the
 * output into PNG with Pillow, which is already a dependency of ./start.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "../ui.h"
#include "../model.h"

static uint32_t s_fb[RP_SCREEN_W * RP_SCREEN_H];

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    (void)area; (void)px;
    lv_display_flush_ready(d);
}

/* A 32-bit BMP, bottom-up, which is what an uncompressed BMP wants. */
static bool write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    const uint32_t data = (uint32_t)(w * h * 4);
    const uint32_t off = 14 + 40;
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

    /* Bottom-up: BMP stores the last row first. */
    for (int y = h - 1; y >= 0 && ok; y--)
        ok = fwrite(px + (size_t)y * w, 4, (size_t)w, f) == (size_t)w;

    fclose(f);
    return ok;
}

/* Let LVGL settle. One pass is not enough - layout, then draw, and anything
   that resizes to its content needs the round trip. */
static void settle(void)
{
    for (int i = 0; i < 8; i++) {
        lv_refr_now(NULL);
        lv_timer_handler();
    }
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "shots";

    lv_init();

    lv_display_t *d = lv_display_create(RP_SCREEN_W, RP_SCREEN_H);
    lv_display_set_buffers(d, s_fb, NULL, sizeof s_fb,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);

    rp_model_refresh();
    rp_ui_init();

    static const struct { rp_screen_t s; const char *name; } shots[] = {
        { RP_SCREEN_NOW,      "1-now" },
        { RP_SCREEN_DIAL,     "2-dial" },
        { RP_SCREEN_PRESETS,  "3-presets" },
        { RP_SCREEN_LIBRARY,  "4-recordings" },
        { RP_SCREEN_SETTINGS, "5-settings" },
        { RP_SCREEN_ADVANCED, "6-advanced" },
    };

    char path[512];
    int made = 0;

    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        rp_ui_show(shots[i].s);
        rp_ui_tick();
        settle();

        snprintf(path, sizeof path, "%s/%s.bmp", out, shots[i].name);
        if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
            printf("  %s\n", path);
            made++;
        } else {
            printf("  FAILED %s\n", path);
        }
    }

    /* Three registers with different field shapes, because the editor is
       generated from the table and the shapes are what it has to get right:
       0x05 packs a range field above a bitmap, 0xFA is an enum, and 0xF9 is
       eight sliders of mixed sign. */
    static const struct { uint8_t addr; const char *name; } regs[] = {
        { 0x05, "8-reg-audio" },
        { 0xFA, "9-reg-antenna" },
        { 0xF9, "10-reg-blend" },
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        rp_ui_open_register(regs[i].addr);
        settle();
        snprintf(path, sizeof path, "%s/%s.bmp", out, regs[i].name);
        if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
            printf("  %s\n", path);
            made++;
        }
    }

    /* One more of Now Playing mid-recording, because the recording state
       changes several things at once and is worth seeing as a whole. */
    /* At the live edge, so the transport is in its record state rather than
       its playback one - the two are the same three buttons. */
    rp_act_play_live();
    rp_act_record_toggle();
    rp_model_refresh();
    rp_ui_show(RP_SCREEN_NOW);
    rp_ui_tick();
    settle();
    snprintf(path, sizeof path, "%s/7-now-recording.bmp", out);
    if (write_bmp(path, s_fb, RP_SCREEN_W, RP_SCREEN_H)) {
        printf("  %s\n", path);
        made++;
    }

    printf("%d screens\n", made);
    return made ? 0 : 1;
}

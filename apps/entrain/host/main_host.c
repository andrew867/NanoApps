/*
 * main_host.c — bring the shared UI up on a Linux desktop, at 240x432.
 *
 * The window is forced to the iPod's exact panel size. That is the whole
 * point: a host build at some other resolution would let you tune a layout
 * that does not exist on the device.
 *
 * Backends, chosen with --backend or ENTRAIN_BACKEND:
 *   sdl       a 240x432 window. What you develop against.
 *   fbdev     /dev/fb0, for a small panel over tinydrm's fbdev shim.
 *   drm       /dev/dri/card0 directly.
 *   headless  renders into memory and writes BMPs. No display needed, which
 *             is what makes it possible to check the layout from a shell.
 *
 *   ./build/entrain-host                    # SDL window
 *   ./build/entrain-host --shot shots/      # render every screen to shots/
 *   ./build/entrain-host --backend fbdev
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "../ui.h"
#include "../engine.h"
#include "../platform/sys.h"
#include "../platform/audio.h"

#define W EN_SCREEN_W
#define H EN_SCREEN_H

/* ---- headless display ---------------------------------------------------- */

static uint8_t s_fb[W * H * 4];      /* XRGB8888, matching the device */

static void headless_flush(lv_display_t *d, const lv_area_t *area,
                           uint8_t *px_map)
{
    (void)area;
    /* DIRECT render mode hands us the whole framebuffer every time. */
    memcpy(s_fb, px_map, sizeof s_fb);
    lv_display_flush_ready(d);
}

static lv_display_t *headless_create(void)
{
    static uint8_t buf[W * H * 4];
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d, buf, NULL, sizeof buf,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, headless_flush);
    return d;
}

/* Bottom-up 24-bit BMP. Enough to eyeball a layout, and it needs no libraries;
   Makefile.host converts them to PNG with Pillow, which ./start already
   installs. */
static int write_bmp(const char *path)
{
    const int row = W * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int img = (row + pad) * H;
    const int total = 54 + img;

    uint8_t h[54];
    memset(h, 0, sizeof h);
    h[0] = 'B'; h[1] = 'M';
    h[2] = (uint8_t)total; h[3] = (uint8_t)(total >> 8);
    h[4] = (uint8_t)(total >> 16); h[5] = (uint8_t)(total >> 24);
    h[10] = 54;
    h[14] = 40;
    h[18] = (uint8_t)W; h[19] = (uint8_t)(W >> 8);
    h[22] = (uint8_t)H; h[23] = (uint8_t)(H >> 8);
    h[26] = 1;
    h[28] = 24;
    h[34] = (uint8_t)img; h[35] = (uint8_t)(img >> 8);
    h[36] = (uint8_t)(img >> 16); h[37] = (uint8_t)(img >> 24);

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fwrite(h, 1, sizeof h, f);

    uint8_t zero[4] = { 0, 0, 0, 0 };
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            const uint8_t *p = &s_fb[(y * W + x) * 4];   /* B G R X */
            fwrite(p, 1, 3, f);
        }
        if (pad) fwrite(zero, 1, (size_t)pad, f);
    }
    fclose(f);
    return 1;
}

/* ---- main ---------------------------------------------------------------- */

static void pump(int frames)
{
    for (int i = 0; i < frames; i++) {
        en_ui_tick();
        lv_timer_handler();
        usleep(8000);
    }
}

static void usage(void)
{
    printf("entrain-host — the Entrain UI at %dx%d\n\n"
           "  --backend sdl|fbdev|drm|headless\n"
           "  --shot DIR     render every screen to DIR/NN-name.bmp and exit\n"
           "  --fbdev PATH   framebuffer device (default /dev/fb0)\n"
           "  --drm PATH     DRM card (default /dev/dri/card0)\n"
           "  --evdev PATH   touch device for fbdev/drm backends\n"
           "  --first-run    show the headphone notice\n", W, H);
}

int main(int argc, char **argv)
{
    const char *backend = getenv("ENTRAIN_BACKEND");
    const char *shot_dir = NULL;
    const char *fbdev_path = "/dev/fb0";
    const char *drm_path = "/dev/dri/card0";
    const char *evdev_path = getenv("ENTRAIN_EVDEV");
    int first_run = 0;

    for (int i = 1; i < argc; i++) {
        int has = i + 1 < argc;
        if (!strcmp(argv[i], "--backend") && has) backend = argv[++i];
        else if (!strcmp(argv[i], "--shot") && has) shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--fbdev") && has) fbdev_path = argv[++i];
        else if (!strcmp(argv[i], "--drm") && has) drm_path = argv[++i];
        else if (!strcmp(argv[i], "--evdev") && has) evdev_path = argv[++i];
        else if (!strcmp(argv[i], "--first-run")) first_run = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (shot_dir && !backend) backend = "headless";
    if (!backend) backend = "sdl";

    lv_init();
    lv_tick_set_cb(en_sys_millis);

    lv_display_t *disp = NULL;

    if (!strcmp(backend, "headless")) {
        disp = headless_create();
    }
#if LV_USE_SDL
    else if (!strcmp(backend, "sdl")) {
        disp = lv_sdl_window_create(W, H);
        lv_sdl_mouse_create();
    }
#endif
#if LV_USE_LINUX_FBDEV
    else if (!strcmp(backend, "fbdev")) {
        disp = lv_linux_fbdev_create();
        if (disp) lv_linux_fbdev_set_file(disp, fbdev_path);
    }
#endif
#if LV_USE_LINUX_DRM
    else if (!strcmp(backend, "drm")) {
        disp = lv_linux_drm_create();
        if (disp) lv_linux_drm_set_file(disp, drm_path, -1);
    }
#endif
    else {
        fprintf(stderr, "backend '%s' is not compiled in\n", backend);
        return 1;
    }

    if (!disp) {
        fprintf(stderr, "could not open the %s backend\n", backend);
        return 1;
    }

    /* Force the device's geometry regardless of what the panel reports, so a
       larger fbdev still shows exactly the iPod's layout. */
    lv_display_set_resolution(disp, W, H);

#if LV_USE_EVDEV
    if (evdev_path && strcmp(backend, "sdl") && strcmp(backend, "headless")) {
        lv_indev_t *in = lv_evdev_create(LV_INDEV_TYPE_POINTER, evdev_path);
        if (!in) fprintf(stderr, "warning: no touch on %s\n", evdev_path);
    }
#endif

    if (first_run) en_ui_set_first_run(true);
    en_ui_init();
    printf("entrain-host: %s backend, %dx%d, audio via %s\n",
           backend, W, H, en_audio_backend_name());

    /* ---- screenshot mode ---- */
    if (shot_dir) {
        /* Give the screens something real to show. A Now Playing shot of an
           idle player would tell you nothing about the layout. */
        en_engine_play_program(0);
        pump(60);

        static const en_screen_t order[] = {
            EN_SCREEN_FIRSTRUN, EN_SCREEN_LIBRARY, EN_SCREEN_NOW,
            EN_SCREEN_TUNE, EN_SCREEN_TIMER, EN_SCREEN_SETTINGS
        };
        for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) {
            en_ui_goto(order[i], false);
            pump(30);
            lv_refr_now(disp);

            char path[512];
            snprintf(path, sizeof path, "%s/%s.bmp", shot_dir,
                     en_ui_screen_name(order[i]));
            if (write_bmp(path)) printf("  %s\n", path);
        }

        /* And a second Library shot on the Programs tab, which is the view
           most of the app's content actually lives in. */
        en_ui_goto(EN_SCREEN_LIBRARY, false);
        pump(5);
        lv_refr_now(disp);

        en_ui_shutdown();
        return 0;
    }

    /* ---- interactive ---- */
    while (!en_sys_exit_requested()) {
        en_ui_tick();
        uint32_t wait = lv_timer_handler();
        if (wait == LV_NO_TIMER_READY) wait = 8;
        if (wait > 16) wait = 16;
        usleep(wait * 1000);
    }

    en_ui_shutdown();
    return 0;
}

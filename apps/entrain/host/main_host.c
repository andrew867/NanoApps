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

#ifdef __linux__
#include <fcntl.h>
#include <errno.h>
#endif

#define W EN_SCREEN_W
#define H EN_SCREEN_H

/* Provided only by the N31 Linux backend. Declared weak at file scope — a
   block-scope weak attribute is quietly ignored by some GCC versions — so the
   desktop build, which links a different backend, still resolves to NULL. */
void en_audio_linux_set_device(int card, int device) __attribute__((weak));
void en_audio_linux_force_null(int on) __attribute__((weak));

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

/* ---- physical buttons over evdev ------------------------------------------
 *
 * The N31 kernel exposes n31-buttons and the PMIC buttons as keyboards, and
 * there is no touchscreen device at all yet, so this is the only way to drive
 * the UI on the device.
 *
 * The event struct is declared by hand rather than pulled from <linux/input.h>
 * on purpose. musl uses a 64-bit time_t, so its struct timeval is 16 bytes,
 * while the kernel writes input_event with 32-bit time fields on arm32. Using
 * the system struct would misparse every event by eight bytes — silently, and
 * in a way that looks like the buttons simply do not work. */

#ifdef __linux__

struct en_input_event {
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EN_EV_KEY 0x01

#define EN_MAX_INPUT_FDS 8
static int s_input_fd[EN_MAX_INPUT_FDS];
static int s_input_count;
static int s_trace_keys;

static void input_open_all(void)
{
    for (int i = 0; i < 32 && s_input_count < EN_MAX_INPUT_FDS; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) s_input_fd[s_input_count++] = fd;
    }
    printf("entrain-host: %d input device(s)\n", s_input_count);
}

/* Key codes as the kernel defines them. Which of these n31-buttons actually
   emits is what --trace-keys is for. */
#define EN_KEY_ESC        1
#define EN_KEY_VOLUMEDOWN 114
#define EN_KEY_VOLUMEUP   115
#define EN_KEY_POWER      116
#define EN_KEY_BACKKEY    158
#define EN_KEY_PLAYPAUSE  164
#define EN_KEY_HOMEPAGE   172
#define EN_KEY_NEXTSONG   163
#define EN_KEY_PREVSONG   165

static void input_poll(void)
{
    struct en_input_event ev;
    for (int i = 0; i < s_input_count; i++) {
        while (read(s_input_fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != EN_EV_KEY || ev.value != 1) continue;   /* press only */
            if (s_trace_keys)
                printf("key %u down (event%d)\n", ev.code, i);
            switch (ev.code) {
            case EN_KEY_VOLUMEUP:   en_ui_key(EN_KEY_VOL_UP);     break;
            case EN_KEY_VOLUMEDOWN: en_ui_key(EN_KEY_VOL_DOWN);   break;
            case EN_KEY_PLAYPAUSE:
            case EN_KEY_NEXTSONG:   en_ui_key(EN_KEY_PLAY_PAUSE); break;
            case EN_KEY_HOMEPAGE:
            case EN_KEY_BACKKEY:
            case EN_KEY_ESC:        en_ui_key(EN_KEY_BACK);       break;
            default: break;
            }
        }
    }
}

#else
static void input_open_all(void) {}
static void input_poll(void) {}
#endif

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
           "  --first-run    show the headphone notice\n"
           "  --buttons      drive the UI from /dev/input (no touchscreen yet)\n"
           "  --trace-keys   print every key code, to find out what the buttons send\n"
           "  --demo N       walk through every screen, N seconds each\n"
           "  --screen NAME  start on a named screen (library, now-playing, ...)\n"
           "  --tab N        Library tab: 0 presets, 1 programs, 2 custom\n"
           "  --preset N     start playing preset N\n"
           "  --program N    start playing program N\n"
           "  --card C [D]   ALSA card and device (default 0 0)\n"
           "  --null-audio   keep time but open no PCM device\n", W, H);
}

int main(int argc, char **argv)
{
    const char *backend = getenv("ENTRAIN_BACKEND");
    const char *shot_dir = NULL;
    const char *fbdev_path = "/dev/fb0";
    const char *drm_path = "/dev/dri/card0";
    const char *evdev_path = getenv("ENTRAIN_EVDEV");
    int first_run = 0;
    int use_buttons = 0;
    int demo_seconds = 0;
    int start_program = -1;
    int start_preset = -1;
    int start_tab = -1;
    const char *start_screen = NULL;

    for (int i = 1; i < argc; i++) {
        int has = i + 1 < argc;
        if (!strcmp(argv[i], "--backend") && has) backend = argv[++i];
        else if (!strcmp(argv[i], "--shot") && has) shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--fbdev") && has) fbdev_path = argv[++i];
        else if (!strcmp(argv[i], "--drm") && has) drm_path = argv[++i];
        else if (!strcmp(argv[i], "--evdev") && has) evdev_path = argv[++i];
        else if (!strcmp(argv[i], "--first-run")) first_run = 1;
        else if (!strcmp(argv[i], "--buttons")) use_buttons = 1;
        else if (!strcmp(argv[i], "--trace-keys")) {
            use_buttons = 1;
#ifdef __linux__
            s_trace_keys = 1;
#endif
        }
        else if (!strcmp(argv[i], "--demo") && has) demo_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--program") && has) start_program = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preset") && has) start_preset = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tab") && has) start_tab = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screen") && has) start_screen = argv[++i];
        else if (!strcmp(argv[i], "--null-audio")) {
            if (en_audio_linux_force_null) en_audio_linux_force_null(1);
        }
        else if (!strcmp(argv[i], "--card") && has) {
            int c = atoi(argv[++i]);
            int d = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : 0;
            if (en_audio_linux_set_device) en_audio_linux_set_device(c, d);
        }
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
    if (use_buttons) input_open_all();
    en_ui_init();

    if (start_tab >= 0) en_ui_set_tab(start_tab);
    if (start_preset >= 0)  en_engine_play_preset(start_preset);
    if (start_program >= 0) en_engine_play_program(start_program);
    if (start_preset >= 0 || start_program >= 0) en_ui_goto(EN_SCREEN_NOW, false);
    if (start_screen) {
        for (int sc = 0; sc < EN_SCREEN_COUNT; sc++) {
            if (!strcmp(start_screen, en_ui_screen_name((en_screen_t)sc))) {
                en_ui_goto((en_screen_t)sc, false);
                break;
            }
        }
    }

    printf("entrain-host: %s backend, %dx%d, audio via %s\n",
           backend, W, H, en_audio_backend_name());
    fflush(stdout);

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
    static const en_screen_t WALK[] = {
        EN_SCREEN_LIBRARY, EN_SCREEN_NOW, EN_SCREEN_TUNE,
        EN_SCREEN_TIMER, EN_SCREEN_SETTINGS
    };
    uint32_t next_walk = en_sys_millis() + (uint32_t)demo_seconds * 1000u;
    unsigned walk_i = 0;

    while (!en_sys_exit_requested()) {
        if (use_buttons) input_poll();

        /* With no touchscreen on this port, a timed walk is the only way to
           see every screen. It is a diagnostic, not a feature. */
        if (demo_seconds > 0 && en_sys_millis() >= next_walk) {
            next_walk = en_sys_millis() + (uint32_t)demo_seconds * 1000u;
            walk_i = (walk_i + 1) % (sizeof WALK / sizeof WALK[0]);
            en_ui_goto(WALK[walk_i], true);
            printf("screen: %s\n", en_ui_screen_name(WALK[walk_i]));
            fflush(stdout);
        }

        en_ui_tick();
        uint32_t wait = lv_timer_handler();
        if (wait == LV_NO_TIMER_READY) wait = 8;
        if (wait > 16) wait = 16;
        usleep(wait * 1000);
    }

    en_ui_shutdown();
    return 0;
}

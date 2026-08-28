/*
 * main_n31.c — Radio+ on the N31 Linux port.
 *
 * Framebuffer out, evdev in, and a loop. Everything interesting is elsewhere:
 * the driver owns the tuner, a thread owns the capture, and the screens read a
 * model that this file never touches.
 *
 * Two things it does own, because nothing else can:
 *
 *   Shutdown. The capture thread holds a PCM device and the tuner holds power,
 *   and neither is released by exiting. A radio left drawing current after the
 *   app is gone is a real fault on a battery device, so the signal handlers
 *   are wired before anything is opened.
 *
 *   Pacing. LVGL is asked to run at a steady rate rather than as fast as it
 *   can. There is nothing on these screens that moves faster than a few times
 *   a second, and spinning would cost battery to redraw a frequency that has
 *   not changed.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "../ui.h"
#include "../model.h"
#include "../platform/tuner.h"
#include "../platform/capture.h"

#define FRAME_MS 33          /* about 30 Hz, which is more than enough */

static volatile sig_atomic_t s_quit;

static uint32_t millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000));
}

static void on_signal(int sig)
{
    (void)sig;
    s_quit = 1;
}

/* The touch panel is not always the same event node, so it is found by asking
   rather than by hard-coding a number that a driver load order can change. */
static const char *find_touch(void)
{
    static char path[64];
    for (int i = 0; i < 12; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[128] = { 0 };
        /* EVIOCGNAME(sizeof name) without dragging in linux/input.h, which
           conflicts with some libc headers on this toolchain. */
        if (ioctl(fd, (int)(0x80000000u | ((sizeof name) << 16) | ('E' << 8) | 0x06),
                  name) >= 0) {
            close(fd);
            for (char *p = name; *p; p++)
                if ((p[0] == 't' || p[0] == 'T') &&
                    (p[1] == 'o' || p[1] == 'O') &&
                    (p[2] == 'u' || p[2] == 'U'))
                    return path;
            continue;
        }
        close(fd);
    }
    return NULL;
}

static void usage(const char *me)
{
    printf("usage: %s [--fb /dev/fb0] [--input /dev/input/eventN]\n"
           "\n"
           "  RADIOPLUS_HOME  where presets and recordings live\n"
           "                  (default /tmp/radioplus)\n"
           "\n"
           "FM rides on hci0, so bring Bluetooth up first with n31-bt-up.\n",
           me);
}

int main(int argc, char **argv)
{
    const char *fb = "/dev/fb0";
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        int has = i + 1 < argc;
        if (!strcmp(argv[i], "--fb") && has) fb = argv[++i];
        else if (!strcmp(argv[i], "--input") && has) input = argv[++i];
        else { usage(argv[0]); return 0; }
    }

    /* Wired before anything is opened, so an interrupt during start-up still
       releases whatever had been taken by then. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    lv_init();
    lv_tick_set_cb(millis);

    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) {
        fprintf(stderr, "radioplus: no framebuffer display\n");
        return 1;
    }
    lv_linux_fbdev_set_file(disp, fb);
    lv_display_set_resolution(disp, RP_SCREEN_W, RP_SCREEN_H);

#if LV_USE_EVDEV
    if (!input) input = find_touch();
    if (input) {
        lv_indev_t *in = lv_evdev_create(LV_INDEV_TYPE_POINTER, input);
        if (in) {
            lv_indev_set_display(in, disp);
            printf("radioplus: touch on %s\n", input);
        }
    } else {
        printf("radioplus: no touch device found; display only\n");
    }
#endif

    /* Brings the tuner and the capture up as a side effect of the first
       refresh, so the screens have something to draw before the first frame. */
    rp_model_refresh();
    rp_ui_init();

    printf("radioplus: %s\n", rp_model.backend ? rp_model.backend : "?");
    printf("radioplus: %s\n",
           rp_model.capture_backend ? rp_model.capture_backend : "?");
    if (!rp_model.tuner_ok && rp_model.tuner_note)
        printf("radioplus: %s\n", rp_model.tuner_note);

    while (!s_quit) {
        rp_model_refresh();
        rp_ui_tick();

        uint32_t wait = lv_timer_handler();
        if (wait > FRAME_MS) wait = FRAME_MS;   /* also covers no-timer */
        usleep(wait * 1000u);
    }

    /* The whole reason the signal handlers exist. A capture thread still
       holding a PCM device, or a tuner still drawing power, outlives the
       process otherwise. */
    printf("\nradioplus: stopping\n");
    en_cap_record_stop();
    en_cap_stop();
    en_tuner_shutdown();
    return 0;
}

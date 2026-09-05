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
#include "../platform/player.h"

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

/*
 * Buttons.
 *
 * There is no touchscreen on this device - /proc/bus/input/devices lists
 * n31-buttons, an accelerometer and n31-pmic-buttons and nothing else - so an
 * interface that can only be tapped cannot be driven at all.
 *
 * Read straight from evdev rather than through an LVGL keypad group: the
 * screens are positioned rather than laid out in a focus order, and inventing
 * one purely so a generic focus mechanism has something to walk would be more
 * machinery than the four actions actually wanted.
 */
#define KEY_VOLUMEDOWN   114
#define KEY_VOLUMEUP     115
#define KEY_MENU         139
#define KEY_BACK         158
#define KEY_NEXTSONG     163
#define KEY_PLAYPAUSE    164
#define KEY_PREVIOUSSONG 165

#define MAX_KEY_FDS 4
static int s_key_fd[MAX_KEY_FDS];
static int s_key_fds;

/* struct input_event, without pulling in linux/input.h - it collides with some
   libc headers on this toolchain. Layout is time, type, code, value. */
struct rp_input_event {
    long     sec;
    long     usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

static void open_keys(void)
{
    for (int i = 0; i < 12 && s_key_fds < MAX_KEY_FDS; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        /* Ask which EVENT TYPES the device reports, not which key codes.
           EVIOCGBIT(EV_KEY) returns the key bitmap, and reading only its first
           word tests keys 0 to 31 - which these devices do not have. Their keys
           are volume and transport, at 114 and above, so a device full of
           exactly the keys wanted looked like a device with none. */
        unsigned long types = 0;
        int req = (int)(0x80000000u | ((sizeof types) << 16) | ('E' << 8)
                        | 0x20);              /* EVIOCGBIT(0, ...) */
        if (ioctl(fd, req, &types) >= 0 && (types & (1u << 1))) {   /* EV_KEY */
            s_key_fd[s_key_fds++] = fd;
            printf("radioplus: keys on %s\n", path);
            continue;
        }
        close(fd);
    }
    if (!s_key_fds) printf("radioplus: no key input found\n");
}

/* One step along the swipe sequence from wherever we are, wrapping, so one
   button reaches every screen rather than stopping at the end. A screen that
   is not in the sequence - the register editors - starts from the beginning. */
static int swipe_step(int delta)
{
    const int n = rp_ui_swipe_count();
    if (n <= 0) return 0;

    int at = 0;
    for (int i = 0; i < n; i++) {
        if (rp_ui_swipe_at(i) == rp_ui_current()) { at = i; break; }
    }
    at = (at + delta) % n;
    if (at < 0) at += n;
    return at;
}

static void pump_keys(void)
{
    struct rp_input_event ev;

    for (int i = 0; i < s_key_fds; i++) {
        while (read(s_key_fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != 1 || ev.value != 1) continue;   /* presses only */

            switch (ev.code) {
            /*
             * Step through the swipe sequence, not through the enum.
             *
             * These used to add one to rp_ui_current() and wrap at a fixed
             * count. That stopped being the same thing when two screens became
             * optional: the sequence is built at run time now, so walking the
             * enum would land on a screen that is turned off and skip the
             * dots' idea of where you are.
             */
            case KEY_VOLUMEUP:
            case KEY_NEXTSONG:
                rp_ui_show(rp_ui_swipe_at(swipe_step(+1)));
                break;
            case KEY_VOLUMEDOWN:
            case KEY_PREVIOUSSONG:
                rp_ui_show(rp_ui_swipe_at(swipe_step(-1)));
                break;
            case KEY_PLAYPAUSE:
                /* Exactly what the middle transport button does, so the key and
                   the screen never disagree about what it means. */
                if (rp_model.play_file || rp_model.behind_ms)
                    rp_act_pause_toggle();
                else
                    rp_act_record_toggle();
                break;
            case KEY_MENU:
            case KEY_BACK:
                rp_ui_show(RP_SCREEN_NOW);
                break;
            default:
                break;
            }
        }
    }
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
        printf("radioplus: no touch panel; buttons only\n");
    }
#endif

    open_keys();

    /*
     * Something on the screen before anything else.
     *
     * Until this was here the panel kept whatever the launcher had left on it
     * until the first real screen existed, and a device that looks like it did
     * not start is indistinguishable from one that did not.
     *
     * The first rp_model_refresh is quick now - it reads the settings and the
     * presets, then asks the hardware once and returns whatever it finds. The
     * waiting, if there is any, happens further down where the buttons still
     * work.
     */
    rp_ui_boot("starting");

    static const rp_progress_t prog = { rp_ui_boot, rp_ui_boot_failed };
    rp_model_set_progress(&prog);

    rp_model_refresh();

    /*
     * The interface is built before the wait, not after it.
     *
     * Nothing in rp_ui_init needs hardware - it draws presets and recordings,
     * both of which came off the filesystem during bring-up - so there is no
     * reason to make the listener wait for a radio before they can have the
     * screens. Building first means the buttons do something, every screen is
     * ready the instant a driver turns up, and the boot screen becomes a cover
     * over a working interface rather than a substitute for one.
     */
    rp_ui_init();

    /*
     * Then wait on top of it for whatever has not arrived.
     *
     * The drivers this needs are loaded by scripts running alongside this
     * process - the sound modules a few seconds into boot, hci0 raised by
     * another - so which of us gets there first is a race, and losing it used
     * to be permanent: one attempt, at the wrong instant, and "no audio
     * hardware" and "no Bluetooth" latched into a model that never asked
     * again.
     *
     * The platform says what it is still waiting for and decides when to give
     * up saying it. Three things end this loop: everything arrives, the
     * platform's deadline passes, or the listener presses something and goes
     * to a screen of their own - which is why pump_keys is in here. A wait
     * that ignores the buttons is a device that has hung, and this one can
     * legitimately last twenty seconds.
     *
     * Whatever ends it, the retry inside rp_model_refresh keeps going, so a
     * tuner that appears a minute from now still works.
     */
    if (rp_model_waiting()) rp_ui_boot_show();

    for (const char *what;
         !s_quit && rp_ui_booting() && (what = rp_model_waiting()) != NULL; ) {
        pump_keys();
        rp_ui_boot(what);
        usleep(120000);
        rp_model_refresh();
        rp_ui_tick();
    }

    /* Nothing more to report; the real screens say the rest themselves. */
    rp_model_set_progress(NULL);
    if (rp_ui_booting()) rp_ui_show(RP_SCREEN_NOW);

    printf("radioplus: %s\n", rp_model.backend ? rp_model.backend : "?");
    printf("radioplus: %s\n",
           rp_model.capture_backend ? rp_model.capture_backend : "?");
    if (!rp_model.tuner_ok && rp_model.tuner_note)
        printf("radioplus: %s\n", rp_model.tuner_note);

    while (!s_quit) {
        pump_keys();
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
    en_play_stop();
    en_cap_stop();
    en_tuner_shutdown();
    return 0;
}

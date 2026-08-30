/*
 * launcher.c — the N31 launcher.
 *
 * Three buttons, and what they mean depends only on which screen you are on:
 * PLAY goes in, HOME comes back, volume moves. On the home screen volume opens
 * the two apps that are always installed, because there is nothing to scroll
 * through there and a fixed screen can be used by feel.
 *
 * Most of the care goes into four things that are easy to get wrong.
 *
 * Closing an app properly. HOME sends SIGTERM, not SIGKILL, because these apps
 * have state worth keeping - Radio+ writes its settings and presets on the way
 * out, and a recording that is still open needs its WAV length patched or the
 * file is unplayable. SIGKILL is only the fallback, several seconds later, for
 * something that has genuinely stopped listening. Killing first and asking later
 * would quietly corrupt exactly the things a user would miss.
 *
 * Sharing the framebuffer. Both the launcher and the app draw to /dev/fb0, and
 * there is no compositor to arbitrate. So the launcher stops drawing entirely
 * while a child is alive - it keeps running and keeps reading buttons, but
 * touches nothing - and marks the whole screen dirty when the child exits,
 * because LVGL still believes the display holds what it last drew.
 *
 * Both the launcher and the running app see the same evdev stream; there is no
 * grab. That is deliberate. Grabbing input would make HOME invisible to the
 * launcher, which is the one key it must never miss.
 *
 * Handing the screen over. Not every app draws the same way: Radio+ paints the
 * framebuffer itself and wants the console kept off it, while TinyPod is a
 * terminal program and needs the console put back or it draws into something
 * nobody is displaying. The manifest says which, because nothing about the
 * binary does. Either way a splash goes up first - radioplus-start spends about
 * ten seconds on Bluetooth diagnostics before it shows anything, and without a
 * splash the launcher's last frame just sits there looking frozen.
 *
 * Mounting the volume takes a while, so it is run as a separate script that
 * reports its own progress rather than blocking the loop. The launcher keeps
 * drawing and keeps listening for HOME throughout, because a progress bar you
 * cannot escape from is a hang with extra steps.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "ui.h"
#include "apps.h"
#include "fbcon.h"
#include "status.h"
#include "scanner.h"
#include "klog.h"

#define FRAME_MS 33

/* How long a child gets to save and exit after SIGTERM before it is killed.
   Generous on purpose: the thing it is doing with those seconds is writing the
   user's settings. */
#define TERM_GRACE_MS 4000

/*
 * PLAY both opens an app and, one screen earlier, opened the screen it is on.
 * Distinguishing those by state alone does not work, because the state changes
 * the instant the key is handled - so a key still held, or bounced, arrives on
 * the new screen and acts again.
 */
#define SCREEN_GUARD_MS 350

/* The volume is not always mounted when the launcher starts, so the list is
   rebuilt occasionally and an app that appears simply appears. This is a
   readdir of one directory, not a disk scan. */
#define RESCAN_MS 2000

/* Battery, Bluetooth, playback and the tuner. A second is plenty - none of
   them change faster than that in any way worth drawing. */
#define STATUS_MS 1000

/* How often to check the console has not taken the screen back. One small
   sysfs read; the cost of missing it is every kernel message drawing over the
   UI for the rest of the session. */
#define FBCON_MS 2000

/* How often to ask whether another process has the framebuffer. Slower than
   the console check because it walks /proc, and an app appearing half a second
   late to the launcher's notice costs nothing. */
#define FOREIGN_MS 500

/*
 * How long to wait before trying the volume again after a failure, and how far
 * to back off. The helper escalates internally - load, recover, BPB fallback,
 * reload the stack - so each retry is a different attempt rather than the same
 * one repeated, and it is worth doing more than once.
 *
 * It backs off because the failure that is not going to fix itself (no NAND at
 * all) should not sit in a retry loop for the life of the session.
 */

/* Tilt, which has to keep up with the hand holding the device. One short read
   and three small moves, so this is cheap enough to do properly. */
#define TILT_MS 80


/* Two screens. There were two more - a mount progress bar and a mount failure
   - back when the launcher brought the volume up itself. It has not done that
   for a while, and leaving the names here suggested to anyone reading that it
   still might. */
typedef enum {
    SCREEN_HOME,
    SCREEN_EXTRAS,
} screen_t;

static volatile sig_atomic_t s_quit;

static screen_t s_screen;
static int      s_selected;
static uint32_t s_screen_at;      /* when the current screen was entered */

/*
 * A child that has been asked to stop but has not gone yet. Kept here rather
 * than waited for in place: the wait is up to four seconds and the loop that
 * would be doing the waiting is the loop that draws.
 */
static pid_t    s_dying;
static uint32_t s_dying_since;

static pid_t s_child;             /* a running app */
static bool  s_child_console;     /* it draws through the console, not fb0 */
static screen_t s_child_from;     /* the screen to come back to */
static uint32_t s_launched_at;

static n31_status_t s_status;



static uint32_t millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000));
}

static void on_signal(int sig) { (void)sig; s_quit = 1; }

/* ---- input ---------------------------------------------------------------- */

#define MAX_KEY_FDS 4
static int s_key_fd[MAX_KEY_FDS];
static int s_key_fds;

struct n31_input_event {
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

        /* Ask which event TYPES the device reports, not which key codes. The
           key bitmap's first word covers codes 0 to 31, and every key this
           launcher wants is above 100 - so testing it would reject exactly the
           devices it needs. */
        unsigned long types = 0;
        int req = (int)(0x80000000u | ((sizeof types) << 16) | ('E' << 8)
                        | 0x20);                    /* EVIOCGBIT(0, ...) */
        if (ioctl(fd, req, &types) >= 0 && (types & (1u << 1))) {  /* EV_KEY */
            s_key_fd[s_key_fds++] = fd;
            printf("n31launcher: keys on %s\n", path);
            continue;
        }
        close(fd);
    }
}

/* ---- screens -------------------------------------------------------------- */

/*
 * Is the internal volume up? Asked of /proc/mounts rather than by stat-ing a
 * path, because the mount point is not settled - any vfat under /mnt counts,
 * which is the same assumption the app scan makes.
 */
static bool disk_mounted(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return false;

    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof line, f))
        found = strstr(line, " /mnt/") && strstr(line, " vfat ");

    fclose(f);
    return found;
}

static void go(screen_t s)
{
    s_screen = s;
    s_screen_at = millis();

    switch (s) {
    case SCREEN_HOME:   n31_ui_home(); break;
    case SCREEN_EXTRAS: n31_ui_extras(s_selected, disk_mounted()); break;
    }
}

/* A key that arrives within a moment of the screen changing is the one that
   changed it, still held or bounced. */
static bool settling(void)
{
    return millis() - s_screen_at < SCREEN_GUARD_MS;
}

/* ---- children ------------------------------------------------------------- */

/*
 * Start an app.
 *
 * Through n31-autostart when it is there, because it already resolves a program
 * name against the same locations this does and adds the per-app setup the app
 * table cannot know about - the wad path for fbdoom, and a writable home for
 * radioplus, since the volume it was found on is mounted read-only. Only if it
 * is missing do we fall back to the path discovery already resolved.
 */
static bool launch(const n31_app_t *a)
{
    if (!a || s_child) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        /* Its own process group, so a signal aimed at the app reaches the whole
           of it - n31-autostart is a shell script that execs, but an app that
           spawns helpers would otherwise leave them behind. */
        setpgid(0, 0);

        execl("/bin/n31-autostart", "n31-autostart", a->prog, (char *)0);
        if (a->path[0])
            execl(a->path, a->prog, (char *)0);
        _exit(127);
    }

    setpgid(pid, pid);            /* also here: whichever wins, it is set */
    s_child = pid;
    s_launched_at = millis();
    return true;
}

/*
 * Ask a child to stop. Returns immediately - reaping it, and killing it if it
 * will not go, is reap_dying()'s job on subsequent passes of the main loop.
 *
 * This used to wait here for up to four seconds, which meant HOME froze the
 * launcher for that long with nothing on screen explaining the pause.
 */
static void ask_child_to_stop(pid_t pid)
{
    kill(-pid, SIGTERM);
    s_dying = pid;
    s_dying_since = millis();
}

/* Advance a pending stop. Called every pass; does nothing when none is
   pending. Returns true when the child has finally gone. */
static bool reap_dying(void)
{
    if (!s_dying) return false;

    pid_t r = waitpid(s_dying, NULL, WNOHANG);
    if (r == s_dying || (r < 0 && errno == ECHILD)) {
        s_dying = 0;
        return true;
    }

    /* It has had its grace and is not listening. */
    if (millis() - s_dying_since >= TERM_GRACE_MS) {
        kill(-s_dying, SIGKILL);
        waitpid(s_dying, NULL, WNOHANG);
        s_dying = 0;
        return true;
    }
    return false;
}

/* Only for the way out, where blocking is fine because nothing is drawing
   afterwards. */
static void stop_child_blocking(pid_t pid)
{
    kill(-pid, SIGTERM);

    uint32_t start = millis();
    while (millis() - start < TERM_GRACE_MS) {
        pid_t r = waitpid(pid, NULL, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return;
        usleep(50000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* Take the screen back from whatever the app was using, and go back to where
   it was started from. */
static void after_app(void)
{
    if (s_child_console) {
        n31_fbcon_reclaim();
        s_child_console = false;
    }
    n31_ui_status("");
    n31_ui_extras_opening(false);
    go(s_child_from);
    n31_ui_redraw();
}

static void close_app(void)
{
    if (!s_child) return;

    printf("n31launcher: closing pid %d\n", (int)s_child);
    fflush(stdout);

    ask_child_to_stop(s_child);
    s_child = 0;

    /*
     * Say so now. The app may take a few seconds to save and go, and until
     * this the screen just held its last frame - so HOME looked like it had
     * done nothing at all.
     */
    n31_ui_starting("Closing", 0x8B92A0);
    lv_refr_now(NULL);
}

/* Has the app exited on its own? fbdoom quitting from its own menu should bring
   the launcher back exactly as HOME would. */
static bool app_gone(void)
{
    if (!s_child) return false;

    pid_t r = waitpid(s_child, NULL, WNOHANG);
    if (r != s_child && !(r < 0 && errno == ECHILD)) return false;

    s_child = 0;
    after_app();
    return true;
}

/* ---- the list ------------------------------------------------------------- */

/*
 * Rebuild the list, keeping the selection on the same app rather than the same
 * row. An app appearing above the selection would otherwise slide something
 * else under the cursor, and the next PLAY would open the wrong thing.
 */
/*
 * Take whatever the scanner thread has finished, keeping the selection on the
 * same app rather than the same row. An app appearing above the selection
 * would otherwise slide something else under the cursor, and the next PLAY
 * would open the wrong thing.
 */
static void rescan(void)
{
    char was[32];
    was[0] = 0;
    if (s_selected >= (int)n31_extra_first && s_selected < (int)n31_app_count)
        memcpy(was, n31_apps[s_selected].prog, sizeof was);

    if (!n31_scanner_collect()) return;

    s_selected = (int)n31_extra_first;
    if (was[0]) {
        for (uint8_t i = n31_extra_first; i < n31_app_count; i++)
            if (!strcmp(n31_apps[i].prog, was)) { s_selected = i; break; }
    }
    if (s_selected >= (int)n31_app_count)
        s_selected = (int)n31_app_count - 1;
    if (s_selected < (int)n31_extra_first)
        s_selected = (int)n31_extra_first;

    /* Only redraw the screen that shows any of this. */
    if (s_screen == SCREEN_EXTRAS)   n31_ui_extras(s_selected, disk_mounted());
    else if (s_screen == SCREEN_HOME) n31_ui_home();
}

static void move_selection(int delta)
{
    if (!n31_extra_count) return;

    int n = s_selected + delta;
    if (n < (int)n31_extra_first) n = (int)n31_extra_first;
    if (n >= (int)n31_app_count)  n = (int)n31_app_count - 1;
    if (n == s_selected) return;

    s_selected = n;
    n31_ui_extras(s_selected, disk_mounted());
}

static void open_app(const n31_app_t *a)
{
    if (!a) return;

    const screen_t from = s_screen;

    /* Up before anything else, and drawn synchronously. Some of these take ten
       seconds to put a pixel on the screen, and the launcher stops drawing the
       moment it forks - so this frame is the last thing the user sees until the
       app appears, and it has to say which app. */
    n31_ui_starting(a->name, a->accent);
    lv_refr_now(NULL);

    /* A terminal app draws through the console, so give the console back and
       clear it. Done after the splash, or the splash would be wiped by the
       clear rather than the other way round. */
    if (a->console) {
        n31_fbcon_lend();
        n31_fbcon_clear();
    }

    if (launch(a)) {
        s_child_console = a->console;
        s_child_from = from;
        return;
    }

    if (a->console) n31_fbcon_reclaim();
    n31_ui_status("failed to start");
    n31_ui_extras_opening(false);
    go(from);
}

/* The app behind a home tile, or NULL if it is not installed. The two builtins
   are always first in the list when they resolve at all. */
static const n31_app_t *builtin(const char *prog)
{
    for (uint8_t i = 0; i < n31_app_count; i++)
        if (!strcmp(n31_apps[i].prog, prog)) return &n31_apps[i];
    return 0;
}

/* ---- keys ----------------------------------------------------------------- */

static void on_key(uint16_t code, int32_t value)
{
    const bool pressed = (value == 1);
    const bool repeat  = (value == 2);

    /* While an app is running its own buttons are its business. The launcher
       listens for HOME and nothing else. */
    if (s_child) {
        if (code == N31_KEY_HOMEPAGE && pressed
            && millis() - s_launched_at >= SCREEN_GUARD_MS) {
            close_app();
            n31_ui_redraw();
        }
        return;
    }

    switch (s_screen) {
    case SCREEN_HOME:
        if (!pressed) return;
        if (code == N31_KEY_VOLUMEUP)
            open_app(builtin("radioplus"));
        else if (code == N31_KEY_VOLUMEDOWN)
            open_app(builtin("tinypod"));
        else if (code == N31_KEY_PLAYPAUSE) {
            s_selected = (int)n31_extra_first;
            go(SCREEN_EXTRAS);
        }
        return;

    case SCREEN_EXTRAS:
        /* Auto-repeat counts here and nowhere else: holding volume should run
           down a long list rather than tapping twenty times. */
        if (code == N31_KEY_VOLUMEUP && (pressed || repeat)) {
            move_selection(-1);
            return;
        }
        if (code == N31_KEY_VOLUMEDOWN && (pressed || repeat)) {
            move_selection(+1);
            return;
        }
        if (!pressed || settling()) return;

        if (code == N31_KEY_PLAYPAUSE) {
            /*
             * Nothing to open, and nothing to offer: bringing the volume up is
             * a system service's job now, not this program's. The list follows
             * whatever is mounted, so when the service succeeds the apps appear
             * here on their own.
             */
            if (n31_extra_count)
                open_app(&n31_apps[s_selected]);
        } else if (code == N31_KEY_HOMEPAGE) {
            go(SCREEN_HOME);
        }
        return;

    }
}

static void pump_keys(void)
{
    struct n31_input_event ev;

    for (int i = 0; i < s_key_fds; i++) {
        while (read(s_key_fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
            if (ev.type != 1 || ev.value == 0) continue;   /* type EV_KEY */
            on_key(ev.code, ev.value);
        }
    }
}

/* ---- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *fb = "/dev/fb0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fb") && i + 1 < argc) fb = argv[++i];
        else {
            printf("usage: %s [--fb /dev/fb0]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Children are reaped explicitly, so the default handler is what is
       wanted; ignoring SIGCHLD would make waitpid unable to see them. */
    signal(SIGCHLD, SIG_DFL);

    lv_init();
    lv_tick_set_cb(millis);

    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) {
        fprintf(stderr, "n31launcher: no framebuffer\n");
        return 1;
    }
    lv_linux_fbdev_set_file(disp, fb);
    lv_display_set_resolution(disp, N31_SCREEN_W, N31_SCREEN_H);

    /* Now that we can draw, take the screen. Before this point the console is
       still the only thing that can report a failure, and it keeps that job -
       the framebuffer error above is printed while it is still visible. */
    if (!n31_fbcon_detach())
        printf("n31launcher: framebuffer console still attached - kernel "
               "messages will draw over the UI\n");

    open_keys();
    if (!s_key_fds)
        printf("n31launcher: no key input - nothing can be started\n");

    /*
     * The first scan is inline, because there is nothing to draw yet and the
     * home screen should come up with the right list rather than filling in a
     * moment later. Every scan after this one is on the worker.
     */
    n31_apps_scan();
    for (uint8_t i = 0; i < n31_app_count; i++)
        printf("n31launcher: %-12s %s\n", n31_apps[i].prog, n31_apps[i].path);

    if (!n31_scanner_start())
        printf("n31launcher: no scanner thread - scanning inline\n");

    /* Only worth opening while there is nothing mounted to talk about. */
    if (!disk_mounted() && !n31_klog_open())
        printf("n31launcher: no /dev/kmsg - no bring-up commentary\n");

    n31_status_read(&s_status);
    n31_status_tilt(&s_status);
    printf("n31launcher: battery %s, bluetooth %s, accelerometer %s\n",
           s_status.have_battery ? "yes" : "no",
           s_status.bt_up ? "up" : (s_status.bt_present ? "down" : "absent"),
           s_status.have_tilt ? "yes" : "no");

    n31_ui_init();
    n31_ui_status_bar(&s_status);
    go(SCREEN_HOME);

    /*
     * The volume is brought up by n31-autostart, before this ever runs:
     * load-mods storage does the insmod and the recover, then autostart
     * mounts /mnt/disk and only then starts the launcher.
     *
     * This used to start the helper here as well, on the reasoning that
     * nothing else needed the volume. Something else did. Two independent
     * bring-ups raced, and because the helper's first stage asks for a
     * recover whenever the map does not look valid *to it*, the whole
     * thirty-second classify pass ran twice on every boot -- the second
     * result identical to the first and thrown away.
     *
     * So the helper is now only ever run because someone asked for it, from
     * the Extras tile. If autostart could not mount the volume, that is a
     * fault to show and retry deliberately, not to paper over by starting a
     * second bring-up behind the user's back.
     */

    uint32_t last_scan = millis();
    uint32_t last_status = millis();
    uint32_t last_tilt = millis();
    uint32_t last_fbcon = millis();
    uint32_t last_foreign = millis();
    bool     foreign = false;

    while (!s_quit) {
        pump_keys();

        /*
         * The kernel's running commentary on storage, while there is none.
         * It is the only sign of life during the bring-up, and it goes as
         * soon as something is mounted - after that it would be a log for
         * its own sake.
         */
        if (s_screen == SCREEN_HOME && !s_child) {
            static bool note_on;
            char line[96];

            if (disk_mounted()) {
                if (note_on) {
                    n31_ui_home_note(NULL);
                    n31_klog_close();
                    note_on = false;
                }
            } else if (n31_klog_poll(line, sizeof line)) {
                n31_ui_home_note(line);
                note_on = true;
            }
        }

        /*
         * Has the console taken the screen back? Checked even while an app is
         * running - especially then, since that is when a module load is most
         * likely and when the mess is most obvious.
         */
        if (millis() - last_fbcon >= FBCON_MS) {
            last_fbcon = millis();
            if (n31_fbcon_reassert()) {
                printf("n31launcher: console had rebound - taken back\n");
                fflush(stdout);
                /* It has been drawing over us, so nothing on screen can be
                   trusted. Only redraw if the screen is ours to redraw. */
                if (!s_child) n31_ui_redraw();
            }
        }

        /* A child that was asked to stop, going in its own time. The only
           thing this program starts now is an app. */
        if (reap_dying())
            after_app();

        app_gone();

        /* While an app owns the framebuffer the launcher draws nothing at all.
           There is no compositor here, and two writers would fight over every
           frame. */
        if (s_child) {
            usleep(FRAME_MS * 1000u);
            continue;
        }

        /*
         * The same rule for an app the launcher did not start.
         *
         * Something run over ssh is not our child, so none of the above
         * applies to it - and the result was both programs drawing the same
         * pixels, with the launcher showing through whatever the app painted.
         *
         * Polled rather than checked every frame: it walks /proc, and the
         * answer does not change between frames. On the way out of it the
         * screen is repainted, because whatever was there belongs to the
         * program that has just let go.
         */
        if (millis() - last_foreign >= FOREIGN_MS) {
            last_foreign = millis();
            bool now_foreign = n31_fbcon_foreign_owner();
            if (now_foreign != foreign) {
                foreign = now_foreign;
                printf("n31launcher: framebuffer %s\n",
                       now_foreign ? "taken by another process - going quiet"
                                   : "released - drawing again");
                fflush(stdout);
                if (!foreign) n31_ui_redraw();
            }
        }
        if (foreign) {
            usleep(FRAME_MS * 1000u);
            continue;
        }

        if (millis() - last_scan >= RESCAN_MS) {
            last_scan = millis();
            if (!n31_scanner_busy())
                n31_scanner_request();
        }

        /* Collect on every pass, not on the poll interval: the worker finishes
           when it finishes, and a result sitting unclaimed is a list that is
           already stale on screen. */
        rescan();

        /*
         * No scheduled retry. It only ever rearmed the automatic mount that
         * no longer happens, and an unattended retry of a bring-up is how a
         * single failure turns into a loop of thirty-second scans.
         */

        /* Only the home screen shows either of these, so only sample for it. */
        if (s_screen == SCREEN_HOME) {
            if (millis() - last_status >= STATUS_MS) {
                last_status = millis();
                n31_status_read(&s_status);
                n31_ui_status_bar(&s_status);
            }
            if (millis() - last_tilt >= TILT_MS) {
                last_tilt = millis();
                if (n31_status_tilt(&s_status))
                    n31_ui_tilt(s_status.tilt_x, s_status.tilt_y);
            }
        }

        uint32_t wait = lv_timer_handler();
        if (wait > FRAME_MS) wait = FRAME_MS;
        usleep(wait * 1000u);
    }

    /* Going away without taking them with us would leave something drawing to
       a screen nothing can get back. Blocking is fine here: nothing is drawing
       after this point. */
    n31_scanner_stop();
    n31_klog_close();
    if (s_child) stop_child_blocking(s_child);
    if (s_dying) stop_child_blocking(s_dying);

    /* Hand the screen back. Exiting the launcher should leave a console you
       can read, not the last frame it happened to draw. */
    n31_fbcon_restore();
    return 0;
}

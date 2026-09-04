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
#include <poll.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "ui.h"
#include "apps.h"
#include "backlight.h"
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

/*
 * No rescan interval.
 *
 * The volume is not always mounted when the launcher starts, so the list has
 * to be rebuilt when one appears - but on a timer it found the same apps it
 * found last time, every two seconds, and kept the storage awake to do it.
 * /proc/mounts reports a change by poll, so the scan happens at startup and
 * when the mount table actually moves.
 */

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
 * How long the loop may sleep, by what the screen is doing.
 *
 * While an app owns the framebuffer the launcher draws nothing and only has
 * to notice HOME, which poll() reports the instant it arrives - so a quarter
 * of a second between wake-ups costs nothing and leaves the CPU to the app
 * that needs it. The home screen has the parallax to feed; the others have
 * neither.
 */
/*
 * The Sleep button, as an input event like any other.
 *
 * gpio-d1830 reports it as KEY_POWER on a short press. Holding it is the
 * kernel's business - it cuts power itself after about half a second - and
 * n31-powerwatch deliberately does nothing with the short press, so this is
 * free to mean sleep and wake.
 */
#define N31_KEY_POWER 116

/*
 * How long the screen stays lit with nobody touching it.
 *
 * Everything the launcher draws when idle is decoration: the parallax, the
 * battery percentage, the tilt. None of it is worth a lit panel and a wake-up
 * every eighty milliseconds when nobody is looking.
 */
#define SLEEP_MS 30000

#define IDLE_QUIET_MS 250
#define IDLE_HOME_MS  TILT_MS
#define IDLE_OTHER_MS 120

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
static bool  s_child_owns_keys;   /* it handles HOME itself; leave it alone */
static screen_t s_child_from;     /* the screen to come back to */
static uint32_t s_launched_at;
static char     s_launched_prog[32];

/* Defined below with the other reporting; declared here because the reaper
   above it is where a child's exit is noticed. */
static void report_exit(int status, uint32_t ran_for);

/* An app that goes away sooner than this did not start, it failed. Long
   enough to cover a slow mount and the autostart script's own setup, short
   enough that nobody would have opened and closed something on purpose
   inside it. */
#define STARTED_MS 3000

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
static bool     s_asleep;          /* backlight off, drawing nothing */
static uint32_t s_last_input;      /* when a key last arrived */

/* Defined below, next to the poll they belong with; on_key comes first. */
static void go_to_sleep(void);
static void wake_up(void);

static int  s_mounts_fd = -1;   /* held open so it can be polled */
static bool s_mounted;          /* last answer, kept until the kernel says */
static bool s_scan_wanted = true;

/*
 * Read the mount table and note whether the volume is on it.
 *
 * Also re-arms the poll: the namespace compares its event counter against the
 * one taken at the last read, so reading is what makes the next change
 * signal again.
 */
static void mounts_read(void)
{
    char buf[4096];
    ssize_t n, total = 0;
    bool found = false;

    if (s_mounts_fd < 0) return;
    if (lseek(s_mounts_fd, 0, SEEK_SET) < 0) return;

    /* One line can straddle a read, so scan the whole thing once it is in. */
    while (total < (ssize_t)sizeof buf - 1 &&
           (n = read(s_mounts_fd, buf + total, sizeof buf - 1 - (size_t)total)) > 0)
        total += n;
    buf[total > 0 ? total : 0] = 0;

    for (char *l = buf; l && *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        if (strstr(l, " /mnt/") && strstr(l, " vfat ")) found = true;
        if (!e) break;
        *e = '\n';
        l = e + 1;
    }
    s_mounted = found;
}

static bool disk_mounted(void)
{
    return s_mounted;
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
    s_child_owns_keys = a->owns_keys;
    s_launched_at = millis();

    /* Kept so a failure note can name the app rather than saying "it". */
    {
        size_t i = 0;
        while (a->prog[i] && i < sizeof s_launched_prog - 1) {
            s_launched_prog[i] = a->prog[i];
            i++;
        }
        s_launched_prog[i] = 0;
    }
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
    s_child_owns_keys = false;

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

    int status = 0;
    pid_t r = waitpid(s_child, &status, WNOHANG);
    if (r != s_child && !(r < 0 && errno == ECHILD)) return false;

    uint32_t ran_for = millis() - s_launched_at;
    s_child = 0;
    s_child_owns_keys = false;
    after_app();
    report_exit(status, ran_for);
    return true;
}

/*
 * What just happened to the app, on the home screen.
 *
 * Only when something went wrong: an app that ran for a while and left with
 * nothing to say is somebody pressing HOME, and a launcher that commented on
 * that would be noise. Quick or unhappy is the interesting case, and it is
 * the one that used to be silent.
 */
static void report_exit(int status, uint32_t ran_for)
{
    bool exited = WIFEXITED(status);
    int  code = exited ? WEXITSTATUS(status) : 0;
    bool killed = WIFSIGNALED(status);

    if (!killed && exited && code == 0 && ran_for >= STARTED_MS) return;

    char note[192];
    int n = 0;

    n += snprintf(note + n, sizeof note - (size_t)n, "%s ",
                  s_launched_prog[0] ? s_launched_prog : "app");

    if (killed)
        n += snprintf(note + n, sizeof note - (size_t)n, "was killed (signal %d)",
                      WTERMSIG(status));
    else if (code == 127)
        /* The exec chain in launch() ends in _exit(127), and so does the
           shell's "not found" - either way nothing ran. */
        n += snprintf(note + n, sizeof note - (size_t)n, "was not found");
    else if (code)
        n += snprintf(note + n, sizeof note - (size_t)n, "exited (code %d)",
                      code);
    else
        n += snprintf(note + n, sizeof note - (size_t)n,
                      "closed straight away");

    /* And what autostart said it was going to run, which is the difference
       between "started" and "started the build I just made". */
    FILE *f = fopen("/tmp/n31-autostart.log", "r");
    if (f) {
        char line[160], last[160];
        last[0] = 0;
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
                line[--l] = 0;
            if (l) snprintf(last, sizeof last, "%s", line);
        }
        fclose(f);
        if (last[0])
            snprintf(note + n, sizeof note - (size_t)n, " - %s", last);
    }

    printf("n31launcher: %s\n", note);
    fflush(stdout);
    n31_ui_home_note(note);
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

    s_last_input = millis();

    /*
     * Asleep, any button wakes and does nothing else. Swallowing the press is
     * the point: a button found in a pocket should light the screen, not open
     * whatever it happened to land on.
     */
    if (s_asleep) {
        if (pressed) wake_up();
        return;
    }

    /*
     * While an app is running, every button is its business.
     *
     * The launcher used to take HOME and kill the child. That cost every app
     * its fourth button and made quitting arrive as a signal, with no chance
     * to write a config or stop a sink - so TinyPod could not use HOME to go
     * back, and leaving it was indistinguishable from a crash.
     *
     * Now an app that says so in its manifest is left entirely alone and is
     * expected to exit by itself. Everything else - fbDOOM, anything
     * third-party - is closed with a short press of the Sleep button, which
     * is the one button no app here does anything with. Holding it is still
     * the kernel's hard power-off, so nothing can trap the device.
     */
    if (s_child) {
        if (code == N31_KEY_POWER && pressed
            && !s_child_owns_keys
            && millis() - s_launched_at >= SCREEN_GUARD_MS) {
            close_app();
            n31_ui_redraw();
        }
        return;
    }

    /* On the launcher's own screens, the Sleep button sleeps. */
    if (code == N31_KEY_POWER) {
        if (pressed) go_to_sleep();
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

/*
 * Sleep until there is a key or the deadline, whichever comes first.
 *
 * The loop used to usleep a fixed frame time, so a keypress waited for the
 * next frame and an idle launcher woke thirty times a second to find nothing
 * to do. poll() gives back both: input is noticed the moment it arrives, so
 * the idle period can be as long as the screen's own needs allow.
 *
 * Floored, because the point of this is that the loop cannot spin. A zero
 * wait from lv_timer_handler - which happens whenever a timer came due while
 * we were busy elsewhere - used to mean no sleep at all.
 */
#define IDLE_FOREVER 0xFFFFFFFFu

static void idle_for(uint32_t ms)
{
    struct pollfd pfd[MAX_KEY_FDS + 1];
    int n = 0, mounts_slot = -1;

    if (ms < 4) ms = 4;

    /* Indefinite means indefinite: poll returns when a button or the mount
       table has something to say, and not before. */
    int timeout = ms == IDLE_FOREVER ? -1 : (int)ms;

    for (int i = 0; i < s_key_fds; i++) {
        pfd[n].fd = s_key_fd[i];
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }

    /* The mount table, which signals POLLPRI when it moves. This is the whole
       reason the launcher no longer scans on a timer. */
    if (s_mounts_fd >= 0) {
        mounts_slot = n;
        pfd[n].fd = s_mounts_fd;
        pfd[n].events = POLLPRI | POLLERR;
        pfd[n].revents = 0;
        n++;
    }

    if (!n) {
        /* No descriptors to wait on, so there is nothing to wake for either.
           Sleep a bounded amount rather than for ever. */
        usleep((ms == IDLE_FOREVER ? IDLE_QUIET_MS : ms) * 1000u);
        return;
    }

    if (poll(pfd, (nfds_t)n, timeout) <= 0)
        return;

    if (mounts_slot >= 0 &&
        (pfd[mounts_slot].revents & (POLLPRI | POLLERR))) {
        mounts_read();
        s_scan_wanted = true;
    }

    /*
     * A descriptor in error is ready for ever, which would turn this back
     * into the spin it was written to remove - and a device that has gone
     * away is exactly how that happens. Drop it; the launcher keeps whatever
     * other buttons it found, and with none it falls back to sleeping.
     */
    for (int i = n - 1; i >= 0; i--) {
        if (i == mounts_slot)
            continue;
        if (!(pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)))
            continue;
        for (int j = 0; j < s_key_fds; j++) {
            if (s_key_fd[j] != pfd[i].fd)
                continue;
            printf("n31launcher: input device gone, dropping it\n");
            fflush(stdout);
            close(s_key_fd[j]);
            s_key_fd[j] = s_key_fd[--s_key_fds];
            break;
        }
    }
}

/*
 * The screen off, and back on.
 *
 * Only the backlight is touched. LVGL keeps its display and its object tree
 * exactly as they were, so waking is a redraw of something already composed
 * rather than a rebuild - which is why this can be aggressive about when it
 * sleeps without being annoying to wake.
 */
static void go_to_sleep(void)
{
    if (s_asleep) return;
    s_asleep = true;
    n31_backlight_off();
    printf("n31launcher: screen off\n");
    fflush(stdout);
}

static void wake_up(void)
{
    if (!s_asleep) return;
    s_asleep = false;
    s_last_input = millis();
    n31_backlight_on();

    /* The console may have taken the framebuffer while nobody was looking,
       and nothing on screen has been drawn since before that. */
    n31_fbcon_reassert();
    n31_ui_redraw();
    printf("n31launcher: screen on\n");
    fflush(stdout);
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

    /*
     * The mount table, held open for the whole run so it can be polled. The
     * first read is what tells us the state now and arms the notification for
     * everything after it; if it will not open, s_mounted stays false and the
     * launcher behaves as it does with no volume, which is a state it already
     * handles.
     */
    n31_backlight_open();
    s_last_input = millis();

    s_mounts_fd = open("/proc/mounts", O_RDONLY);
    if (s_mounts_fd < 0)
        s_mounts_fd = open("/proc/self/mounts", O_RDONLY);
    if (s_mounts_fd >= 0)
        mounts_read();
    else
        printf("n31launcher: no /proc/mounts - assuming no volume\n");

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
           frame. The screen belongs to the app then, including whether it is
           lit - so an app that wants it dark can say so itself. */
        if (s_child) {
            s_last_input = millis();
            idle_for(IDLE_QUIET_MS);
            continue;
        }

        /*
         * Nobody has touched it for a while. Everything below this point
         * draws or samples something that only matters to someone looking at
         * it, so none of it runs; the wait below becomes indefinite and the
         * launcher costs nothing at all until a button arrives.
         */
        if (!s_asleep && millis() - s_last_input >= SLEEP_MS)
            go_to_sleep();

        if (s_asleep) {
            idle_for(IDLE_FOREVER);
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
            idle_for(IDLE_QUIET_MS);
            continue;
        }

        /*
         * Scan when there is a reason to, which means at startup and when the
         * mount table moves. It used to be every two seconds for ever, which
         * walked the volume looking for apps that were already found and kept
         * the storage awake to do it.
         */
        if (s_scan_wanted && !n31_scanner_busy()) {
            s_scan_wanted = false;
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

        /*
         * LVGL says when it next has something to do; the cap says how long
         * we are willing to wait anyway, and that depends on the screen
         * rather than on a fixed frame rate. An animation makes
         * lv_timer_handler ask for a short wait, so this follows one without
         * being told about it.
         */
        uint32_t wait = lv_timer_handler();
        uint32_t cap = s_screen == SCREEN_HOME ? IDLE_HOME_MS : IDLE_OTHER_MS;

        if (wait > cap) wait = cap;
        idle_for(wait);
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

/*
 * tg_input_linux.c — evdev, for the buttons and the accelerometer.
 * See tg_input.h.
 */

#include "tg_input.h"
#include "../core/tg_core.h"
#include "tg_tilt.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

/* The launcher's key codes, and for the same reason: they are the ones this
   hardware actually emits. */
#define KEY_VOLUMEDOWN_N31 114
#define KEY_VOLUMEUP_N31   115
#define KEY_PLAYPAUSE_N31  164
#define KEY_HOMEPAGE_N31   172
#define KEY_POWER_N31      116

#define MAX_FDS 8

/*
 * The event struct, spelled out.
 *
 * Not `struct input_event` from the kernel headers: on 32-bit with a 64-bit
 * time_t those headers and the kernel disagree about the size of the timestamp,
 * and reading with the wrong size silently shears every field after it - which
 * looks like random keys rather than like a bug. This is the layout the
 * launcher established works on this device: two longs, then the payload.
 */
struct evt {
    long     sec;
    long     usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

struct tg_input {
    int      fd[MAX_FDS];
    unsigned nfd;
    unsigned sources;

    uint8_t  keys;        /* buttons held via real keys */
    bool     quit;

    /* Accelerometer. The chip's own full-scale range, asked for rather than
       assumed, plus the last raw reading on each axis. */
    int      acc_fd;
    int      acc_min, acc_max;
    int      acc_x, acc_y, acc_z;
    tg_tilt  tilt;        /* the decision itself; see tg_tilt.h */
    uint8_t  tilt_bits;
};

/* ---- opening ------------------------------------------------------------- */

static bool has_type(int fd, unsigned bit)
{
    unsigned long types = 0;

    /* EVIOCGBIT(0, sizeof types) - which event TYPES, not which codes. Testing
       the key bitmap's first word instead would reject every device here,
       because every code we want is above 100. */
    if (ioctl(fd, EVIOCGBIT(0, sizeof types), &types) < 0) return false;
    return (types & (1UL << bit)) != 0;
}

/* Does this device report a real range on that axis? */
static bool abs_range(int fd, unsigned code, int *min, int *max)
{
    struct input_absinfo ai;

    if (ioctl(fd, EVIOCGABS(code), &ai) < 0) return false;
    /*
     * The ioctl succeeding proves nothing. evdev answers for every axis a
     * caller asks about, filling in zeros for the ones a device does not have,
     * so "the call worked" is true of ABS_MT_POSITION_X on an accelerometer.
     * A range is the actual evidence - and testing the return value instead is
     * what made the accelerometer look like a touchscreen and get dropped.
     */
    if (ai.maximum <= ai.minimum) return false;
    if (min) *min = ai.minimum;
    if (max) *max = ai.maximum;
    return true;
}

/* An accelerometer is an absolute device with X and Y and no touch slots. A
   touchscreen also reports ABS, so the distinguishing question is whether it
   has a real multitouch range - and on this device the touch controller is
   parked, but it still describes its axes. */
static bool looks_like_accel(int fd)
{
    if (!has_type(fd, EV_ABS)) return false;
    if (!abs_range(fd, ABS_X, NULL, NULL)) return false;
    if (abs_range(fd, ABS_MT_POSITION_X, NULL, NULL)) return false;
    return true;
}

/*
 * Where "level" is.
 *
 * Nobody holds a device at zero. Measured on the bench it rests at Y = -1086
 * of +-2304, which is most of a g down the long axis and already past any
 * useful threshold - so an uncalibrated tilt d-pad holds a direction down
 * permanently and the game is unplayable before it starts.
 *
 * So the neutral is whatever the accelerometer says during the first quarter
 * second, and every angle after that is measured from there. It follows that
 * the device should be held the way it is going to be played while this runs,
 * which is what the front end tells the player.
 */
static void calibrate(tg_input *in)
{
    long sx = 0, sy = 0, sz = 0;
    unsigned n = 0;
    struct timespec deadline, now;

    if (in->acc_fd < 0) return;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 250 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec++;
    }

    for (;;) {
        struct evt e;

        while (read(in->acc_fd, &e, sizeof e) == (ssize_t)sizeof e) {
            if (e.type != EV_ABS) continue;
            if (e.code == ABS_X) { sx += e.value; n++; }
            if (e.code == ABS_Y) { sy += e.value; }
            if (e.code == ABS_Z) { sz += e.value; }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;

        {
            struct timespec ts = { 0, 5 * 1000 * 1000 };

            nanosleep(&ts, NULL);
        }
    }

    /* No samples means the chip is not reporting; leaving the centre at zero
       is then no worse than any other guess, and tilt simply will not fire. */
    if (n) {
        in->acc_x = (int)(sx / (long)n);
        in->acc_y = (int)(sy / (long)n);
        in->acc_z = (int)(sz / (long)n);
        tg_tilt_set_centre(&in->tilt, in->acc_x, in->acc_y, in->acc_z);
        printf("tinygb: tilt %s\n", tg_tilt_describe(&in->tilt));
    }
}

/* Override the measured neutral, for a test that cannot hold anything. */
void tg_input_set_centre(tg_input *in, int x, int y, int z)
{
    if (!in) return;
    tg_tilt_set_centre(&in->tilt, x, y, z);
}

tg_input *tg_input_open(unsigned sources)
{
    tg_input *in = calloc(1, sizeof *in);

    if (!in) return NULL;

    in->acc_fd = -1;

    /*
     * About sixteen degrees to press and eight to release.
     *
     * The chip reads +-2304 for +-2 g, so a fraction of full scale is a sine
     * of the angle. Far enough that holding the device naturally presses
     * nothing, close enough that a flick of the wrist does.
     */
    tg_tilt_init(&in->tilt, -1, 1, 14, 7);

    for (int i = 0; i < 16 && in->nfd < MAX_FDS; i++) {
        char path[64];
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) continue;

        if ((sources & TG_SRC_TILT) && in->acc_fd < 0 && looks_like_accel(fd) &&
            abs_range(fd, ABS_X, &in->acc_min, &in->acc_max)) {
            in->acc_fd = fd;
            tg_tilt_init(&in->tilt, in->acc_min, in->acc_max, 14, 7);
            in->sources |= TG_SRC_TILT;
            in->fd[in->nfd++] = fd;
            continue;
        }

        if ((sources & TG_SRC_KEYS) && has_type(fd, EV_KEY)) {
            in->sources |= TG_SRC_KEYS;
            in->fd[in->nfd++] = fd;
            continue;
        }

        close(fd);
    }

    calibrate(in);
    return in;
}

void tg_input_close(tg_input *in)
{
    if (!in) return;
    for (unsigned i = 0; i < in->nfd; i++) close(in->fd[i]);
    free(in);
}

unsigned tg_input_sources(const tg_input *in) { return in ? in->sources : 0; }

void tg_input_set_tilt(tg_input *in, int on_pct, int off_pct)
{
    int cx, cy, cz;

    if (!in) return;
    /* Keep the measured neutral across a threshold change - re-init would
       throw away the calibration and put level back at zero. */
    cx = in->tilt.cx;
    cy = in->tilt.cy;
    cz = in->tilt.cz;
    tg_tilt_init(&in->tilt, in->acc_min, in->acc_max, on_pct, off_pct);
    tg_tilt_set_centre(&in->tilt, cx, cy, cz);
}

/* ---- the mapping --------------------------------------------------------- */

/*
 * Volume Up is A, Volume Down is B, Play/Pause is Start.
 *
 * The two volume keys are on the side under a right thumb, which is where A
 * and B belong. Select has no key: it is needed once per session in most games
 * and is reached by holding Play/Pause, below.
 */
static uint8_t key_to_button(uint16_t code)
{
    switch (code) {
    case KEY_VOLUMEUP_N31:   return TG_A;
    case KEY_VOLUMEDOWN_N31: return TG_B;
    case KEY_PLAYPAUSE_N31:  return TG_START;
    default:                 return 0;
    }
}

uint8_t tg_input_poll(tg_input *in)
{
    struct evt e;

    if (!in) return 0;

    for (unsigned i = 0; i < in->nfd; i++) {
        while (read(in->fd[i], &e, sizeof e) == (ssize_t)sizeof e) {
            if (e.type == EV_KEY) {
                uint8_t b = key_to_button(e.code);

                if (e.code == KEY_HOMEPAGE_N31) {
                    if (e.value == 1) in->quit = true;
                    continue;
                }
                /* Power belongs to the launcher's sleep handling. Never a
                   game button, and never swallowed here. */
                if (e.code == KEY_POWER_N31) continue;

                if (!b) continue;
                if (e.value == 1)      in->keys |= b;   /* press */
                else if (e.value == 0) in->keys &= (uint8_t)~b;
                /* value 2 is autorepeat, which for a held button is a no-op. */
            } else if (e.type == EV_ABS) {
                if (e.code == ABS_X) in->acc_x = e.value;
                if (e.code == ABS_Y) in->acc_y = e.value;
                if (e.code == ABS_Z) in->acc_z = e.value;
            }
        }
    }

    if (in->sources & TG_SRC_TILT)
        in->tilt_bits = tg_tilt_feed(&in->tilt, in->acc_x, in->acc_y,
                                     in->acc_z);

    return (uint8_t)(in->keys | in->tilt_bits);
}

bool tg_input_take_quit(tg_input *in)
{
    bool q;

    if (!in) return false;
    q = in->quit;
    in->quit = false;
    return q;
}

/* ---- the probe ----------------------------------------------------------- */

void tg_input_probe(unsigned secs)
{
    int fds[MAX_FDS];
    unsigned n = 0;
    time_t end;

    printf("input devices:\n");

    for (int i = 0; i < 16 && n < MAX_FDS; i++) {
        char path[64], name[128] = "?";
        struct input_absinfo ai;
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) continue;

        ioctl(fd, EVIOCGNAME(sizeof name), name);
        printf("  %-18s %-26s %s%s\n", path, name,
               has_type(fd, EV_KEY) ? "keys " : "",
               has_type(fd, EV_ABS) ? "abs" : "");

        if (has_type(fd, EV_ABS)) {
            static const struct { unsigned code; const char *nm; } axes[] = {
                { ABS_X, "X" }, { ABS_Y, "Y" }, { ABS_Z, "Z" },
                { ABS_MT_POSITION_X, "MT_X" }, { ABS_MT_POSITION_Y, "MT_Y" },
            };

            for (unsigned k = 0; k < sizeof axes / sizeof axes[0]; k++)
                if (ioctl(fd, EVIOCGABS(axes[k].code), &ai) >= 0 &&
                    (ai.minimum || ai.maximum))
                    printf("      %-5s %d..%d  now %d\n", axes[k].nm,
                           ai.minimum, ai.maximum, ai.value);
        }
        fds[n++] = fd;
    }

    printf("\nevents for %u seconds - press buttons, tilt the device:\n", secs);
    end = time(NULL) + (time_t)secs;

    while (time(NULL) < end) {
        struct evt e;
        bool any = false;

        for (unsigned i = 0; i < n; i++) {
            while (read(fds[i], &e, sizeof e) == (ssize_t)sizeof e) {
                if (e.type == EV_KEY)
                    printf("  event%u KEY  code %-4u %s\n", i, e.code,
                           e.value == 1 ? "down" : e.value == 0 ? "up" : "repeat");
                else if (e.type == EV_ABS)
                    printf("  event%u ABS  axis %-4u %d\n", i, e.code, e.value);
                any = true;
            }
        }
        if (!any) {
            struct timespec ts = { 0, 20 * 1000 * 1000 };

            nanosleep(&ts, NULL);
        }
        fflush(stdout);
    }

    for (unsigned i = 0; i < n; i++) close(fds[i]);
}

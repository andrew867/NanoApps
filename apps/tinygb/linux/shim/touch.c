/*
 * touch.c — see touch.h.
 */

#include "touch.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * The ioctls, spelled out.
 *
 * linux/input.h conflicts with some libc headers on this toolchain, which is
 * why the rest of this tree hand-builds these too. The encoding is
 * _IOC(dir, type, nr, size): direction 2 (read) in the top two bits, the size
 * at bit 16, 'E' for the type and the number in the low byte.
 *
 *   EVIOCGNAME(len)          nr 0x06
 *   EVIOCGBIT(EV_ABS, len)   nr 0x20 + EV_ABS, and EV_ABS is 3
 */
#define IOC_READ_E(nr, size) \
    ((unsigned long)(0x80000000u | ((unsigned)(size) << 16) | ('E' << 8) | (nr)))

#define EVIOCGNAME_(size)    IOC_READ_E(0x06, size)
#define EVIOCGBIT_ABS_(size) IOC_READ_E(0x20 + 3, size)

/* The axes that make something a pointer. ABS_X and ABS_Y are 0 and 1;
   ABS_MT_POSITION_X and _Y are 0x35 and 0x36 - a multitouch driver may report
   only the second pair, and LVGL reads either. */
#define ABS_X_BIT          0
#define ABS_Y_BIT          1
#define ABS_MT_X_BIT       0x35
#define ABS_MT_Y_BIT       0x36

static int has_bit(const unsigned char *bits, unsigned n)
{
    return (bits[n / 8] >> (n % 8)) & 1;
}

/*
 * Names that are a pointer but are not a touch panel.
 *
 * The accelerometer reports absolute axes and would otherwise be the first
 * device that looks like a pointer - and LVGL would then follow the tilt of
 * the device around the screen, which is a very confusing bug to be looking
 * at from the outside.
 */
static int looks_like_a_sensor(const char *name)
{
    /*
     * Long enough to mean something. "als" for an ambient light sensor was in
     * here and is a substring of far too much - a panel called "Alps" or a
     * driver with "signals" in its name would have been ruled out by it, and
     * the failure would have been a touchscreen that simply was not found.
     * The accelerometer on this device is lis3lv02d, which "lis3" and "accel"
     * both catch.
     */
    static const char *const no[] = {
        "accel", "gyro", "lis3", "compass", "magnet", "ambient"
    };
    unsigned i;

    for (i = 0; i < sizeof no / sizeof no[0]; i++)
        if (strstr(name, no[i]))
            return 1;
    return 0;
}

static int looks_like_touch(const char *name)
{
    return strstr(name, "touch") || strstr(name, "digitiz") ||
           strstr(name, "screen") || strstr(name, "ts");
}

static void lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

const char *n31_touch_find(void)
{
    static char path[64];
    static char best[64];
    int i;

    best[0] = 0;

    for (i = 0; i < 32; i++) {
        unsigned char abs_bits[16] = { 0 };
        char name[128] = { 0 };
        int pointer;
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        /* What it can report. A device with no absolute axes cannot be a
           touch panel, whatever it is called. */
        pointer = 0;
        if (ioctl(fd, EVIOCGBIT_ABS_(sizeof abs_bits), abs_bits) >= 0)
            pointer = (has_bit(abs_bits, ABS_X_BIT) &&
                       has_bit(abs_bits, ABS_Y_BIT)) ||
                      (has_bit(abs_bits, ABS_MT_X_BIT) &&
                       has_bit(abs_bits, ABS_MT_Y_BIT));

        if (ioctl(fd, EVIOCGNAME_(sizeof name), name) < 0)
            name[0] = 0;
        close(fd);

        if (!pointer)
            continue;

        lower(name);

        /*
         * A name that says touch wins outright, and is checked before
         * anything is ruled out - the panel here is "Apple Grape
         * Touchscreen", and a device that says what it is should not have to
         * survive a list of things it might be confused with.
         */
        if (looks_like_touch(name))
            return path;

        /*
         * Everything else that reports absolute axes is a fallback, so a
         * panel whose driver names it something unexpected is still found.
         * The accelerometer reports absolute axes too, and picking it would
         * mean LVGL following the tilt of the device around the screen - a
         * very confusing thing to be looking at from the outside.
         */
        if (looks_like_a_sensor(name))
            continue;

        if (!best[0])
            snprintf(best, sizeof best, "%s", path);
    }

    if (best[0]) {
        snprintf(path, sizeof path, "%s", best);
        return path;
    }
    return NULL;
}

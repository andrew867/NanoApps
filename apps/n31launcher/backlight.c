#include "backlight.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The backlight class, and nothing below it.
 *
 * bl_power takes the FB_BLANK values: 0 is unblanked, 4 is powered down. That
 * is the interface the driver publishes and the one a driver is obliged to
 * honour, so it is what gets used. Writing brightness 0 is the fallback for a
 * driver that exposes no bl_power - it dims the panel to nothing without
 * telling the driver it may cut its regulator, which is worse for power and
 * still dark.
 */
#define BL_UNBLANK 0
#define BL_POWERDOWN 4

/* A class device name is short in practice and unbounded in the header, so
   every path built from one is bounded here rather than hoped about. */
#define NAME_MAX_USED 48

static char s_dir[192];
static char s_name[NAME_MAX_USED + 1];
static int  s_max = 0;
static int  s_level = 0;      /* what to come back to */
static bool s_have = false;
static bool s_off = false;

static bool read_int(const char *path, int *out)
{
    char buf[32];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) return false;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    *out = atoi(buf);
    return true;
}

static bool write_int(const char *path, int v)
{
    char buf[32];
    int len, fd = open(path, O_WRONLY);
    bool ok;

    if (fd < 0) return false;
    len = snprintf(buf, sizeof buf, "%d\n", v);
    ok = write(fd, buf, (size_t)len) == (ssize_t)len;
    close(fd);
    return ok;
}

bool n31_backlight_open(void)
{
    /* The class path, overridable so this can be exercised against a fake
       tree on a host that has no backlight. Nothing on the device sets it. */
    const char *cls = getenv("N31_BACKLIGHT_CLASS");
    DIR *d;
    struct dirent *e;

    if (!cls || !*cls) cls = "/sys/class/backlight";
    d = opendir(cls);

    if (!d) {
        printf("n31launcher: no backlight class - screen stays lit\n");
        fflush(stdout);
        return false;
    }

    while ((e = readdir(d)) != NULL) {
        char path[256];

        if (e->d_name[0] == '.') continue;

        snprintf(path, sizeof path, "%s/%.*s/max_brightness",
                 cls, NAME_MAX_USED, e->d_name);
        if (!read_int(path, &s_max) || s_max <= 0) continue;

        snprintf(s_dir, sizeof s_dir, "%s/%.*s", cls,
                 NAME_MAX_USED, e->d_name);
        snprintf(s_name, sizeof s_name, "%.*s", NAME_MAX_USED, e->d_name);

        snprintf(path, sizeof path, "%s/brightness", s_dir);
        if (!read_int(path, &s_level) || s_level <= 0)
            s_level = s_max;      /* came up dark, or unreadable: full on */

        s_have = true;
        break;
    }
    closedir(d);

    if (!s_have) {
        printf("n31launcher: no usable backlight - screen stays lit\n");
        fflush(stdout);
        return false;
    }

    printf("n31launcher: backlight %s, level %d of %d\n",
           s_name, s_level, s_max);
    fflush(stdout);
    return true;
}

void n31_backlight_off(void)
{
    char path[256];
    int now;

    if (!s_have || s_off) return;

    /* Whatever it is now is what to come back to - it may have been changed
       by something else since the last time through here. */
    snprintf(path, sizeof path, "%s/brightness", s_dir);
    if (read_int(path, &now) && now > 0)
        s_level = now;

    snprintf(path, sizeof path, "%s/bl_power", s_dir);
    if (!write_int(path, BL_POWERDOWN)) {
        snprintf(path, sizeof path, "%s/brightness", s_dir);
        write_int(path, 0);
    }
    s_off = true;
}

void n31_backlight_on(void)
{
    char path[256];

    if (!s_have || !s_off) return;

    /* Level first, then power: a driver that cut its regulator brings it back
       at whatever brightness it finds, and setting that before unblanking is
       the difference between coming up right and coming up at full and then
       stepping down. */
    snprintf(path, sizeof path, "%s/brightness", s_dir);
    write_int(path, s_level > 0 ? s_level : s_max);

    snprintf(path, sizeof path, "%s/bl_power", s_dir);
    write_int(path, BL_UNBLANK);

    s_off = false;
}

const char *n31_backlight_name(void)
{
    return s_have ? s_name : NULL;
}

/*
 * fbcon.c — see fbcon.h.
 */

#include "fbcon.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/* The bind file of the framebuffer console, once found. Empty means we never
   found one, or never took it - and in that case nothing here does anything,
   because putting back something we did not take would turn the console on for
   a system that had deliberately turned it off. */
static char s_bind[128];
static bool s_taken;              /* we unbound it at startup */
static bool s_lent;               /* and have since given it back, temporarily */

static bool read_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    bool ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    return ok;
}

static bool write_char(const char *path, char c)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;

    bool ok = fputc(c, f) != EOF;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/*
 * Find the framebuffer console by asking each vtcon what it is, rather than
 * assuming it is vtcon1. It usually is, but the numbering depends on which
 * console drivers registered and in what order, and unbinding the wrong one
 * would take out the dummy console instead - leaving the screen exactly as it
 * was and the reason why entirely invisible.
 */
bool n31_fbcon_detach(void)
{
    DIR *d = opendir("/sys/class/vtconsole");
    if (!d) return false;

    bool done = false;
    struct dirent *e;

    while ((e = readdir(d)) && !done) {
        if (strncmp(e->d_name, "vtcon", 5) != 0) continue;

        char path[128], name[128];
        snprintf(path, sizeof path, "/sys/class/vtconsole/%s/name", e->d_name);
        if (!read_line(path, name, sizeof name)) continue;
        if (!strstr(name, "frame buffer")) continue;

        snprintf(path, sizeof path, "/sys/class/vtconsole/%s/bind", e->d_name);

        /* Already unbound - by an earlier run, or by the boot arguments. The
           screen is ours either way, but it is not ours to give back. */
        char state[8];
        if (read_line(path, state, sizeof state) && state[0] == '0')
            return true;

        if (write_char(path, '0')) {
            snprintf(s_bind, sizeof s_bind, "%s", path);
            s_taken = true;
            done = true;
        }
    }

    closedir(d);
    return done;
}

bool n31_fbcon_reassert(void)
{
    char state[8];

    /* Never held it, or deliberately handed it to a console app. */
    if (!s_taken || s_lent || !s_bind[0])
        return false;

    if (!read_line(s_bind, state, sizeof state))
        return false;
    if (state[0] != '1')
        return false;                   /* still ours */

    return write_char(s_bind, '0');
}

void n31_fbcon_lend(void)
{
    if (!s_taken || s_lent) return;

    if (write_char(s_bind, '1')) s_lent = true;
}

void n31_fbcon_reclaim(void)
{
    if (!s_lent) return;

    write_char(s_bind, '0');
    s_lent = false;
}

/*
 * Clear the terminal the console app will draw through. Written to /dev/tty0
 * rather than to our own stdout, which is a log file or a pipe and not the
 * screen at all.
 */
void n31_fbcon_clear(void)
{
    static const char seq[] = "\033[H\033[2J\033[3J";

    int fd = open("/dev/tty0", O_WRONLY);
    if (fd < 0) fd = open("/dev/console", O_WRONLY);
    if (fd < 0) return;

    ssize_t n = write(fd, seq, sizeof seq - 1);
    (void)n;
    close(fd);
}

void n31_fbcon_restore(void)
{
    if (!s_taken) return;

    if (!s_lent) write_char(s_bind, '1');
    s_taken = false;
    s_lent = false;
    s_bind[0] = 0;
}

/*
 * Somebody else has /dev/fb0 open. See the header for why this is a poll over
 * /proc rather than a question asked of anything.
 */
bool n31_fbcon_foreign_owner(void)
{
    const pid_t self = getpid();

    DIR *proc = opendir("/proc");
    if (!proc) return false;

    bool found = false;
    struct dirent *pe;

    while (!found && (pe = readdir(proc))) {
        if (pe->d_name[0] < '0' || pe->d_name[0] > '9') continue;

        long pid = strtol(pe->d_name, NULL, 10);
        if (pid <= 0 || (pid_t)pid == self) continue;

        char fddir[64];
        snprintf(fddir, sizeof fddir, "/proc/%ld/fd", pid);

        DIR *fds = opendir(fddir);
        if (!fds) continue;                 /* gone, or not ours to look at */

        struct dirent *fe;
        while ((fe = readdir(fds))) {
            if (fe->d_name[0] == '.') continue;

            char link[96], target[128];
            snprintf(link, sizeof link, "%s/%s", fddir, fe->d_name);

            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;

            /* Any framebuffer, not only fb0: this device has one today, and a
               launcher that only checked fb0 would be quietly wrong on a
               device that had two. */
            if (!strncmp(target, "/dev/fb", 7)) {
                found = true;
                break;
            }
        }
        closedir(fds);
    }

    closedir(proc);
    return found;
}

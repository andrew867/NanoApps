/*
 * klog.c — see klog.h.
 */

#include "klog.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int s_fd = -1;

/*
 * What counts as storage. Deliberately narrow: the point is to show why the
 * apps are not there yet, and a line about anything else is a distraction at
 * exactly the moment someone is watching the screen wondering.
 */
static const char *k_interesting[] = {
    "ftl", "FTL", "nand", "NAND", "whimory", "s5l8740-ipod", "s5l8740-ftl",
    "vfat", "FAT-fs", "mount", "CXT", "BTOC", "recover",
};

bool n31_klog_open(void)
{
    if (s_fd >= 0) return true;

    s_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (s_fd < 0) return false;

    /* Start at the end. The interesting part is what happens from now on;
       replaying the whole boot would scroll past faster than anyone could
       read and end on something long finished. */
    lseek(s_fd, 0, SEEK_END);
    return true;
}

void n31_klog_close(void)
{
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
}

static bool interesting(const char *line)
{
    for (unsigned i = 0; i < sizeof k_interesting / sizeof k_interesting[0]; i++)
        if (strstr(line, k_interesting[i])) return true;
    return false;
}

bool n31_klog_poll(char *out, size_t cap)
{
    char buf[1024];
    bool got = false;

    if (s_fd < 0 || cap < 2) return false;

    /*
     * Drain everything waiting and keep the last match rather than the first.
     * A poll can span several messages, and the newest is the one that says
     * where the bring-up has actually got to.
     */
    for (;;) {
        ssize_t n = read(s_fd, buf, sizeof buf - 1);

        if (n <= 0) {
            /*
             * EAGAIN is the normal "nothing new". EPIPE means records were
             * overwritten while we were not looking, which is not an error -
             * the next read simply resumes from what survived.
             */
            break;
        }
        buf[n] = 0;

        /* "priority,sequence,timestamp,flags;text" - everything before the
           semicolon is bookkeeping. */
        char *text = strchr(buf, ';');
        if (!text) continue;
        text++;

        /* Continuation lines are appended as "\n key=value"; the message is
           the first line only. */
        char *nl = strchr(text, '\n');
        if (nl) *nl = 0;

        if (!interesting(text)) continue;

        /* Control characters would draw as boxes. */
        for (char *p = text; *p; p++)
            if ((unsigned char)*p < 0x20) *p = ' ';

        snprintf(out, cap, "%s", text);
        got = true;
    }

    return got;
}

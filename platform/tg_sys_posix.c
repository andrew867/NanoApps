/*
 * tg_sys_posix.c — tg_sys.h on Linux and the desktop.
 */

#include "tg_sys.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

long tg_file_size(const char *path)
{
    struct stat st;

    if (!path || stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

long tg_file_read(const char *path, void *buf, size_t cap)
{
    FILE *f;
    size_t got;

    if (!path || !(f = fopen(path, "rb"))) return -1;
    got = fread(buf, 1, cap, f);
    fclose(f);
    return (long)got;
}

bool tg_file_write(const char *path, const void *buf, size_t len)
{
    FILE *f;
    bool ok;

    if (!path || !(f = fopen(path, "wb"))) return false;
    ok = fwrite(buf, 1, len, f) == len;
    /* Flushed before the handle goes away, so a power cut a moment later
       loses nothing this call claimed to have written. */
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool tg_file_exists(const char *path)
{
    return tg_file_size(path) >= 0;
}

long long tg_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void tg_log(const char *line)
{
    fprintf(stderr, "tinygb: %s\n", line ? line : "");
}

const char *tg_log_recent(unsigned i)
{
    (void)i;
    return NULL;
}

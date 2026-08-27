/*
 * sys_host.c — sys.h on a Linux desktop.
 *
 * The battery is faked (slowly draining, so the header's readout is visibly
 * alive rather than a frozen 100%), the backlight is a no-op, and everything
 * else is ordinary POSIX.
 */

#include "sys.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

static uint64_t s_start_us;
static bool     s_exit;
static char     s_cache[512];
static char     s_programs[512];

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

uint32_t en_sys_millis(void)
{
    if (!s_start_us) s_start_us = now_us();
    return (uint32_t)((now_us() - s_start_us) / 1000ull);
}

int en_sys_battery_percent(void)
{
    /* Drift down one percent a minute from 87, wrapping, so the readout moves
       during a long session and layout bugs in the header show up. */
    uint32_t mins = en_sys_millis() / 60000u;
    int v = 87 - (int)(mins % 80u);
    return v < 7 ? v + 80 : v;
}

bool en_sys_battery_charging(void) { return false; }

void en_sys_wake_lock(bool on) { (void)on; }
void en_sys_backlight(int percent) { (void)percent; }

static const char *base_dir(void)
{
    const char *e = getenv("ENTRAIN_HOME");
    if (e && *e) return e;
    static char buf[512];
    const char *home = getenv("HOME");
    snprintf(buf, sizeof buf, "%s/.entrain", home ? home : "/tmp");
    return buf;
}

static void ensure_dir(const char *p)
{
    mkdir(p, 0755);
}

const char *en_sys_cache_dir(void)
{
    if (!s_cache[0]) {
        ensure_dir(base_dir());
        snprintf(s_cache, sizeof s_cache, "%s/cache", base_dir());
        ensure_dir(s_cache);
    }
    return s_cache;
}

const char *en_sys_programs_dir(void)
{
    if (!s_programs[0]) {
        ensure_dir(base_dir());
        snprintf(s_programs, sizeof s_programs, "%s/programs", base_dir());
        ensure_dir(s_programs);
    }
    return s_programs;
}

bool en_sys_prefs_load(void *blob, uint32_t size)
{
    char path[600];
    snprintf(path, sizeof path, "%s/prefs.bin", base_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(blob, 1, size, f);
    fclose(f);
    return n == size;
}

bool en_sys_prefs_save(const void *blob, uint32_t size)
{
    ensure_dir(base_dir());
    char path[600];
    snprintf(path, sizeof path, "%s/prefs.bin", base_dir());
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = fwrite(blob, 1, size, f);
    fclose(f);
    return n == size;
}

uint32_t en_sys_read_file(const char *path, void *buf, uint32_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return (uint32_t)n;
}

bool en_sys_list_dir(const char *dir, int index, char *name_out, int name_cap)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    int i = 0;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (i == index) {
            snprintf(name_out, (size_t)name_cap, "%s", e->d_name);
            found = true;
            break;
        }
        i++;
    }
    closedir(d);
    return found;
}

void *en_sys_alloc(uint32_t size) { return malloc(size); }
void  en_sys_free(void *p) { free(p); }

void en_sys_request_exit(void) { s_exit = true; }
bool en_sys_exit_requested(void) { return s_exit; }

const char *en_sys_platform_name(void) { return "Linux host"; }

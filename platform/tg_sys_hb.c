/*
 * tg_sys_hb.c — tg_sys.h on RetailOS, over the NanoApps SDK.
 *
 * Also the two things a freestanding build has to supply for code that was
 * written with a libc in mind: abort(), which the vendored core names but
 * never reaches, and a monotonic clock.
 */

#include "tg_sys.h"
#include "tg_util.h"

#include "hb_sdk.h"

#include <string.h>

long tg_file_size(const char *path)
{
    if (!path || !hb_fs_exists(path)) return -1;
    /* hb_fs_size is 0 for both empty and missing; exists() above settles
       which, so 0 here really is an empty file. */
    return (long)hb_fs_size(path);
}

long tg_file_read(const char *path, void *buf, size_t cap)
{
    if (!path || !hb_fs_exists(path)) return -1;
    /* One call, offset 0: the SDK has no seek, and a file longer than `cap`
       is simply cut off here, which is why callers size-check first. */
    return (long)hb_fs_read(path, buf, (uint32_t)cap);
}

bool tg_file_write(const char *path, const void *buf, size_t len)
{
    /* hb_fs_write truncates, writes, sets EOF and syncs before it returns. */
    return path && hb_fs_write(path, buf, (uint32_t)len);
}

bool tg_file_exists(const char *path)
{
    return path && hb_fs_exists(path);
}

long long tg_now_ns(void)
{
    /* Milliseconds are all the SDK offers as a stable unit; a 32-bit ms
       count wraps after 49 days, which is longer than this device stays on. */
    return (long long)hb_time_uptime_ms() * 1000000LL;
}

/* ---- the log ------------------------------------------------------------- */

#define LOG_KEEP 4
#define LOG_LEN  96

static char     s_lines[LOG_KEEP][LOG_LEN];
static unsigned s_head;    /* the next slot to write */
static unsigned s_count;

void tg_log(const char *line)
{
    tg_strlcpy(s_lines[s_head], line ? line : "", LOG_LEN);
    s_head = (s_head + 1) % LOG_KEEP;
    if (s_count < LOG_KEEP) s_count++;

    /* Four characters of it into the trace ring, so `start trace` from the
       NanoApps tree shows that something was said even when the screen
       cannot. The tag is the line's first four letters, which for the lines
       this app writes is enough to tell them apart. */
    {
        char tag[5] = { 'T', 'G', ' ', ' ', 0 };

        if (line) {
            for (unsigned i = 0; i < 4 && line[i]; i++)
                tag[i] = line[i];
        }
        hb_trace_log(tag, 0, 0);
    }
}

const char *tg_log_recent(unsigned i)
{
    if (i >= s_count) return NULL;
    return s_lines[(s_head + LOG_KEEP - 1 - i) % LOG_KEEP];
}

/* ---- what the freestanding build lacks ----------------------------------- */

/* Peanut-GB names abort() in a branch it only takes when the compiler has no
   __builtin_unreachable, which this one has; the reference still has to
   resolve. There is nothing sensible to do here but stop moving. */
void abort(void)
{
    hb_trace_log("ABRT", 0, 0);
    for (;;) { }
}

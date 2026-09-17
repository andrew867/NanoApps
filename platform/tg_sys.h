/*
 * tg_sys.h — the operating system, reduced to what the portable modules need.
 *
 * Three things: whole files in and out, a clock, and somewhere to say what
 * went wrong. Linux answers with stdio and clock_gettime; RetailOS with
 * hb_fs, hb_time and the DRAM trace ring. tg_save.c and the menus are written
 * against this and compile unchanged for both.
 *
 * Whole files on purpose. RetailOS has no seek and no partial read, so an
 * interface that offered one would be a promise one target could not keep;
 * and nothing here needs one - a cartridge, a save and a state are each read
 * once and written whole.
 */

#ifndef TINYGB_SYS_H
#define TINYGB_SYS_H

#include <stdbool.h>
#include <stddef.h>

/* Size in bytes, or -1 when the file cannot be found. */
long tg_file_size(const char *path);

/* Read up to `cap` bytes into `buf`. Bytes read, or -1 when it could not be
   opened. A file larger than `cap` is NOT an error here; check the size
   first if that matters, as the cartridge loader does. */
long tg_file_read(const char *path, void *buf, size_t cap);

/* Create or replace `path` with exactly these bytes, durable on return. */
bool tg_file_write(const char *path, const void *buf, size_t len);

bool tg_file_exists(const char *path);

/* Monotonic nanoseconds, from any epoch. */
long long tg_now_ns(void);

/* One line, no newline. Linux prints it; RetailOS keeps the last few for the
   About page and drops a breadcrumb in the trace ring. */
void tg_log(const char *line);

/* The i-th most recent line, newest first, or NULL. Only RetailOS keeps
   any. */
const char *tg_log_recent(unsigned i);

#endif /* TINYGB_SYS_H */

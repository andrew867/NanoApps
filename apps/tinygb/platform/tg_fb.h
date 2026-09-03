/*
 * tg_fb.h — the Linux framebuffer, as much of it as TinyGB needs.
 *
 * Enough to open /dev/fb0, find out what shape it is, and get a scaled frame
 * onto it. Not a graphics library: the menu in Phase 03 is LVGL's job, and the
 * emulator's own path stays independent of it so a dropped frame is never
 * something LVGL did.
 *
 * The geometry is read from the device rather than assumed. It happens to be
 * 240x432 at 32 bpp with a 960-byte line, and every one of those has been
 * different at some point during this port.
 */

#ifndef TINYGB_FB_H
#define TINYGB_FB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int       fd;
    uint32_t *pixels;       /* the mapping, as 32-bit pixels */
    size_t    map_len;      /* what to unmap */

    unsigned  w, h;         /* visible size, in pixels */
    unsigned  stride_px;    /* row pitch in PIXELS, from line_length / 4 */
    unsigned  bpp;

    bool      took_console; /* we detached fbcon and owe it back */
} tg_fb;

/*
 * Open and map. `path` may be NULL for /dev/fb0.
 *
 * On success the console has also been detached if it could be, because
 * otherwise every kernel message lands on top of the picture. It is given back
 * by tg_fb_close, so a crash-free exit leaves a usable console behind.
 *
 * Returns false and leaves *fb zeroed on failure, with a reason on stderr.
 */
bool tg_fb_open(tg_fb *fb, const char *path);

void tg_fb_close(tg_fb *fb);

/* Every pixel to one colour. */
void tg_fb_fill(tg_fb *fb, uint32_t rgb);

/*
 * Where the picture goes.
 *
 * The Game Boy screen sits at the top and the controls go beneath it, which on
 * a 432-tall panel is an exact half each. Returned rather than hardcoded at the
 * call site so Phase 03 has one place to move it.
 */
void tg_fb_layout(const tg_fb *fb, unsigned *x, unsigned *y);

/* The pixel at (x, y), for a caller that wants to draw its own. NULL if it is
   outside the visible area. */
uint32_t *tg_fb_at(const tg_fb *fb, unsigned x, unsigned y);

#endif /* TINYGB_FB_H */

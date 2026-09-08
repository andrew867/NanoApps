/*
 * drmfb.h — a plain writable surface on the DRM display, for apps that are
 * not LVGL.
 *
 * TinyGB and fbDOOM both do the same thing: mmap a framebuffer, write pixels
 * into it, and expect the screen to follow. Through /dev/fb0 the last part is
 * fb_deferred_io noticing page faults on a timer, which caps them at about
 * twenty frames a second whatever they do. This is the same shape against the
 * DRM device instead, where telling the driver is explicit and immediate.
 *
 * WHY ONE BUFFER AND A DAMAGE CALL, NOT TWO AND A FLIP
 *
 * Double buffering is the usual answer and is wrong for these two. Both draw
 * INCREMENTALLY - TinyGB's scaler skips rows that did not change between
 * frames, and DOOM's status bar is left alone for most of a tick - and with
 * two buffers each is a frame stale, so "unchanged since last frame" becomes
 * false and every skip-optimisation turns into a bug that looks like tearing.
 *
 * The driver makes the single-buffer path the right one anyway: it declares
 * .fb_create = drm_gem_fb_create_with_dirty, so a damage report is turned into
 * a plane update, and it stages into its own compositor buffers on every
 * commit - so the double buffering that matters is already happening one layer
 * down. One dumb buffer, written in place, with n31_drmfb_present() to say
 * what changed, gives one compositor kick per frame and leaves both apps'
 * drawing exactly as it was.
 *
 * Nothing here is LVGL. display.h is the LVGL equivalent and the two do not
 * share code, because they share no shape: one hands back an lv_display_t and
 * the other hands back a pointer to pixels.
 */

#ifndef N31_DRMFB_H
#define N31_DRMFB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int       fd;           /* the card, or -1 when this is not open */

    uint32_t *pixels;       /* the mapping, as XRGB8888 */
    size_t    map_len;
    unsigned  w, h;
    unsigned  stride_px;    /* row pitch in PIXELS, which is not always w */

    /* What has to be given back on the way out. */
    uint32_t  fb_id;
    uint32_t  handle;
    uint32_t  crtc_id;
    uint32_t  conn_id;
    void     *saved_crtc;   /* drmModeCrtc *, opaque so this header stays clean */
    void     *mode;         /* drmModeModeInfo *, kept so the mode can be re-set */

    /* How a finished frame is announced - see the comment in n31_drmfb_present. */
    int       use_flip;     /* page flips, until one proves they do not work */
    int       flip_pending; /* a flip is in the air and its event is not read */
} n31_drmfb;

/*
 * Open the card, take a dumb buffer the size of the panel, and show it.
 *
 * Returns false when there is no DRM device, when it cannot be driven, or
 * when N31_DISPLAY=fbdev asks for the old path - in every case the caller
 * should fall back to /dev/fb0 rather than treat it as fatal. A kernel
 * without this driver must not mean a black screen.
 *
 * *s is left zeroed with fd == -1 on failure.
 */
bool n31_drmfb_open(n31_drmfb *s);

/* Give the mode back the way it was found, unmap, and close. */
void n31_drmfb_close(n31_drmfb *s);

/*
 * Say that the picture changed, which is what actually puts it on the panel.
 *
 * The whole surface. A rectangle would be less work for the compositor, but
 * both callers already redraw most of the screen every frame and neither
 * tracks a bounding box it could hand over honestly - and a damage rectangle
 * that is smaller than the truth is a display with stale pixels in it, which
 * is a far worse bug than a slightly larger copy.
 */
void n31_drmfb_present(n31_drmfb *s);

/* "DRM 240x432 /dev/dri/card0", for the line an app prints at startup. */
const char *n31_drmfb_describe(const n31_drmfb *s);

#endif /* N31_DRMFB_H */

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
 * WHY ONE BUFFER, NOT TWO
 *
 * Double buffering is the usual answer and is wrong for these two. Both draw
 * INCREMENTALLY - TinyGB's scaler skips rows that did not change between
 * frames, and DOOM's status bar is left alone for most of a tick - and with
 * two buffers each is a frame stale, so "unchanged since last frame" becomes
 * false and every skip-optimisation turns into a bug that looks like tearing.
 * One buffer, written in place, keeps both apps' drawing exactly as it was;
 * the tear that costs is real and accepted.
 *
 * WHY ATOMIC AND NOT THE LEGACY CALLS
 *
 * This started on drmModeSetCrtc plus a DIRTYFB damage hint, on the reasoning
 * that the driver declares .fb_create = drm_gem_fb_create_with_dirty and so
 * turns damage into a plane update. Both TinyGB and DOOM were black anyway,
 * for a whole evening, with their sound playing.
 *
 * The bench that proved this driver fast - tools/linux-n31/drmtest.c, 120
 * commits and 120 flip events at sixty-four frames a second - sets the
 * universal-planes and atomic client caps and drives the panel with
 * DRM_IOCTL_MODE_ATOMIC. It never calls SetCrtc, PageFlip or DIRTYFB. That is
 * the one path on this driver with evidence behind it, and the legacy calls
 * were the part chosen by reasoning. So this now does what the bench does:
 * same caps, same properties, same commit, one buffer instead of two.
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

/* The plane properties a commit has to carry. Named rather than an array so
   a missing one is a compile error somewhere useful. */
typedef struct {
    uint32_t fb_id, crtc_id;
    uint32_t src_x, src_y, src_w, src_h;
    uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
} n31_drmfb_planeprops;

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
    uint32_t  plane_id;
    uint32_t  mode_blob;
    void     *saved_crtc;   /* drmModeCrtc *, opaque so this header stays clean */

    /* Property ids, looked up once. */
    n31_drmfb_planeprops plane;
    uint32_t  p_crtc_active;
    uint32_t  p_crtc_mode;
    uint32_t  p_conn_crtc;

    int       modeset_done; /* the first commit carries ALLOW_MODESET */
    int       flip_pending; /* a commit is in the air, its event unread */
    int       failures;     /* consecutive refused commits */
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
 * Put the picture on the panel.
 *
 * One atomic commit of the primary plane, with a flip event asked for and
 * waited on - which is what paces the caller to the panel. Not optional and
 * not a hint: on DRM this call IS the frame.
 */
void n31_drmfb_present(n31_drmfb *s);

/* "DRM 240x432 /dev/dri/card0", for the line an app prints at startup. */
const char *n31_drmfb_describe(const n31_drmfb *s);

#endif /* N31_DRMFB_H */

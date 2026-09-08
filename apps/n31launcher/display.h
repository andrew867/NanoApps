/*
 * display.h — open the screen the fast way, and fall back to the slow one.
 *
 * WHICH APPS NEED THIS
 *
 * The display driver is a real DRM/KMS driver and its fbdev emulation is a
 * courtesy, not the intended interface. Measured on the device: 150 repaints
 * through /dev/fb0 produced 75 compositor kicks, because fb_deferred_io
 * coalesces damage on a timer and FBIOPUT_VSCREENINFO cannot un-coalesce it.
 * The same sequence through /dev/dri/card0 ran 120 commits and 120 flip
 * events with no errors at 64 fps - one kick per commit.
 *
 * So an app that redraws continuously - Radio+ with its signal meter, TinyPod
 * while a track plays - should be a DRM client, and it is worth the 114 KB of
 * static libdrm to make it one.
 *
 * The LAUNCHER deliberately does not use this. It draws a static home screen a
 * few times and then stops, so a twenty-frame ceiling costs it nothing, and
 * staying on fbdev keeps it out of the way of the app it is about to start:
 * DRM has exactly one master, LVGL takes it implicitly by opening the node,
 * and a launcher holding it would leave every app it launched unable to
 * modeset. The fbdev emulation is an in-kernel client, so the kernel preempts
 * it when an app takes over and restores it when the app exits - which is the
 * behaviour we want and is already what happens on any Linux box running a
 * console and a DRM application.
 *
 * FALLING BACK IS NOT OPTIONAL
 *
 * A kernel without the DRM driver, or a device node that is not there yet
 * because the driver probed late, must not mean a black screen - the fbdev
 * path still works, just slowly. So this tries DRM and falls back, and says
 * which one it got, because "the radio is slow" and "the radio is on the
 * fallback path" are the same sentence and only one of them is actionable.
 *
 * N31_DISPLAY=fbdev forces the old path, for comparing the two on one device.
 */

#ifndef N31_DISPLAY_H
#define N31_DISPLAY_H

#include "lvgl/lvgl.h"

typedef enum {
    N31_DISPLAY_NONE = 0,
    N31_DISPLAY_DRM,
    N31_DISPLAY_FBDEV
} n31_display_kind_t;

/*
 * Create the display, DRM first.
 *
 * `fb` is the framebuffer device for the fallback, or NULL for /dev/fb0.
 * `w` and `h` are the panel size, which is set on whichever path is taken.
 *
 * Returns NULL only when both failed, which is a machine with no screen at
 * all. `kind` is filled in with what was actually opened, and may be NULL.
 */
lv_display_t *n31_display_create(const char *fb, int32_t w, int32_t h,
                                 n31_display_kind_t *kind);

/* "DRM /dev/dri/card0" or "fbdev /dev/fb0", for the line an app prints at
   startup and for its settings screen. Valid until the next call. */
const char *n31_display_describe(void);

#endif /* N31_DISPLAY_H */

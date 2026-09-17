/*
 * fbrefresh.h — one switch, so the same experiment can be run in every app.
 *
 * WHAT THIS IS FOR
 *
 * The display driver is a DRM driver now, and /dev/fb0 is drm_fbdev_shmem's
 * emulation of one: a shadow buffer whose contents reach the panel when
 * something reports damage. LVGL's framebuffer driver reports none. It mmaps
 * MAP_SHARED, writes into the mapping, and relies entirely on fb_deferred_io
 * noticing through the page faults that writing causes - there is no msync and
 * no damage ioctl anywhere in it. TinyGB writes its frames the same way.
 *
 * That is fine for a screen that is redrawn occasionally and is the open
 * question for one redrawn sixty times a second, which is exactly the split
 * observed: the launcher draws a static home screen a few times and works,
 * and every app that draws continuously shows nothing.
 *
 * Setting this makes each app ask for the flush explicitly after every frame -
 * FBIOPUT_VSCREENINFO, which is what LVGL's own force-refresh does and what
 * drm_fb_helper turns into a full-screen damage. So:
 *
 *   the picture comes back    the writes were not producing damage
 *   nothing changes           damage was arriving and the kick was dropped
 *
 * Either answer is worth having and neither needs a rebuild to get.
 *
 *   N31_FB_FORCE_REFRESH=1 n31launcher
 *
 * Off by default, because it costs an ioctl per frame and because if it turns
 * out to be needed the right place to fix it is the driver, not four apps.
 */

#ifndef N31_FBREFRESH_H
#define N31_FBREFRESH_H

#include <stdbool.h>
#include <stdlib.h>

/*
 * Inline and header-only on purpose: four apps in three build systems want
 * this and none of them should need a new object file to ask a question about
 * an environment variable. What matters is that the NAME lives in one place.
 */
static inline bool n31_fb_force_refresh(void)
{
    const char *e = getenv("N31_FB_FORCE_REFRESH");

    /* "0" is off, so the variable can be left set and turned off in place. */
    return e && *e && *e != '0';
}

#endif /* N31_FBREFRESH_H */

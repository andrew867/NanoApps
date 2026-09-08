/*
 * display.c — see display.h.
 */

#include "display.h"
#include "fbrefresh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_desc[96];

static int forced_fbdev(void)
{
    const char *e = getenv("N31_DISPLAY");

    return e && (strcmp(e, "fbdev") == 0 || strcmp(e, "fb") == 0);
}

/*
 * The DRM node.
 *
 * card0 by name rather than through LVGL's own scan, because this machine has
 * exactly one and the scan allocates a string this would then have to free on
 * every path out. N31_DRM_CARD overrides it for a device where the numbering
 * moved.
 */
static const char *drm_card(void)
{
    const char *e = getenv("N31_DRM_CARD");

    return (e && *e) ? e : "/dev/dri/card0";
}

lv_display_t *n31_display_create(const char *fb, int32_t w, int32_t h,
                                 n31_display_kind_t *kind)
{
    lv_display_t *disp;

    if (kind) *kind = N31_DISPLAY_NONE;
    if (!fb || !*fb) fb = "/dev/fb0";

#if LV_USE_LINUX_DRM
    if (!forced_fbdev() && access(drm_card(), R_OK | W_OK) == 0) {
        disp = lv_linux_drm_create();
        if (disp) {
            /*
             * -1 is "the first connector that is connected", which on a device
             * with one panel soldered to it is the only answer there is.
             */
            if (lv_linux_drm_set_file(disp, drm_card(), -1) == LV_RESULT_OK) {
                lv_display_set_resolution(disp, w, h);
                snprintf(s_desc, sizeof s_desc, "DRM %s", drm_card());
                if (kind) *kind = N31_DISPLAY_DRM;
                return disp;
            }

            /*
             * Opened the node and could not drive it. Delete rather than
             * leak: LVGL closes the descriptor on LV_EVENT_DELETE, and
             * leaving it open would keep DRM master and stop the fbdev
             * fallback below from ever reaching the panel.
             */
            lv_display_delete(disp);
        }
        fprintf(stderr, "n31: %s would not drive the panel; using fbdev\n",
                drm_card());
    }
#endif

    disp = lv_linux_fbdev_create();
    if (!disp)
        return NULL;

    lv_linux_fbdev_set_file(disp, fb);
    lv_display_set_resolution(disp, w, h);

    /*
     * The fbdev path is the one capped by fb_deferred_io's timer, so this is
     * where the force-refresh switch still means something. It buys nothing -
     * measured - but it costs nothing to leave reachable on the path it was
     * written for.
     */
    if (n31_fb_force_refresh())
        lv_linux_fbdev_set_force_refresh(disp, true);

    snprintf(s_desc, sizeof s_desc, "fbdev %s%s", fb,
             n31_fb_force_refresh() ? " (forced refresh)" : "");
    if (kind) *kind = N31_DISPLAY_FBDEV;
    return disp;
}

const char *n31_display_describe(void)
{
    return s_desc[0] ? s_desc : "no display";
}

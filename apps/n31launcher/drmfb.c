/*
 * drmfb.c — see drmfb.h.
 */

#include "drmfb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static char s_desc[96];

static const char *card_path(void)
{
    const char *e = getenv("N31_DRM_CARD");

    return (e && *e) ? e : "/dev/dri/card0";
}

static bool forced_fbdev(void)
{
    const char *e = getenv("N31_DISPLAY");

    return e && (strcmp(e, "fbdev") == 0 || strcmp(e, "fb") == 0);
}

/*
 * The first connector that has something on the end of it, and a mode to
 * drive it with.
 *
 * "First connected" rather than a preferred-flag search: this panel is
 * soldered to the board and is the only connector the driver registers, so a
 * cleverer choice would be choosing between one thing.
 */
static bool pick_output(int fd, drmModeRes *res, uint32_t *conn_id,
                        uint32_t *crtc_id, drmModeModeInfo *mode)
{
    int i;

    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        drmModeEncoder *enc;

        if (!c)
            continue;
        if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
            drmModeFreeConnector(c);
            continue;
        }

        *conn_id = c->connector_id;
        *mode = c->modes[0];

        /* The encoder it is already using, if it has one - otherwise the
           first CRTC the connector says it can be routed to. */
        enc = c->encoder_id ? drmModeGetEncoder(fd, c->encoder_id) : NULL;
        if (enc && enc->crtc_id) {
            *crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
            drmModeFreeConnector(c);
            return true;
        }
        if (enc)
            drmModeFreeEncoder(enc);

        if (res->count_crtcs > 0) {
            *crtc_id = res->crtcs[0];
            drmModeFreeConnector(c);
            return true;
        }
        drmModeFreeConnector(c);
    }
    return false;
}

bool n31_drmfb_open(n31_drmfb *s)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    drmModeModeInfo mode;
    drmModeRes *res = NULL;
    uint64_t has_dumb = 0;

    if (!s)
        return false;

    memset(s, 0, sizeof *s);
    s->fd = -1;

    if (forced_fbdev())
        return false;

    s->fd = open(card_path(), O_RDWR | O_CLOEXEC);
    if (s->fd < 0)
        return false;

    /*
     * Dumb buffers are the whole basis of this: no GEM allocator of our own,
     * no GBM, just memory the kernel maps for us. A driver without them is one
     * this cannot drive, and saying so here beats failing later.
     */
    if (drmGetCap(s->fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb)
        goto fail;

    res = drmModeGetResources(s->fd);
    if (!res)
        goto fail;

    if (!pick_output(s->fd, res, &s->conn_id, &s->crtc_id, &mode))
        goto fail;

    /* Kept so the console gets its mode back when this exits. */
    s->saved_crtc = drmModeGetCrtc(s->fd, s->crtc_id);

    memset(&creq, 0, sizeof creq);
    creq.width = mode.hdisplay;
    creq.height = mode.vdisplay;
    creq.bpp = 32;
    if (drmIoctl(s->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        goto fail;

    s->handle = creq.handle;
    s->w = creq.width;
    s->h = creq.height;
    s->stride_px = creq.pitch / 4;
    s->map_len = creq.size;

    /* depth 24 in a 32-bit pixel, which is XRGB8888 - the only format this
       driver's primary plane advertises. */
    if (drmModeAddFB(s->fd, s->w, s->h, 24, 32, creq.pitch, s->handle,
                     &s->fb_id) != 0)
        goto fail;

    memset(&mreq, 0, sizeof mreq);
    mreq.handle = s->handle;
    if (drmIoctl(s->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        goto fail;

    s->pixels = mmap(NULL, s->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                     s->fd, (off_t)mreq.offset);
    if (s->pixels == MAP_FAILED) {
        s->pixels = NULL;
        goto fail;
    }

    memset(s->pixels, 0, s->map_len);

    /*
     * Show it. This is the one modeset - everything after it is damage on a
     * surface that is already on screen, which is why there is no flip loop
     * and nothing to wait for.
     */
    if (drmModeSetCrtc(s->fd, s->crtc_id, s->fb_id, 0, 0,
                       &s->conn_id, 1, &mode) != 0)
        goto fail;

    drmModeFreeResources(res);
    snprintf(s_desc, sizeof s_desc, "DRM %ux%u %s", s->w, s->h, card_path());
    return true;

fail:
    if (res)
        drmModeFreeResources(res);
    n31_drmfb_close(s);
    return false;
}

void n31_drmfb_present(n31_drmfb *s)
{
    if (!s || s->fd < 0 || !s->fb_id)
        return;

    /*
     * NULL clips means the whole framebuffer, which is what the driver's
     * drm_gem_fb_create_with_dirty turns into a plane update. Its return is
     * ignored on purpose: a driver that does not implement dirty still shows
     * the surface, just no sooner than it would have anyway, and an app that
     * stopped drawing because a damage hint was refused would be worse than
     * one that draws to a display which is merely late.
     */
    (void)drmModeDirtyFB(s->fd, s->fb_id, NULL, 0);
}

void n31_drmfb_close(n31_drmfb *s)
{
    struct drm_mode_destroy_dumb dreq;

    if (!s || s->fd < 0)
        return;

    /* The mode as it was found, so whatever had the screen before this gets
       it back rather than a blank CRTC. */
    if (s->saved_crtc) {
        drmModeCrtc *c = s->saved_crtc;

        if (c->mode_valid)
            drmModeSetCrtc(s->fd, c->crtc_id, c->buffer_id, c->x, c->y,
                           &s->conn_id, 1, &c->mode);
        drmModeFreeCrtc(c);
        s->saved_crtc = NULL;
    }

    if (s->pixels) {
        munmap(s->pixels, s->map_len);
        s->pixels = NULL;
    }
    if (s->fb_id) {
        drmModeRmFB(s->fd, s->fb_id);
        s->fb_id = 0;
    }
    if (s->handle) {
        memset(&dreq, 0, sizeof dreq);
        dreq.handle = s->handle;
        drmIoctl(s->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        s->handle = 0;
    }

    close(s->fd);
    s->fd = -1;
}

const char *n31_drmfb_describe(const n31_drmfb *s)
{
    if (!s || s->fd < 0)
        return "no DRM display";
    return s_desc[0] ? s_desc : "DRM";
}

/*
 * drmfb.c — see drmfb.h.
 */

#include "drmfb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* How long a flip is given to complete before flips are written off. Two
   frames at the slowest rate this panel is ever driven at. */
#define FLIP_WAIT_MS 100

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
     * Show it - and keep the mode, because this will have to be done again.
     *
     * It was written as the one modeset, on the reasoning that everything
     * after it is damage on a surface already on screen. That reasoning holds
     * only while nothing else commits to this CRTC, and on this device
     * something always does: /dev/fb0 is the driver's own emulation, a real
     * DRM client in the kernel, and fbcon blinking a cursor through it is
     * enough to put its framebuffer back on the plane. Ours is then attached
     * to nothing, DIRTYFB has no plane to damage, and the app draws sixty
     * frames a second into memory that is not being scanned out - a black
     * screen with the sound still playing, which is exactly the symptom this
     * was reported as.
     */
    if (drmModeSetCrtc(s->fd, s->crtc_id, s->fb_id, 0, 0,
                       &s->conn_id, 1, &mode) != 0)
        goto fail;

    s->mode = malloc(sizeof mode);
    if (s->mode)
        memcpy(s->mode, &mode, sizeof mode);

    /* Flips until proven otherwise; see n31_drmfb_present. */
    s->use_flip = 1;
    s->flip_pending = 0;

    drmModeFreeResources(res);
    snprintf(s_desc, sizeof s_desc, "DRM %ux%u %s", s->w, s->h, card_path());
    return true;

fail:
    if (res)
        drmModeFreeResources(res);
    n31_drmfb_close(s);
    return false;
}

/*
 * Is our framebuffer still the one being scanned out?
 *
 * One ioctl, asked once a frame. That is a real cost and it buys the
 * difference between a display and a black screen, because the alternative -
 * assuming the modeset holds - is only true on a device where nothing else
 * ever touches the CRTC, and this is not one.
 */
static bool still_ours(n31_drmfb *s)
{
    drmModeCrtc *c = drmModeGetCrtc(s->fd, s->crtc_id);
    bool ours;

    if (!c)
        return true;    /* Cannot tell; assume yes rather than fight it. */

    ours = (c->buffer_id == s->fb_id);
    drmModeFreeCrtc(c);
    return ours;
}

static void flip_done(int fd, unsigned seq, unsigned sec, unsigned usec,
                      void *data)
{
    n31_drmfb *s = data;

    (void)fd; (void)seq; (void)sec; (void)usec;
    if (s)
        s->flip_pending = 0;
}

/*
 * Wait for the flip we already asked for.
 *
 * Bounded, because a display that stops answering must not take the app down
 * with it. A timeout is taken as proof that flips do not work here and the
 * damage hint becomes the way frames are announced from then on - a slower
 * display beats a frozen one.
 */
static void wait_flip(n31_drmfb *s)
{
    drmEventContext ctx;
    struct pollfd pfd;

    if (!s->flip_pending)
        return;

    pfd.fd = s->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, FLIP_WAIT_MS) <= 0) {
        s->flip_pending = 0;
        s->use_flip = 0;
        return;
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.version = 2;
    ctx.page_flip_handler = flip_done;
    if (drmHandleEvent(s->fd, &ctx) != 0)
        s->flip_pending = 0;
}

void n31_drmfb_present(n31_drmfb *s)
{
    if (!s || s->fd < 0 || !s->fb_id)
        return;

    /*
     * Take the plane back first if something else has it.
     *
     * Only when it has actually been lost: a modeset per frame would be a
     * full mode change sixty times a second, and on this panel that is
     * visible. Losing it is rare - it takes another client committing - so
     * the check is what runs every frame and the repair almost never does.
     */
    if (s->mode && !still_ours(s)) {
        drmModeSetCrtc(s->fd, s->crtc_id, s->fb_id, 0, 0, &s->conn_id, 1,
                       (drmModeModeInfo *)s->mode);
        s->flip_pending = 0;    /* whatever was in the air went with the mode */
    }

    /*
     * A page flip, which is what this driver was actually measured doing.
     *
     * The first version of this announced frames with DRM_IOCTL_MODE_DIRTYFB
     * and nothing appeared. The bench that proved the driver fast - 120
     * commits, 120 flip events, sixty-four frames a second - drove it with
     * atomic commits and flip events, and that is the path with the evidence
     * behind it. Flipping to the buffer that is already scanned out is a real
     * commit to the plane, which is the part that matters; there is one
     * buffer here and drawing into it while it is on screen is a tear this
     * accepts, exactly as the damage-hint version did.
     *
     * The event is waited for rather than left in the queue, because a second
     * flip on top of an unfinished one is refused - and waiting for it paces
     * the caller to the panel, which is a better way to spend the time than
     * spinning ahead of it.
     */
    if (s->use_flip) {
        wait_flip(s);

        if (s->use_flip &&
            drmModePageFlip(s->fd, s->crtc_id, s->fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, s) == 0) {
            s->flip_pending = 1;
            return;
        }

        /* Refused. Fall through, and stop asking. */
        s->use_flip = 0;
    }

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

    /* Do not tear the buffer out from under a flip that has not landed. */
    if (s->flip_pending)
        wait_flip(s);

    free(s->mode);
    s->mode = NULL;

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

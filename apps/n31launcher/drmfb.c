/*
 * drmfb.c — see drmfb.h.
 */

#include "drmfb.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

/* How long a commit is given to report back. Generous: this is a bound on a
   pathology, not a frame budget. */
#define FLIP_WAIT_MS 200

/* Commits refused in a row before the surface declares itself unusable. One
   is a glitch; a run of them means the caller is drawing into nothing and
   should be told rather than left looking at a black screen. */
#define FAIL_LIMIT 30

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

/* ---- properties ----------------------------------------------------------- */

/*
 * The id of a named property on an object, and optionally its current value.
 *
 * Property ids are not fixed by the ABI - they are whatever the driver
 * happened to register - so everything here is looked up by name once at
 * open and then used by id.
 */
static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name,
                        uint64_t *value)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t found = 0;
    uint32_t i;

    if (!props)
        return 0;

    for (i = 0; i < props->count_props && !found; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);

        if (!p)
            continue;
        if (strcmp(p->name, name) == 0) {
            found = p->prop_id;
            if (value)
                *value = props->prop_values[i];
        }
        drmModeFreeProperty(p);
    }

    drmModeFreeObjectProperties(props);
    return found;
}

static bool find_plane_props(n31_drmfb *s)
{
    struct { const char *name; uint32_t *out; } want[] = {
        { "FB_ID",   &s->plane.fb_id   },
        { "CRTC_ID", &s->plane.crtc_id },
        { "SRC_X",   &s->plane.src_x   },
        { "SRC_Y",   &s->plane.src_y   },
        { "SRC_W",   &s->plane.src_w   },
        { "SRC_H",   &s->plane.src_h   },
        { "CRTC_X",  &s->plane.crtc_x  },
        { "CRTC_Y",  &s->plane.crtc_y  },
        { "CRTC_W",  &s->plane.crtc_w  },
        { "CRTC_H",  &s->plane.crtc_h  },
    };
    unsigned i;

    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        *want[i].out = prop_id(s->fd, s->plane_id, DRM_MODE_OBJECT_PLANE,
                               want[i].name, NULL);
        if (!*want[i].out) {
            fprintf(stderr, "drmfb: the primary plane has no %s\n",
                    want[i].name);
            return false;
        }
    }
    return true;
}

/*
 * The primary plane that can drive our CRTC.
 *
 * possible_crtcs is a bitmask over the CRTC list in index order, not over
 * ids, which is why the index is carried down here rather than the id.
 */
static uint32_t pick_plane(int fd, unsigned crtc_index)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    uint32_t chosen = 0;
    uint32_t i;

    if (!pr)
        return 0;

    for (i = 0; i < pr->count_planes && !chosen; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[i]);
        uint64_t type = 0;

        if (!pl)
            continue;
        if ((pl->possible_crtcs & (1u << crtc_index)) &&
            prop_id(fd, pr->planes[i], DRM_MODE_OBJECT_PLANE, "type", &type) &&
            type == DRM_PLANE_TYPE_PRIMARY)
            chosen = pr->planes[i];
        drmModeFreePlane(pl);
    }

    drmModeFreePlaneResources(pr);
    return chosen;
}

/*
 * The first connector that has something on the end of it, and a mode to
 * drive it with, and the CRTC to drive it from.
 *
 * "First connected" rather than a preferred-flag search: this panel is
 * soldered to the board and is the only connector the driver registers, so a
 * cleverer choice would be choosing between one thing.
 */
static bool pick_output(int fd, drmModeRes *res, n31_drmfb *s,
                        drmModeModeInfo *mode, unsigned *crtc_index)
{
    int i;

    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        drmModeEncoder *enc;
        uint32_t crtc = 0;
        int k;

        if (!c)
            continue;
        if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
            drmModeFreeConnector(c);
            continue;
        }

        s->conn_id = c->connector_id;
        *mode = c->modes[0];

        /* The encoder it is already using, if it has one - otherwise the
           first CRTC the card offers. */
        enc = c->encoder_id ? drmModeGetEncoder(fd, c->encoder_id) : NULL;
        if (enc && enc->crtc_id)
            crtc = enc->crtc_id;
        if (enc)
            drmModeFreeEncoder(enc);
        if (!crtc && res->count_crtcs > 0)
            crtc = res->crtcs[0];
        drmModeFreeConnector(c);

        if (!crtc)
            return false;

        for (k = 0; k < res->count_crtcs; k++) {
            if (res->crtcs[k] == crtc) {
                s->crtc_id = crtc;
                *crtc_index = (unsigned)k;
                return true;
            }
        }
        return false;   /* a CRTC that is not in the CRTC list */
    }
    return false;
}

/* ---- the commit ----------------------------------------------------------- */

static void flip_done(int fd, unsigned seq, unsigned sec, unsigned usec,
                      void *data)
{
    n31_drmfb *s = data;

    (void)fd; (void)seq; (void)sec; (void)usec;
    if (s)
        s->flip_pending = 0;
}

/*
 * Wait for the commit already in the air.
 *
 * Bounded, because a display that stops answering must not take the app down
 * with it - a timeout clears the flag and lets the next commit go, and if the
 * driver really has stopped, FAIL_LIMIT is what ends it.
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
        return;
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.version = 2;
    ctx.page_flip_handler = flip_done;
    if (drmHandleEvent(s->fd, &ctx) != 0)
        s->flip_pending = 0;
}

/*
 * One frame: the plane, pointed at our buffer, covering the CRTC.
 *
 * The first commit also brings the pipeline up - connector routed to the
 * CRTC, CRTC active, mode set from the blob - and is the only one allowed to
 * modeset. Every commit after it changes nothing structurally and is just the
 * plane update that puts the pixels out.
 */
static int commit(n31_drmfb *s, bool modeset)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
    int rc;

    if (!req)
        return -ENOMEM;

    if (modeset) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
        drmModeAtomicAddProperty(req, s->conn_id, s->p_conn_crtc, s->crtc_id);
        drmModeAtomicAddProperty(req, s->crtc_id, s->p_crtc_active, 1);
        drmModeAtomicAddProperty(req, s->crtc_id, s->p_crtc_mode, s->mode_blob);
    }

    drmModeAtomicAddProperty(req, s->plane_id, s->plane.fb_id, s->fb_id);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.crtc_id, s->crtc_id);
    /* SRC_* are 16.16 fixed point; CRTC_* are plain pixels. */
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.src_x, 0);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.src_y, 0);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.src_w,
                             (uint64_t)s->w << 16);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.src_h,
                             (uint64_t)s->h << 16);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.crtc_x, 0);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.crtc_y, 0);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.crtc_w, s->w);
    drmModeAtomicAddProperty(req, s->plane_id, s->plane.crtc_h, s->h);

    rc = drmModeAtomicCommit(s->fd, req, flags, s);
    drmModeAtomicFree(req);
    return rc;
}

/* ---- open and close ------------------------------------------------------- */

bool n31_drmfb_open(n31_drmfb *s)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
    drmModeModeInfo mode;
    drmModeRes *res = NULL;
    unsigned crtc_index = 0;
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
     * Both caps, before anything else is asked of the card.
     *
     * Atomic is what this whole file is built on. Universal planes has to
     * come with it: without it the primary plane is not even enumerated, so
     * there is nothing to point at the buffer.
     */
    if (drmSetClientCap(s->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(s->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        fprintf(stderr, "drmfb: %s does not do atomic modesetting\n",
                card_path());
        goto fail;
    }

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

    if (!pick_output(s->fd, res, s, &mode, &crtc_index))
        goto fail;

    s->plane_id = pick_plane(s->fd, crtc_index);
    if (!s->plane_id) {
        fprintf(stderr, "drmfb: no primary plane for crtc %u\n", s->crtc_id);
        goto fail;
    }
    if (!find_plane_props(s))
        goto fail;

    s->p_crtc_active = prop_id(s->fd, s->crtc_id, DRM_MODE_OBJECT_CRTC,
                               "ACTIVE", NULL);
    s->p_crtc_mode = prop_id(s->fd, s->crtc_id, DRM_MODE_OBJECT_CRTC,
                             "MODE_ID", NULL);
    s->p_conn_crtc = prop_id(s->fd, s->conn_id, DRM_MODE_OBJECT_CONNECTOR,
                             "CRTC_ID", NULL);
    if (!s->p_crtc_active || !s->p_crtc_mode || !s->p_conn_crtc) {
        fprintf(stderr, "drmfb: the pipeline is missing ACTIVE/MODE_ID\n");
        goto fail;
    }

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

    /* ADDFB2 with an explicit fourcc, not the legacy depth/bpp guess -
       XRGB8888 is the only format this driver's primary plane advertises. */
    handles[0] = s->handle;
    pitches[0] = creq.pitch;
    if (drmModeAddFB2(s->fd, s->w, s->h, DRM_FORMAT_XRGB8888, handles,
                      pitches, offsets, &s->fb_id, 0) != 0)
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

    if (drmModeCreatePropertyBlob(s->fd, &mode, sizeof mode,
                                  &s->mode_blob) != 0)
        goto fail;

    /* Bring it up, and wait for the pipeline to say it did. */
    if (commit(s, true) != 0) {
        fprintf(stderr, "drmfb: the first commit was refused: %s\n",
                strerror(errno));
        goto fail;
    }
    s->modeset_done = 1;
    s->flip_pending = 1;
    wait_flip(s);

    drmModeFreeResources(res);
    snprintf(s_desc, sizeof s_desc, "DRM %ux%u %s atomic", s->w, s->h,
             card_path());
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
     * The previous frame first. A commit on top of one that has not landed is
     * refused with EBUSY, and waiting here is also what paces the caller to
     * the panel rather than letting it spin ahead of the display.
     */
    wait_flip(s);

    if (commit(s, false) == 0) {
        s->flip_pending = 1;
        s->failures = 0;
        return;
    }

    /*
     * Refused. Say so once and keep going: one is a glitch, and the caller
     * has no better option than to draw the next frame anyway. A run of them
     * means the pixels are going nowhere, which is worth saying out loud -
     * silence here is what made this hard to find the first time.
     */
    if (++s->failures == FAIL_LIMIT)
        fprintf(stderr, "drmfb: %d commits refused in a row (%s); "
                        "the panel is not being updated\n",
                s->failures, strerror(errno));
}

void n31_drmfb_close(n31_drmfb *s)
{
    struct drm_mode_destroy_dumb dreq;

    if (!s || s->fd < 0)
        return;

    /* Do not tear the buffer out from under a commit that has not landed. */
    wait_flip(s);

    if (s->mode_blob) {
        drmModeDestroyPropertyBlob(s->fd, s->mode_blob);
        s->mode_blob = 0;
    }

    /* The mode as it was found, so whatever had the screen before this gets
       it back rather than a blank CRTC. Legacy on purpose: this is the call
       the fbdev emulation understands as "you have it back". */
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
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

const char *n31_drmfb_describe(const n31_drmfb *s)
{
    if (!s || s->fd < 0)
        return "no DRM display";
    return s_desc[0] ? s_desc : "DRM";
}

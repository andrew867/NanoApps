/*
 * tg_fb_linux.c — see tg_fb.h.
 */

#include "tg_fb.h"

#include "fbrefresh.h"
#include "tg_scale.h"
#include "../../n31launcher/fbcon.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * The DRM path, tried first.
 *
 * /dev/fb0 still works and is still here, but it is the driver's emulation of
 * itself and fb_deferred_io caps it at about twenty frames a second whatever
 * the app does - which for something running a Game Boy at sixty is the whole
 * ballgame. On the DRM device a finished frame is announced rather than
 * noticed.
 */
static bool open_drm(tg_fb *fb)
{
    memset(fb, 0, sizeof *fb);
    fb->fd = -1;
    fb->drm.fd = -1;

    if (!n31_drmfb_open(&fb->drm))
        return false;

    fb->pixels    = fb->drm.pixels;
    fb->map_len   = fb->drm.map_len;
    fb->w         = fb->drm.w;
    fb->h         = fb->drm.h;
    fb->stride_px = fb->drm.stride_px;
    fb->bpp       = 32;

    if (fb->w < TG_SCALED_W || fb->h < TG_SCALED_H) {
        fprintf(stderr, "tinygb: %s is %ux%u, need at least %ux%u\n",
                n31_drmfb_describe(&fb->drm), fb->w, fb->h,
                TG_SCALED_W, TG_SCALED_H);
        n31_drmfb_close(&fb->drm);
        return false;
    }

    printf("tinygb: display %s\n", n31_drmfb_describe(&fb->drm));
    return true;
}

bool tg_fb_open(tg_fb *fb, const char *path)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    /*
     * DRM first, fbdev if there is no DRM device or the caller asked for the
     * old path with N31_DISPLAY=fbdev. The console still has to be detached
     * either way, and open_drm does not do it - the DRM path takes the CRTC
     * from fbcon by setting a mode, but fbcon is still free to draw into its
     * own surface underneath and gets it back on the way out.
     */
    if (open_drm(fb)) {
        fb->took_console = n31_fbcon_detach();
        return true;
    }

    memset(fb, 0, sizeof *fb);
    fb->fd = -1;
    fb->drm.fd = -1;

    if (!path) path = "/dev/fb0";

    /*
     * Take the console first, and only then open the device.
     *
     * The other order works right up until it does not: fbcon can be mid-blit
     * when the mapping appears, and the first frame gets a band of kernel log
     * baked into it that nothing ever redraws over. Detaching first means the
     * console has stopped before there is anything to corrupt.
     *
     * If it cannot be detached the app still runs - a picture with dmesg drawn
     * through it is better than no picture, and on a serial console this is a
     * non-issue anyway.
     */
    fb->took_console = n31_fbcon_detach();

    if ((fb->fd = open(path, O_RDWR)) < 0) {
        fprintf(stderr, "tinygb: cannot open %s\n", path);
        goto fail;
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        fprintf(stderr, "tinygb: %s does not answer the framebuffer ioctls\n",
                path);
        goto fail;
    }

    fb->w         = var.xres;
    fb->h         = var.yres;
    fb->bpp       = var.bits_per_pixel;
    fb->stride_px = fix.line_length / 4;

    if (fb->bpp != 32) {
        /* 16bpp would need a second scaler and a second pixel format, and this
           panel is not 16bpp. Say so plainly rather than draw garbage. */
        fprintf(stderr, "tinygb: %s is %u bpp; this build needs 32\n",
                path, fb->bpp);
        goto fail;
    }

    if (fb->w < TG_SCALED_W || fb->h < TG_SCALED_H) {
        fprintf(stderr, "tinygb: %s is %ux%u, too small for %dx%d\n",
                path, fb->w, fb->h, TG_SCALED_W, TG_SCALED_H);
        goto fail;
    }

    /*
     * Map the whole reported buffer, not width*height*4. On this panel
     * line_length is 960 for 240 visible pixels, so a row is 960 bytes and the
     * last row starts 431 of those in - computing the length from the visible
     * width would come up short and the final rows would fault.
     */
    fb->map_len = (size_t)fix.line_length * fb->h;
    if (fb->map_len < (size_t)fix.smem_len) fb->map_len = fix.smem_len;

    fb->pixels = mmap(NULL, fb->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fb->fd, 0);
    if (fb->pixels == MAP_FAILED) {
        fb->pixels = NULL;
        fprintf(stderr, "tinygb: cannot map %s\n", path);
        goto fail;
    }

    return true;

fail:
    if (fb->fd >= 0) close(fb->fd);
    if (fb->took_console) n31_fbcon_restore();
    memset(fb, 0, sizeof *fb);
    fb->fd = -1;
    return false;
}

void tg_fb_close(tg_fb *fb)
{
    if (!fb) return;

    /* Leave a black screen rather than the last frame: the console is coming
       back and a half-drawn Game Boy underneath a shell prompt is confusing. */
    if (fb->pixels) {
        tg_fb_fill(fb, 0x000000);
        tg_fb_flush(fb);
        /* The DRM mapping belongs to n31_drmfb and is unmapped by it, along
           with the buffer and the mode it took. Unmapping it here as well
           would be unmapping it twice. */
        if (fb->drm.fd < 0)
            munmap(fb->pixels, fb->map_len);
    }
    if (fb->drm.fd >= 0) n31_drmfb_close(&fb->drm);
    if (fb->fd >= 0) close(fb->fd);
    if (fb->took_console) n31_fbcon_restore();

    memset(fb, 0, sizeof *fb);
    fb->fd = -1;
}

void tg_fb_fill(tg_fb *fb, uint32_t rgb)
{
    if (!fb->pixels) return;

    for (unsigned y = 0; y < fb->h; y++) {
        uint32_t *row = fb->pixels + (size_t)y * fb->stride_px;

        for (unsigned x = 0; x < fb->w; x++) row[x] = rgb;
    }
}

void tg_fb_layout(const tg_fb *fb, unsigned *x, unsigned *y)
{
    /* Centred horizontally, which on a 240-wide panel is exactly flush, and
       hard against the top so the controls get the whole remaining half. */
    if (x) *x = (fb->w - TG_SCALED_W) / 2;
    if (y) *y = 0;
}

void tg_fb_flush(tg_fb *fb)
{
    struct fb_var_screeninfo var;

    if (!fb)
        return;

    /*
     * On DRM this is the call that puts the frame on the panel, so it is not
     * optional there and is not gated on anything - see drmfb.h. On fbdev it
     * is the force-refresh experiment, which measured as doing nothing, and
     * stays gated behind the switch it was written for.
     */
    if (fb->drm.fd >= 0) {
        n31_drmfb_present(&fb->drm);
        return;
    }

    if (fb->fd < 0)
        return;

    /*
     * The fbdev half is the force-refresh experiment and nothing more. It
     * measured as doing nothing - fb_deferred_io has already coalesced the
     * damage by the time this runs - so it stays behind the switch it was
     * written for rather than costing two ioctls a frame for everyone.
     */
    if (!n31_fb_force_refresh())
        return;

    /*
     * Read it back before writing it, rather than keeping a copy in tg_fb.
     *
     * Two ioctls a frame instead of one, and worth it: putting a
     * fb_var_screeninfo in the struct would put <linux/fb.h> in tg_fb.h and
     * therefore in every file that draws, and the kernel headers on this
     * toolchain are exactly what the hand-rolled ioctls elsewhere in this tree
     * exist to avoid. Nothing is being changed here anyway - the write itself
     * is the point, because it is what drm_fb_helper turns into a full-screen
     * damage.
     */
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0)
        return;

    var.activate = FB_ACTIVATE_NOW;
    (void)ioctl(fb->fd, FBIOPUT_VSCREENINFO, &var);
}

uint32_t *tg_fb_at(const tg_fb *fb, unsigned x, unsigned y)
{
    if (!fb->pixels || x >= fb->w || y >= fb->h) return NULL;
    return fb->pixels + (size_t)y * fb->stride_px + x;
}

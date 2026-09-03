/*
 * tg_linux.c — TinyGB on the N31 Linux port. Phase 01: the picture.
 *
 * Loads a cartridge, runs it, and puts it on /dev/fb0 at 240x216 in the top
 * half of the panel. No sound yet (Phase 02), no controls yet (Phase 03) - so
 * what this proves is the thing worth proving first: that the core runs on the
 * device, at speed, with the picture where it should be.
 *
 *   tinygb <rom.gb> [--fb /dev/fb0] [--frames N] [--sharp] [--bench]
 *
 * Ctrl-C, or --frames, ends it and gives the console back.
 */

#include "../core/tg_core.h"
#include "../platform/tg_fb.h"
#include "../platform/tg_scale.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- time ---------------------------------------------------------------- */

#define NS_PER_S 1000000000LL

static long long now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

static void sleep_ns(long long ns)
{
    struct timespec ts;

    if (ns <= 0) return;
    ts.tv_sec  = (time_t)(ns / NS_PER_S);
    ts.tv_nsec = (long)(ns % NS_PER_S);
    /* Restart across signals rather than return early: a short sleep that gets
       interrupted would silently become a fast frame. */
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/* ---- ctrl-c -------------------------------------------------------------- */

static volatile sig_atomic_t s_stop;

static void on_signal(int sig) { (void)sig; s_stop = 1; }

/* ---- file ---------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    if (!(buf = malloc((size_t)n))) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

/* ---- ------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: tinygb <rom.gb> [options]\n"
        "  --fb PATH     framebuffer device (default /dev/fb0)\n"
        "  --frames N    stop after N frames instead of running until Ctrl-C\n"
        "  --sharp       nearest-neighbour scaling instead of the smooth 1.5x\n"
        "  --bench       run flat out and report frames per second\n");
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *fb_path = NULL;
    unsigned long limit = 0;
    bool smooth = true, bench = false;

    uint8_t *rom = NULL, *sram = NULL;
    void *ctx = NULL;
    size_t rom_len = 0;
    const tg_core *core;
    tg_rom_info info;
    enum tg_result r;
    tg_fb fb;
    tg_scaler scaler;
    const uint32_t *pal;
    unsigned pal_n = 0, ox = 0, oy = 0;
    uint32_t *dst;

    long long frame_ns, started, next, last_report;
    unsigned long frames = 0, frames_at_report = 0;
    int status = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fb") && i + 1 < argc)          fb_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--sharp"))                  smooth = false;
        else if (!strcmp(argv[i], "--bench"))                  bench = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] == '-')                            { usage(); return 2; }
        else if (!rom_path)                                    rom_path = argv[i];
        else                                                   { usage(); return 2; }
    }

    if (!rom_path) { usage(); return 2; }

    if (!(rom = read_file(rom_path, &rom_len))) {
        fprintf(stderr, "tinygb: cannot read %s\n", rom_path);
        return 2;
    }

    if ((r = tg_rom_probe(rom, rom_len, &info)) != TG_OK) {
        fprintf(stderr, "tinygb: %s\n", tg_strerror(r));
        free(rom);
        return 2;
    }

    if (!(core = tg_core_for_rom(rom, rom_len))) {
        fprintf(stderr, "tinygb: no core here can run %s%s\n",
                info.title[0] ? info.title : rom_path,
                info.cgb_only ? " - it needs Game Boy Color" : "");
        free(rom);
        return 2;
    }

    printf("tinygb: %s  [%s]  %s\n",
           info.title[0] ? info.title : "(untitled)", core->name,
           info.header_ok ? "" : "(header checksum mismatch)");

    if (info.sram_size && !(sram = calloc(1, info.sram_size))) {
        fprintf(stderr, "tinygb: out of memory for cart RAM\n");
        free(rom);
        return 2;
    }

    if (!(ctx = calloc(1, core->ctx_size))) {
        fprintf(stderr, "tinygb: out of memory for the core\n");
        free(sram); free(rom);
        return 2;
    }

    /* No audio rate: Phase 02 turns the sound on. Asking for it now would
       generate samples nothing consumes and slow the frame down. */
    if ((r = core->open(ctx, rom, rom_len, sram, info.sram_size, 0)) != TG_OK) {
        fprintf(stderr, "tinygb: %s\n", tg_strerror(r));
        free(ctx); free(sram); free(rom);
        return 2;
    }

    if (!tg_fb_open(&fb, fb_path)) {
        core->close(ctx);
        free(ctx); free(sram); free(rom);
        return 2;
    }

    printf("tinygb: %s is %ux%u, %u bpp, %u px per row\n",
           fb_path ? fb_path : "/dev/fb0", fb.w, fb.h, fb.bpp, fb.stride_px);

    pal = core->palette(ctx, &pal_n);
    tg_scaler_init(&scaler, pal, smooth);

    /* Black once, so the half below the picture is not last boot's console. */
    tg_fb_fill(&fb, 0x000000);
    tg_fb_layout(&fb, &ox, &oy);
    dst = tg_fb_at(&fb, ox, oy);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    core->set_buttons(ctx, 0);

    /*
     * 70224 cycles a frame at 4194304 Hz is 16.7427 ms, not 16.6667. Keeping it
     * as the exact rational and deriving each deadline from the start rather
     * than from the previous frame is what stops a tenth of a millisecond per
     * frame accumulating into a visible drift - it is six frames an hour, and
     * it will matter much more in Phase 02 when the audio clock is the master.
     */
    frame_ns = (long long)NS_PER_S * TG_FPS_DEN / TG_FPS_NUM;
    started = last_report = now_ns();
    next = started;

    while (!s_stop && (!limit || frames < limit)) {
        core->run_frame(ctx);
        tg_scale_15(&scaler, dst, fb.stride_px, core->pixels(ctx));
        frames++;

        if (!bench) {
            long long t;

            next += frame_ns;
            t = now_ns();
            if (next > t) {
                sleep_ns(next - t);
            } else if (t - next > NS_PER_S / 2) {
                /* Half a second behind is not a late frame, it is a stall -
                   a scheduler hiccup, or the disk. Catching up by running
                   frames flat out would fast-forward the game, so give up the
                   debt and carry on in real time. */
                next = t;
            }
        }

        /* Once a second, so it can be watched over ssh without the print
           itself becoming the workload. */
        {
            long long t = now_ns();

            if (t - last_report >= NS_PER_S) {
                double dt = (double)(t - last_report) / NS_PER_S;

                printf("%.2f fps\n", (double)(frames - frames_at_report) / dt);
                fflush(stdout);
                last_report = t;
                frames_at_report = frames;
            }
        }
    }

    {
        double secs = (double)(now_ns() - started) / NS_PER_S;

        printf("tinygb: %lu frames in %.2fs  =  %.2f fps%s\n",
               frames, secs, secs > 0 ? frames / secs : 0.0,
               bench ? "  (unpaced)" : "");
    }
    status = 0;

    tg_fb_close(&fb);
    core->close(ctx);
    free(ctx); free(sram); free(rom);
    return status;
}

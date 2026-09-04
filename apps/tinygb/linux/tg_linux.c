/*
 * tg_linux.c — TinyGB on the N31 Linux port.
 *
 * Loads a cartridge, runs it, puts it on /dev/fb0 at 240x216 in the top half
 * of the panel, plays it, takes its buttons from whatever this device has, and
 * keeps the cartridge's battery save on disk.
 *
 * Controls, in the absence of a working touchscreen: Volume Up and Down are A
 * and B, Play/Pause is Start, and the accelerometer is the d-pad. Home leaves.
 * The Apple Grape touch controller fails its firmware handshake on this build
 * and parks itself, so tilt is what makes a game playable today - see
 * tg_input.h, where touch is already a source waiting for the device to boot.
 *
 * Two pacers, and both are needed.
 *
 * Each iteration runs one emulated frame, asks the core for exactly the audio
 * that frame is worth, and writes it. A sink that blocks when full is the best
 * clock in the machine - the codec's crystal is the only one here that is not
 * approximate - so when the write blocks, it sets the pace and the video
 * follows the audio.
 *
 * But it cannot be the ONLY pacer. This device's tinyalsa write returns
 * success without blocking, whatever the buffer is doing, so relying on it
 * alone let the emulator free-run at 104 fps and fire 90 kHz of audio at a
 * 48 kHz device - which no sink survives, and which presented as a stream
 * restarting on almost every frame. So a monotonic deadline runs underneath
 * it: if the sink blocked, the deadline has already passed and nothing
 * sleeps; if it did not, the deadline holds the frame rate to 59.7275 and the
 * device is fed at exactly the rate it drains.
 *
 *   tinygb <rom.gb> [--fb /dev/fb0] [--frames N] [--sharp] [--bench] [--mute]
 *
 * Ctrl-C, or --frames, ends it and gives the console back.
 */

#include "../core/tg_core.h"
#include "../platform/tg_fb.h"
#include "../platform/tg_input.h"
#include "../platform/tg_roms.h"
#include "../platform/tg_save.h"
#include "../platform/tg_audio.h"
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
        "usage: tinygb [rom] [options]\n"
        "  rom           a path, or any part of a name in the library, so\n"
        "                'zelda' or 'blue' finds the cartridge without its\n"
        "                region codes. With no rom, the library is listed\n"
        "                and the first cartridge starts.\n"
        "  --list        list the library and exit\n"
        "  --fb PATH     framebuffer device (default /dev/fb0)\n"
        "  --frames N    stop after N frames instead of running until Ctrl-C\n"
        "  --sharp       nearest-neighbour scaling instead of the smooth 1.5x\n"
        "  --bench       run flat out and report frames per second\n"
        "  --mute        no audio device; pace off the monotonic clock\n"
        "  --warmup N    run N frames before the timers start\n"
        "  --noskip      repaint every row, even unchanged ones\n"
        "  --no-tilt     do not use the accelerometer as a d-pad\n"
        "  --tilt A,B    tilt press/release angles, percent of full scale\n"
        "  --probe-input list input devices and print events, then exit\n"
        "  --probe-audio print what the sound card will accept, then exit\n");
}

/*
 * The library, as the menu will one day show it.
 *
 * Printed rather than drawn, because there is no menu yet - but the scan is
 * the same one it will use, so what is listed here is what will be on it.
 */
static void print_library(const tg_rom_list *l, bool ok)
{
    unsigned i;

    if (!ok) {
        printf("tinygb: no library at %s\n", l->dir);
        printf("        put cartridges there, or pass a path, or set\n"
               "        TINYGB_ROMS.\n");
        return;
    }

    if (l->n == 0) {
        printf("tinygb: no cartridges in %s\n", l->dir);
        return;
    }

    printf("tinygb: %u cartridge%s in %s\n",
           l->n, l->n == 1 ? "" : "s", l->dir);
    for (i = 0; i < l->n; i++)
        printf("  %s\n", l->name[i]);
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *fb_path = NULL;
    /* The library directory, a slash, and a FAT long name. */
    char rom_resolved[768];
    bool list_only = false;
    unsigned long limit = 0;
    unsigned long warmup = 0;
    bool noskip = false, tilt = true;
    int tilt_on = 22, tilt_off = 12;
    tg_input *input = NULL;
    tg_save save;
    bool have_save = false;
    bool smooth = true, bench = false, mute = false;

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

    tg_audio *audio = NULL;
    tg_audio_clock aclock;
    int16_t *abuf = NULL;
    unsigned abuf_cap = 0;

    /* Where the frame goes. Two clock reads a frame is about a microsecond
       and it is the difference between optimising the emulator and optimising
       the blit, which on this device are not the same problem at all. */
    long long t_core = 0, t_blit = 0, t_apu = 0, t_sink = 0, t_mark;

    long long frame_ns, started, next, last_report;
    unsigned long frames = 0, frames_at_report = 0;
    unsigned long long audio_frames = 0;
    int status = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fb") && i + 1 < argc)          fb_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--sharp"))                  smooth = false;
        else if (!strcmp(argv[i], "--bench"))                  bench = true;
        else if (!strcmp(argv[i], "--mute"))                   mute = true;
        else if (!strcmp(argv[i], "--noskip"))                 noskip = true;
        else if (!strcmp(argv[i], "--no-tilt"))                tilt = false;
        else if (!strcmp(argv[i], "--tilt") && i + 1 < argc)
            sscanf(argv[++i], "%d,%d", &tilt_on, &tilt_off);
        else if (!strcmp(argv[i], "--list"))                   list_only = true;
        else if (!strcmp(argv[i], "--probe-input")) { tg_input_probe(15); return 0; }
        else if (!strcmp(argv[i], "--probe-audio")) { tg_audio_probe(); return 0; }
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] == '-')                            { usage(); return 2; }
        else if (!rom_path)                                    rom_path = argv[i];
        else                                                   { usage(); return 2; }
    }

    /*
     * Which cartridge.
     *
     * Three ways in, and they all end at a path: --list just says what is
     * there; a bare name is matched against the library; nothing at all
     * starts the first cartridge, which is the closest thing to a menu until
     * there is one.
     */
    if (list_only || !rom_path) {
        tg_rom_list lib;
        bool ok = tg_roms_scan(&lib);

        print_library(&lib, ok);

        if (list_only || !ok || lib.n == 0) {
            tg_roms_free(&lib);
            return (list_only && ok) ? 0 : 2;
        }

        snprintf(rom_resolved, sizeof rom_resolved, "%s/%s",
                 lib.dir, lib.name[0]);
        tg_roms_free(&lib);
        rom_path = rom_resolved;
        printf("tinygb: starting %s\n", rom_path);
    } else {
        bool ambiguous;

        if (!tg_roms_resolve(rom_path, rom_resolved, sizeof rom_resolved,
                             &ambiguous)) {
            char dir[512];

            tg_roms_dir(dir, sizeof dir);
            if (ambiguous)
                fprintf(stderr, "tinygb: \"%s\" matches more than one "
                                "cartridge in %s - be more specific\n",
                        rom_path, dir);
            else
                fprintf(stderr, "tinygb: no cartridge \"%s\" - not a file, "
                                "and not in %s\n", rom_path, dir);
            return 2;
        }
        rom_path = rom_resolved;
    }

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

    /*
     * The battery, before the core opens - a cartridge reads its save during
     * its own startup, so loading afterwards would be a frame too late and the
     * game would already have decided it was a new one.
     */
    if (info.sram_size) {
        char sav[512];

        tg_save_path_for(rom_path, sav, sizeof sav);
        have_save = tg_save_load(&save, sav, sram, info.sram_size);
        printf("tinygb: save %s  %s\n", sav,
               have_save ? "loaded" : "(new)");
    }

    if (!(ctx = calloc(1, core->ctx_size))) {
        fprintf(stderr, "tinygb: out of memory for the core\n");
        free(sram); free(rom);
        return 2;
    }

    /*
     * The sink opens first, because the core has to be told the rate that was
     * actually granted rather than the one we asked for. minigb_apu is
     * compiled for one rate, so if the device gave us a different one the core
     * would be generating at the wrong pitch - and that is worth knowing about
     * loudly rather than discovering by ear.
     */
    if (!bench) {
        audio = tg_audio_open(AUDIO_SAMPLE_RATE, mute);
        if (!audio) {
            fprintf(stderr, "tinygb: no audio\n");
            free(ctx); free(sram); free(rom);
            return 2;
        }
        if (!tg_audio_is_silent(audio) &&
            tg_audio_rate(audio) != (unsigned)AUDIO_SAMPLE_RATE) {
            fprintf(stderr,
                    "tinygb: the APU is built for %d Hz but the device gave "
                    "%u Hz - pitch will be wrong\n",
                    AUDIO_SAMPLE_RATE, tg_audio_rate(audio));
        }
    }

    if ((r = core->open(ctx, rom, rom_len, sram, info.sram_size,
                        audio ? AUDIO_SAMPLE_RATE : 0)) != TG_OK) {
        fprintf(stderr, "tinygb: %s\n", tg_strerror(r));
        tg_audio_close(audio);
        free(ctx); free(sram); free(rom);
        return 2;
    }

    if (audio) {
        tg_audio_clock_init(&aclock, tg_audio_rate(audio));
        /* Two video frames of headroom: the clock hands out 803 or 804, and a
           buffer sized to the larger never has to grow mid-run. */
        abuf_cap = tg_audio_rate(audio) / 30 + 64;
        if (!(abuf = malloc((size_t)abuf_cap * 2 * sizeof *abuf))) {
            fprintf(stderr, "tinygb: out of memory for the audio buffer\n");
            core->close(ctx); tg_audio_close(audio);
            free(ctx); free(sram); free(rom);
            return 2;
        }
        printf("tinygb: audio %u Hz%s\n", tg_audio_rate(audio),
               tg_audio_is_silent(audio) ? " (silent, paced only)" : "");
    }

    if (!tg_fb_open(&fb, fb_path)) {
        core->close(ctx); tg_audio_close(audio);
        free(abuf); free(ctx); free(sram); free(rom);
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

    /*
     * Whatever this device can offer. The touchscreen is the intended d-pad
     * and is not booting on this build, so tilt is what makes a game playable
     * today - and because the sources are OR-ed, touch appearing later needs
     * no change here.
     */
    input = tg_input_open(TG_SRC_KEYS | (tilt ? TG_SRC_TILT : 0) | TG_SRC_TOUCH);
    if (input) {
        unsigned got = tg_input_sources(input);

        tg_input_set_tilt(input, tilt_on, tilt_off);
        printf("tinygb: input%s%s%s\n",
               (got & TG_SRC_KEYS)  ? " keys" : "",
               (got & TG_SRC_TILT)  ? " tilt" : "",
               (got & TG_SRC_TOUCH) ? " touch" : "");
        /* Level was measured during tg_input_open, so how the device was
           lying a moment ago is now the neutral position. Worth saying: put
           down flat and picked up to play, every direction is held at once. */
        if (got & TG_SRC_TILT)
            printf("tinygb: tilt d-pad - hold it the way you mean to play\n");
        if (!got) printf("tinygb: no input devices - nothing to play with\n");
    }

    core->set_buttons(ctx, 0);

    /*
     * Get past the boot logo before timing anything.
     *
     * A cartridge spends its first few hundred frames scrolling a logo with the
     * CPU halted, which is nothing like the game that follows. Measuring from
     * frame zero made Tetris report 7.3 ms of core time on one run and 14.6 ms
     * on another purely because the runs had different frame counts - and that
     * difference very nearly got read as a large win from an unrelated change
     * to the blit.
     */
    for (unsigned long i = 0; i < warmup; i++) {
        core->run_frame(ctx);
        if (audio && core->audio_pull) {
            /* Keep the APU's clock moving too, or the measured run starts with
               a frame of backlog to work off. */
            unsigned w = tg_audio_clock_next(&aclock);

            if (w > abuf_cap) w = abuf_cap;
            core->audio_pull(ctx, abuf, w);
        }
    }
    if (warmup) {
        /* Paint once, so the skip-unchanged logic starts from a real frame
           rather than counting the whole first screen as changed. */
        tg_scale_15(&scaler, dst, fb.stride_px, core->pixels(ctx));
        printf("tinygb: %lu warm-up frames done\n", warmup);
    }

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
        if (input) {
            core->set_buttons(ctx, tg_input_poll(input));
            if (tg_input_take_quit(input)) break;   /* Home leaves the game */
        }

        /*
         * Write the save out if it changed. Not only on the way out: a
         * handheld leaves by having its power button held, and nothing gets to
         * run at that point.
         */
        if (info.sram_size) tg_save_tick(&save, now_ns());

        t_mark = now_ns();
        core->run_frame(ctx);
        t_core += now_ns() - t_mark;

        t_mark = now_ns();
        /* Forcing a repaint every frame is what the blit cost was before rows
           could be skipped. Here so the two can be compared on the same game
           at the same moment, rather than across two runs of different
           content - which is exactly the mistake that made a boot logo look
           like a performance regression. */
        if (noskip) tg_scaler_invalidate(&scaler);
        tg_scale_15(&scaler, dst, fb.stride_px, core->pixels(ctx));
        t_blit += now_ns() - t_mark;

        frames++;

        if (audio) {
            /*
             * Sound first, picture already done. The write blocks until the
             * device has room, so this is where the frame's time is spent -
             * and by the time it returns, the next frame is due.
             *
             * Asking the clock how many frames this one is worth rather than
             * using a constant is what stops the sink starving: 48000/59.7275
             * is 803.65, and 803 every time is 39 frames a second short, which
             * empties an eight-period buffer in about four seconds.
             */
            unsigned want = tg_audio_clock_next(&aclock);
            unsigned got;

            if (want > abuf_cap) want = abuf_cap;
            t_mark = now_ns();
            got = core->audio_pull ? core->audio_pull(ctx, abuf, want) : 0;
            t_apu += now_ns() - t_mark;
            if (got < want) {
                /* A core with no APU, or one that came up short. Silence is
                   the only honest filler; repeating the last block would be a
                   buzz. */
                memset(abuf + (size_t)got * 2, 0,
                       (size_t)(want - got) * 2 * sizeof *abuf);
            }
            t_mark = now_ns();
            if (!tg_audio_write(audio, abuf, want)) {
                t_sink += now_ns() - t_mark;
                fprintf(stderr, "tinygb: carrying on without sound\n");
                tg_audio_close(audio);
                audio = NULL;
                next = now_ns();     /* the monotonic clock takes over */
            } else {
                t_sink += now_ns() - t_mark;
                audio_frames += want;
            }
        }

        if (!bench) {
            long long t;

            /*
             * The deadline, whether or not there is a sink. When the write
             * blocked, this has already passed and costs one clock read; when
             * it did not, this is what keeps the emulator at 59.7275 fps
             * instead of as fast as the CPU allows.
             */
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

        if (frames) {
            /* Per frame, in milliseconds. The budget is 16.74. */
            double f = (double)frames;

            printf("tinygb: core %.2f ms  blit %.2f ms  apu %.2f ms"
                   "  sink %.2f ms  per frame\n",
                   t_core / f / 1e6, t_blit / f / 1e6,
                   t_apu / f / 1e6, t_sink / f / 1e6);
        }

        if (audio_frames) {
            /* The two clocks, side by side. If these disagree, the emulator
               and the sink are running at different speeds and everything
               else is guesswork. */
            printf("tinygb: %llu audio frames  =  %.1f Hz measured"
                   "  (%lu restarts)\n",
                   audio_frames, secs > 0 ? audio_frames / secs : 0.0,
                   tg_audio_restarts(audio));
        }
    }
    status = 0;

    if (info.sram_size && tg_save_flush(&save))
        printf("tinygb: save written\n");

    tg_input_close(input);
    tg_fb_close(&fb);
    core->close(ctx);
    tg_audio_close(audio);
    free(abuf); free(ctx); free(sram); free(rom);
    return status;
}

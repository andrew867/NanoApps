/*
 * tg_headless.c — run a cartridge with no screen, and say what came out.
 *
 * This is the Phase 00 gate. Everything TinyGB does that is not drawing or
 * making noise can be checked here, on a desktop, in a fraction of a second:
 * the core runs, the cartridge header parses, the picture is what it should be,
 * and the test ROMs pass.
 *
 * The two gates it exists to answer:
 *
 *   dmg-acid2.gb   a single frame that is either pixel-correct or is not.
 *                  Dump it with -r and compare against the reference image,
 *                  which is 2-bit greyscale at the same size - so the check is
 *                  an equality test on all 23040 pixels, not a look.
 *   cpu_instrs.gb  Blargg's suite, which reports over the link port. Capture
 *                  the serial bytes and look for "Passed".
 *
 *   ./tg_headless rom.gb [-f frames] [-o out.ppm] [-c core] [-s] [-q]
 *
 * Exit status is 0 when nothing went wrong and, if serial output mentioned
 * failure, 1 - so this drops into a script without parsing anything.
 */

#include "../core/tg_core.h"
#include "../platform/tg_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a file, as bytes ---------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);

    if (!(buf = malloc((size_t)n))) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

/* ---- CRC32, so two runs can be compared without keeping the image --------- */

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t tab[256];
    static int built;
    uint32_t c = 0xFFFFFFFFu;

    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            tab[i] = v;
        }
        built = 1;
    }
    for (size_t i = 0; i < n; i++)
        c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- serial capture ------------------------------------------------------ */

/* Blargg's ROMs print a banner, the failing test, and then Passed or Failed.
   A few KiB is far more than any of them emit. */
#define SERIAL_CAP 4096

typedef struct {
    char   buf[SERIAL_CAP + 1];
    size_t len;
} serial_log;

static void serial_byte(void *user, uint8_t b)
{
    serial_log *s = user;

    if (s->len < SERIAL_CAP) s->buf[s->len++] = (char)b;
}

/* ---- writing the frame out ----------------------------------------------- */

/*
 * The frame as shade indices, one byte per pixel, nothing else.
 *
 * dmg-acid2 ships a reference image that is 2-bit greyscale at exactly this
 * size, so a comparison against it should be an equality test on 23040 bytes.
 * Going through the PPM would mean colouring the pixels and then guessing the
 * shades back out of them, which can only lose information and would make the
 * palette a variable in a test that has nothing to do with palettes.
 */
static int write_raw(const char *path, const uint8_t *px)
{
    FILE *f = fopen(path, "wb");
    uint8_t shades[TG_W * TG_H];

    if (!f) return -1;
    for (size_t i = 0; i < sizeof shades; i++)
        shades[i] = px[i] & TG_PX_SHADE;
    if (fwrite(shades, 1, sizeof shades, f) != sizeof shades) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/*
 * Binary PPM, because it is six lines to write and every image tool reads it.
 * The pixel byte carries the palette bits as well as the shade, so mask before
 * indexing or an object pixel indexes past the end of a four-entry palette.
 */
static int write_ppm(const char *path, const uint8_t *px,
                     const uint32_t *pal, unsigned pal_n)
{
    FILE *f = fopen(path, "wb");

    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", TG_W, TG_H);
    for (size_t i = 0; i < (size_t)TG_W * TG_H; i++) {
        unsigned idx = px[i] & TG_PX_SHADE;
        uint32_t rgb = pal[idx < pal_n ? idx : 0];
        uint8_t  out[3] = { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8),
                            (uint8_t)rgb };

        if (fwrite(out, 1, 3, f) != 3) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

/*
 * The scaled picture, exactly as the device will show it.
 *
 * Same scaler the framebuffer front end uses, so what comes out of here is
 * what goes on the panel - which makes "is 1.5x actually any good on Game Boy
 * art" a question that can be answered on a desktop instead of over ssh.
 */
static int write_scaled_ppm(const char *path, const uint8_t *px,
                            const uint32_t *pal, bool smooth)
{
    FILE *f = fopen(path, "wb");
    tg_scaler sc;
    uint32_t *buf;

    if (!f) return -1;
    if (!(buf = malloc((size_t)TG_SCALED_W * TG_SCALED_H * sizeof *buf))) {
        fclose(f);
        return -1;
    }

    tg_scaler_init(&sc, pal, smooth);
    tg_scale_15(&sc, buf, TG_SCALED_W, px);

    fprintf(f, "P6\n%d %d\n255\n", TG_SCALED_W, TG_SCALED_H);
    for (size_t i = 0; i < (size_t)TG_SCALED_W * TG_SCALED_H; i++) {
        uint8_t out[3] = { (uint8_t)(buf[i] >> 16), (uint8_t)(buf[i] >> 8),
                           (uint8_t)buf[i] };

        if (fwrite(out, 1, 3, f) != 3) { free(buf); fclose(f); return -1; }
    }
    free(buf);
    fclose(f);
    return 0;
}

/* ---- ------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: tg_headless <rom.gb> [-f frames] [-o out.ppm] [-c core]\n"
        "                   [-s] [-q] [-l]\n"
        "  -f N    run N frames (default 60)\n"
        "  -o P    write the final frame to P as binary PPM\n"
        "  -r P    write the final frame to P as raw shade indices\n"
        "  -S P    write the frame SCALED to 240x216 as the device shows it\n"
        "  --sharp nearest-neighbour scaling for -S, instead of smooth\n"
        "  -c ID   force a core by id; default is the best for the cartridge\n"
        "  -s      capture and print link-port output (Blargg's test ROMs)\n"
        "  -q      only print the summary line\n"
        "  -l      list the cores compiled in, and exit\n");
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *out_path = NULL, *core_id = NULL;
    const char *raw_path = NULL, *scaled_path = NULL;
    bool smooth = true;
    unsigned frames = 60;
    int want_serial = 0, quiet = 0;

    uint8_t *rom = NULL, *sram = NULL;
    void *ctx = NULL;
    size_t rom_len = 0, sram_len = 0;
    const tg_core *core;
    tg_rom_info info;
    enum tg_result r;
    serial_log slog = { .len = 0 };
    const uint32_t *pal;
    unsigned pal_n = 0;
    uint32_t hash;
    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) {
            for (unsigned k = 0; k < tg_core_count(); k++) {
                const tg_core *c = tg_core_at(k);

                printf("%-10s %-14s %s%s%s\n", c->id, c->name,
                       (c->caps & TG_CAP_DMG) ? "DMG " : "",
                       (c->caps & TG_CAP_CGB) ? "CGB " : "",
                       (c->caps & TG_CAP_SOUND) ? "sound" : "");
                printf("           %s\n", c->blurb);
                /* The context is the core's entire RAM cost - it has no
                   allocator of its own - so this is the number the RetailOS
                   heap budget gets checked against. */
                printf("           context %zu bytes (%.1f KiB)\n",
                       c->ctx_size, c->ctx_size / 1024.0);
            }
            return 0;
        }
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) scaled_path = argv[++i];
        else if (!strcmp(argv[i], "--sharp"))            smooth = false;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) core_id = argv[++i];
        else if (!strcmp(argv[i], "-s")) want_serial = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (argv[i][0] == '-')      { usage(); return 2; }
        else if (!rom_path)              rom_path = argv[i];
        else                             { usage(); return 2; }
    }

    if (!rom_path) { usage(); return 2; }

    if (!(rom = read_file(rom_path, &rom_len))) {
        fprintf(stderr, "tg_headless: cannot read %s\n", rom_path);
        return 2;
    }

    if ((r = tg_rom_probe(rom, rom_len, &info)) != TG_OK) {
        fprintf(stderr, "tg_headless: %s\n", tg_strerror(r));
        free(rom);
        return 2;
    }

    core = core_id ? tg_core_by_id(core_id) : tg_core_for_rom(rom, rom_len);
    if (!core) {
        fprintf(stderr, "tg_headless: %s\n",
                core_id ? "no such core" :
                "no compiled-in core can run this cartridge"
                " (Game Boy Color needs the second core)");
        free(rom);
        return 2;
    }

    if (!quiet) {
        printf("rom      %s\n", rom_path);
        printf("title    %s\n", info.title[0] ? info.title : "(none)");
        printf("model    %s%s\n",
               info.cgb ? "CGB-aware" : "DMG",
               info.cgb_only ? ", colour required" : "");
        printf("mapper   0x%02X   rom %zu KiB declared, %zu KiB on disk\n",
               info.mapper_code, info.rom_size / 1024, rom_len / 1024);
        printf("sram     %zu bytes\n", info.sram_size);
        printf("header   checksum %s\n", info.header_ok ? "ok" : "MISMATCH");
        printf("core     %s\n", core->name);
    }

    /* Cart RAM, when the cartridge has any. A zero-size malloc is
       implementation-defined, so skip it rather than rely on it. */
    sram_len = info.sram_size;
    if (sram_len && !(sram = calloc(1, sram_len))) {
        fprintf(stderr, "tg_headless: out of memory for cart RAM\n");
        free(rom);
        return 2;
    }

    if (!(ctx = calloc(1, core->ctx_size))) {
        fprintf(stderr, "tg_headless: out of memory for the core\n");
        free(sram); free(rom);
        return 2;
    }

    /* No sound: this runs faster than real time on purpose, and generating
       audio nobody listens to would only slow the gate down. */
    if ((r = core->open(ctx, rom, rom_len, sram, sram_len, 0)) != TG_OK) {
        fprintf(stderr, "tg_headless: %s\n", tg_strerror(r));
        free(ctx); free(sram); free(rom);
        return 2;
    }

    if (want_serial && core->set_serial_sink)
        core->set_serial_sink(ctx, serial_byte, &slog);

    core->set_buttons(ctx, 0);
    for (unsigned i = 0; i < frames; i++)
        core->run_frame(ctx);

    pal = core->palette(ctx, &pal_n);
    hash = crc32_of(core->pixels(ctx), (size_t)TG_W * TG_H);

    if (out_path && write_ppm(out_path, core->pixels(ctx), pal, pal_n) != 0) {
        fprintf(stderr, "tg_headless: cannot write %s\n", out_path);
        status = 2;
    }

    if (raw_path && write_raw(raw_path, core->pixels(ctx)) != 0) {
        fprintf(stderr, "tg_headless: cannot write %s\n", raw_path);
        status = 2;
    }

    if (scaled_path &&
        write_scaled_ppm(scaled_path, core->pixels(ctx), pal, smooth) != 0) {
        fprintf(stderr, "tg_headless: cannot write %s\n", scaled_path);
        status = 2;
    }

    if (want_serial) {
        slog.buf[slog.len] = 0;
        if (!quiet && slog.len) printf("--- link port ---\n%s\n", slog.buf);
        /* Blargg says "Passed" or "Failed"; a suite that says neither has not
           finished, which is a failure of this run rather than of the core. */
        if (strstr(slog.buf, "Failed")) status = 1;
        else if (!strstr(slog.buf, "Passed") && slog.len) status = 1;
    }

    printf("frames %u  crc32 %08X%s\n", frames, hash,
           out_path ? "  wrote the frame" : "");

    core->close(ctx);
    free(ctx); free(sram); free(rom);
    return status;
}

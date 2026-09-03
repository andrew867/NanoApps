/*
 * tests.c — the parts of TinyGB that can be wrong without a Game Boy.
 *
 * Reading a cartridge header is the step everything else stands on: it decides
 * which core can run the thing, how much save RAM to allocate, and whether the
 * file is a cartridge at all. It is also all fixed offsets and lookup tables,
 * which is exactly the kind of code that is quietly wrong for months.
 *
 * The ROMs here are synthesised rather than fetched, so this suite needs no
 * network and no copyrighted anything. The real cartridges get exercised by
 * tools/gate.sh.
 */

#include "../core/tg_core.h"
#include "../platform/tg_audio.h"
#include "../platform/tg_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void ok(const char *what, int cond)
{
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

/* ---- a cartridge that is not a real game --------------------------------- */

#define ROM_BYTES (32 * 1024)

/*
 * Enough of a cartridge for gb_init to accept it and the CPU to run: a valid
 * header with a correct checksum, and an entry point that jumps to itself.
 * 0x18 0xFE is `jr -2`, the one-instruction infinite loop, which is what a ROM
 * that has nothing to do should do rather than run off into unmapped space.
 */
static uint8_t *make_rom(uint8_t mapper, uint8_t ram_code, uint8_t cgb_flag,
                         const char *title)
{
    uint8_t *rom = calloc(1, ROM_BYTES);
    uint8_t sum = 0;

    if (!rom) abort();

    rom[0x0100] = 0x00;             /* nop  */
    rom[0x0101] = 0xC3;             /* jp 0x0150 */
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;
    rom[0x0150] = 0x18;             /* jr -2 */
    rom[0x0151] = 0xFE;

    memset(rom + 0x0134, ' ', 16);
    if (title) memcpy(rom + 0x0134, title, strlen(title) > 15 ? 15 : strlen(title));

    rom[0x0143] = cgb_flag;
    rom[0x0146] = 0x00;             /* not SGB */
    rom[0x0147] = mapper;
    rom[0x0148] = 0x00;             /* 32 KiB */
    rom[0x0149] = ram_code;

    for (uint16_t a = 0x0134; a <= 0x014C; a++)
        sum = (uint8_t)(sum - rom[a] - 1);
    rom[0x014D] = sum;

    return rom;
}

/* ---- header -------------------------------------------------------------- */

static void test_header(void)
{
    tg_rom_info info;
    uint8_t *rom;

    printf("cartridge header:\n");

    rom = make_rom(0x00, 0x00, 0x00, "TESTROM");
    ok("reads a plain 32 KiB cartridge",
       tg_rom_probe(rom, ROM_BYTES, &info) == TG_OK);
    ok("checksum agrees",            info.header_ok);
    ok("title, trailing spaces gone", !strcmp(info.title, "TESTROM"));
    ok("not colour",                 !info.cgb && !info.cgb_only);
    ok("declared size is 32 KiB",    info.rom_size == 32 * 1024);
    ok("no cart RAM",                info.sram_size == 0);
    free(rom);

    /*
     * Byte 0x0149 is a code, not an exponent, and the codes are not ordered:
     * 0x05 is 64 KiB while 0x04 is 128. A shift-based conversion gets both
     * wrong and would silently under-allocate a save file.
     */
    rom = make_rom(0x03, 0x03, 0x00, "RAM32");
    tg_rom_probe(rom, ROM_BYTES, &info);
    ok("RAM code 0x03 is 32 KiB",  info.sram_size == 32 * 1024);
    free(rom);

    rom = make_rom(0x03, 0x04, 0x00, "RAM128");
    tg_rom_probe(rom, ROM_BYTES, &info);
    ok("RAM code 0x04 is 128 KiB", info.sram_size == 128 * 1024);
    free(rom);

    rom = make_rom(0x03, 0x05, 0x00, "RAM64");
    tg_rom_probe(rom, ROM_BYTES, &info);
    ok("RAM code 0x05 is 64 KiB, not more than 0x04",
       info.sram_size == 64 * 1024);
    free(rom);

    /* MBC2 keeps its save inside the mapper and declares no RAM at all. Taking
       the header at its word gives it nowhere to save. */
    rom = make_rom(0x06, 0x00, 0x00, "MBC2");
    tg_rom_probe(rom, ROM_BYTES, &info);
    ok("MBC2 gets RAM despite declaring none", info.sram_size == 512);
    free(rom);

    /* The title field lost its last byte to the CGB flag. Reading sixteen on a
       colour cartridge picks the flag up as text. */
    rom = make_rom(0x1B, 0x02, 0xC0, "COLOURONLY12345");
    tg_rom_probe(rom, ROM_BYTES, &info);
    ok("colour cartridge is seen as colour", info.cgb);
    ok("...and as colour-only",             info.cgb_only);
    ok("title stops before the CGB flag",   strlen(info.title) <= 15);
    free(rom);

    rom = make_rom(0x1B, 0x02, 0x80, "DUALMODE");
    tg_rom_probe(rom, ROM_BYTES, &info);
    ok("0x80 is colour-aware but not colour-only",
       info.cgb && !info.cgb_only);
    free(rom);

    /* A bad checksum is reported, not fatal: plenty of homebrew ships with one
       and a real Game Boy is the only thing that refuses to start. */
    rom = make_rom(0x00, 0x00, 0x00, "BADSUM");
    rom[0x014D] ^= 0xFF;
    tg_rom_probe(rom, ROM_BYTES, &info);
    ok("a wrong checksum is reported", !info.header_ok);
    free(rom);

    ok("a file too short to hold a header is rejected",
       tg_rom_probe((const uint8_t *)"short", 5, &info) == TG_ERR_ROM);
}

/* ---- the registry -------------------------------------------------------- */

static void test_registry(void)
{
    uint8_t *rom;

    printf("core registry:\n");

    ok("at least one core is compiled in", tg_core_count() >= 1);
    ok("peanut is findable by id",         tg_core_by_id("peanut") != NULL);
    ok("an unknown id finds nothing",      tg_core_by_id("nope") == NULL);

    {
        const tg_core *c = tg_core_by_id("peanut");

        ok("peanut claims Game Boy",       c && (c->caps & TG_CAP_DMG));
        /* If this ever passes, the vendored Peanut-GB grew a colour path and
           the plan's Phase 07 has changed shape. */
        ok("peanut does not claim colour", c && !(c->caps & TG_CAP_CGB));
        ok("peanut states a context size", c && c->ctx_size > 0);
        ok("peanut has a link port",       c && c->set_serial_sink != NULL);
    }

    /* A colour-aware cartridge still runs on a Game Boy - that is what the
       compatibility byte is for - so it must not be hidden from a DMG core. */
    rom = make_rom(0x1B, 0x02, 0x80, "DUAL");
    ok("a colour-aware cartridge finds a core",
       tg_core_for_rom(rom, ROM_BYTES) != NULL);
    free(rom);

    /* Colour-only is the case that genuinely has no core yet. */
    rom = make_rom(0x1B, 0x02, 0xC0, "CGBONLY");
    ok("a colour-only cartridge finds none, for now",
       tg_core_for_rom(rom, ROM_BYTES) == NULL);
    free(rom);
}

/* ---- actually running one ------------------------------------------------ */

static void test_run(void)
{
    const tg_core *c = tg_core_by_id("peanut");
    uint8_t *rom = make_rom(0x00, 0x00, 0x00, "SPIN");
    void *ctx;
    const uint8_t *px;
    int uniform = 1;

    printf("running a cartridge:\n");

    if (!c) { fails++; free(rom); return; }

    ctx = calloc(1, c->ctx_size);
    if (!ctx) { fails++; free(rom); return; }

    ok("opens", c->open(ctx, rom, ROM_BYTES, NULL, 0, 0) == TG_OK);

    c->set_buttons(ctx, 0);
    for (int i = 0; i < 10; i++) c->run_frame(ctx);

    px = c->pixels(ctx);
    ok("produces a frame", px != NULL);

    /* Nothing has been drawn, so every pixel should be the same shade. A frame
       that varies means the buffer is uninitialised or the scanline callback
       is writing at the wrong stride - both of which look like noise. */
    for (size_t i = 1; px && i < (size_t)TG_W * TG_H; i++)
        if ((px[i] & TG_PX_SHADE) != (px[0] & TG_PX_SHADE)) { uniform = 0; break; }
    ok("a blank screen is uniform", uniform);

    {
        unsigned n = 0;
        const uint32_t *pal = c->palette(ctx, &n);

        ok("publishes a palette", pal != NULL && n >= 4);
    }

    /* Reset has to be usable on a live session, since that is what the menu's
       "restart" does. */
    c->reset(ctx);
    c->run_frame(ctx);
    ok("survives a reset", c->pixels(ctx) != NULL);

    /* A cartridge whose header says it needs more save RAM than the caller
       brought must be refused, not quietly truncated. */
    {
        void *ctx2 = calloc(1, c->ctx_size);
        uint8_t *big = make_rom(0x03, 0x03, 0x00, "NEEDSRAM");
        uint8_t small[16];

        ok("refuses a cartridge whose save RAM will not fit",
           ctx2 && c->open(ctx2, big, ROM_BYTES, small, sizeof small, 0)
                   == TG_ERR_CAPACITY);
        free(ctx2);
        free(big);
    }

    c->close(ctx);
    free(ctx);
    free(rom);
}

/* ---- the scaler ---------------------------------------------------------- */

/*
 * 160x144 to 240x216 is two source pixels becoming three, and there are only a
 * few ways to get it wrong - but each of them looks like "the picture is a bit
 * off" rather than like a bug, so they are worth pinning.
 */
static void test_scale(void)
{
    static const uint32_t pal[4] = { 0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000 };
    static uint8_t  src[TG_W * TG_H];
    /* A stride wider than the picture, because the real framebuffer has one
       and a scaler that assumes stride == width writes a diagonal. */
    enum { STRIDE = TG_SCALED_W + 17 };
    static uint32_t dst[STRIDE * (TG_SCALED_H + 2)];
    static tg_scaler sc;

    printf("scaler:\n");

    ok("240x216 is exactly 1.5x",
       TG_SCALED_W == 240 && TG_SCALED_H == 216);

    /* A flat field must come out flat, whatever the blending does. */
    memset(src, 2, sizeof src);
    memset(dst, 0xAB, sizeof dst);
    tg_scaler_init(&sc, pal, true);
    tg_scale_15(&sc, dst, STRIDE, src);
    {
        int flat = 1;

        for (unsigned y = 0; y < TG_SCALED_H && flat; y++)
            for (unsigned x = 0; x < TG_SCALED_W; x++)
                if (dst[y * STRIDE + x] != pal[2]) { flat = 0; break; }
        ok("a flat field scales flat", flat);
    }

    /* Nothing outside the picture may be touched: the row after the last one,
       and the columns past the right edge, still hold the fill. */
    {
        int clean = 1;

        for (unsigned x = TG_SCALED_W; x < STRIDE; x++)
            if (dst[x] != 0xABABABABu) { clean = 0; break; }
        ok("does not write past the right edge", clean);

        clean = 1;
        for (unsigned x = 0; x < STRIDE; x++)
            if (dst[TG_SCALED_H * STRIDE + x] != 0xABABABABu) { clean = 0; break; }
        ok("does not write past the last row", clean);
    }

    /*
     * The pattern that matters. Two adjacent source pixels of different shades
     * become left, blend, right - so each source pixel keeps one full-strength
     * output pixel and the seam between them is the average. Nearest
     * neighbour would give left, left, right, which is what makes one-pixel
     * Game Boy detail flicker between one and two pixels wide.
     */
    memset(src, 0, sizeof src);
    src[0] = 0;   /* white */
    src[1] = 3;   /* black */
    tg_scaler_init(&sc, pal, true);
    tg_scale_15(&sc, dst, STRIDE, src);
    ok("smooth: left pixel is untouched",  dst[0] == 0xFFFFFF);
    ok("smooth: middle is the average",    dst[1] == 0x7F7F7F);
    ok("smooth: right pixel is untouched", dst[2] == 0x000000);

    tg_scaler_init(&sc, pal, false);
    tg_scale_15(&sc, dst, STRIDE, src);
    ok("sharp: middle repeats the left",   dst[1] == 0xFFFFFF);
    ok("sharp: right pixel is untouched",  dst[2] == 0x000000);

    /* Vertically it is the same rule, and getting the row arithmetic wrong is
       the classic way to produce a picture that is subtly squashed. */
    memset(src, 0, sizeof src);
    memset(src + TG_W, 3, TG_W);     /* row 0 white, row 1 black */
    tg_scaler_init(&sc, pal, true);
    tg_scale_15(&sc, dst, STRIDE, src);
    ok("rows: first is the top source row",  dst[0 * STRIDE] == 0xFFFFFF);
    ok("rows: second is the average",        dst[1 * STRIDE] == 0x7F7F7F);
    ok("rows: third is the bottom row",      dst[2 * STRIDE] == 0x000000);

    /* Only the shade bits are the shade. A core that also reports which
       palette a pixel came from must not index past a four-entry table. */
    memset(src, 0, sizeof src);
    for (size_t i = 0; i < sizeof src; i++) src[i] = 1 | TG_PX_OBJ | TG_PX_OBJ1;
    tg_scale_15(&sc, dst, STRIDE, src);
    ok("ignores the palette bits", dst[0] == pal[1]);

    /*
     * Skipping unchanged rows.
     *
     * The blit is a quarter of the frame budget on the device because the
     * framebuffer is uncached, and most of a Game Boy frame is usually the
     * previous frame. The risk of skipping is stale pixels, so what is checked
     * here is that a skipped frame leaves the CORRECT pixels behind and that
     * anything which invalidates the assumption repaints.
     */
    memset(src, 1, sizeof src);
    tg_scaler_init(&sc, pal, true);
    tg_scale_15(&sc, dst, STRIDE, src);          /* full paint */

    /* Scribble over the destination, then send the same frame again. With no
       skipping the scribble would be repainted; with skipping it survives,
       which is exactly the hazard - so this pins the behaviour rather than
       pretending it does not exist. */
    dst[5 * STRIDE + 7] = 0xDEADBEEF;
    tg_scale_15(&sc, dst, STRIDE, src);
    ok("an unchanged frame is skipped", dst[5 * STRIDE + 7] == 0xDEADBEEF);

    /* ...and invalidating repaints it, which is what a menu closing must do. */
    tg_scaler_invalidate(&sc);
    tg_scale_15(&sc, dst, STRIDE, src);
    ok("invalidate forces a repaint",  dst[5 * STRIDE + 7] == pal[1]);

    /* A row that changes is redrawn even when its neighbours do not. */
    memset(src + 40 * TG_W, 3, TG_W);
    tg_scale_15(&sc, dst, STRIDE, src);
    ok("a changed row is redrawn",     dst[60 * STRIDE] == pal[3]);
    ok("its neighbours are still right", dst[0] == pal[1]);
}

/* ---- the audio clock ----------------------------------------------------- */

/*
 * How many audio frames one video frame is worth.
 *
 * The naive answer - rate divided by 59.7275, truncated - is 803 at 48 kHz,
 * and 803 every frame is 47961 Hz. That starves the sink by 39 frames a
 * second, which empties an eight-period buffer in about four seconds and
 * presents as audio that clicks roughly every four seconds forever. The fix is
 * to keep the remainder, and the thing worth testing is that it has no drift
 * over a long run rather than that any single frame is right.
 */
static void test_audio_clock(void)
{
    tg_audio_clock c;

    printf("audio clock:\n");

    tg_audio_clock_init(&c, 48000);

    {
        unsigned n = tg_audio_clock_next(&c);

        ok("a frame is 803 or 804 at 48 kHz", n == 803 || n == 804);
    }

    /* An hour of frames. The total must match the sample rate times the
       elapsed time to within one frame - not approximately, exactly, because
       the arithmetic is integer and the remainder is carried. */
    {
        const unsigned long VF = 60u * 60u * 60u;   /* ~1 hour of video frames */
        unsigned long long total = 0;
        unsigned lo = 0xFFFFFFFFu, hi = 0;

        tg_audio_clock_init(&c, 48000);
        for (unsigned long i = 0; i < VF; i++) {
            unsigned n = tg_audio_clock_next(&c);

            total += n;
            if (n < lo) lo = n;
            if (n > hi) hi = n;
        }

        /* What the sink will have consumed over the same span. */
        {
            /* seconds = VF * FPS_DEN / FPS_NUM, so frames = rate * that. */
            unsigned long long want =
                (unsigned long long)48000 * VF * TG_FPS_DEN / TG_FPS_NUM;
            unsigned long long diff = total > want ? total - want : want - total;

            ok("an hour of frames lands within one frame of the sink",
               diff <= 1);
        }

        ok("never hands out a silly number", lo >= 800 && hi <= 810);
        /* Both values must actually occur, or the remainder is not being
           carried and this is just a constant with extra steps. */
        ok("uses both 803 and 804", lo != hi);
    }

    /*
     * A different rate has to work too - RetailOS's mixer is 22050.
     *
     * Sixty video frames is 1.0046 seconds, not one, because the Game Boy runs
     * at 59.7275: the answer is 22151 and not 22050. Writing the expectation
     * as "about a second's worth" got this wrong on the first attempt, which
     * is a decent illustration of why the whole file exists.
     */
    {
        unsigned long long total = 0;
        unsigned long long want =
            (unsigned long long)22050 * 60 * TG_FPS_DEN / TG_FPS_NUM;

        tg_audio_clock_init(&c, 22050);
        for (unsigned i = 0; i < 60; i++) total += tg_audio_clock_next(&c);
        ok("22050 for RetailOS is exact too",
           total >= want && total <= want + 1);
    }
}

int main(void)
{
    test_header();
    test_registry();
    test_run();
    test_scale();
    test_audio_clock();

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}

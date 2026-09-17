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
#include "../platform/tg_audq.h"
#include "../platform/tg_pad.h"
#include "../platform/tg_palette.h"
#include "../platform/tg_scale.h"
#include "../platform/tg_text.h"
#include "../platform/tg_tilt.h"
#include "../platform/tg_util.h"

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
/*
 * Save, run on, load, and end up where the save was.
 *
 * The failure this catches is the one that cannot be seen by looking: a state
 * that restores MOST of the machine leaves a game that plays for a few
 * seconds and then diverges, which reads as emulator inaccuracy rather than
 * as a broken save. Running both paths forward from the same state and
 * comparing frames is what tells the two apart.
 */
static void test_state(void)
{
    const tg_core *c = tg_core_by_id("peanut");
    uint8_t *rom = make_rom(0x00, 0x00, 0x00, "STATE");
    void *ctx;
    uint8_t *blob, *frame_a;
    size_t need;
    int same = 1;

    printf("save states:\n");

    if (!c) { fails++; free(rom); return; }

    ok("the core declares them", (c->caps & TG_CAP_STATE) != 0 &&
                                 c->state_size && c->state_save && c->state_load);
    if (!c->state_size) { free(rom); return; }

    ctx = calloc(1, c->ctx_size);
    if (!ctx) { fails++; free(rom); return; }
    if (c->open(ctx, rom, ROM_BYTES, NULL, 0, 0) != TG_OK) {
        fails++; free(ctx); free(rom); return;
    }

    c->set_buttons(ctx, 0);
    for (int i = 0; i < 20; i++) c->run_frame(ctx);

    need = c->state_size(ctx);
    ok("asks for a sensible size", need > sizeof(void *) && need < (4u << 20));

    blob = malloc(need);
    frame_a = malloc((size_t)TG_W * TG_H);
    if (!blob || !frame_a) {
        fails++; free(blob); free(frame_a); free(ctx); free(rom); return;
    }

    ok("refuses a buffer that is too small",
       c->state_save(ctx, blob, need - 1) == TG_ERR_CAPACITY);
    ok("saves", c->state_save(ctx, blob, need) == TG_OK);

    /* Where the machine gets to from the save, kept to compare against. */
    for (int i = 0; i < 30; i++) c->run_frame(ctx);
    memcpy(frame_a, c->pixels(ctx), (size_t)TG_W * TG_H);

    /* Somewhere else entirely, so a load that does nothing cannot pass. */
    for (int i = 0; i < 60; i++) c->run_frame(ctx);

    ok("loads", c->state_load(ctx, blob, need) == TG_OK);
    for (int i = 0; i < 30; i++) c->run_frame(ctx);

    for (size_t i = 0; i < (size_t)TG_W * TG_H; i++)
        if (c->pixels(ctx)[i] != frame_a[i]) { same = 0; break; }
    ok("the same thirty frames come out again", same);

    /* Rubbish must be refused rather than restored into the machine. */
    blob[0] ^= 0xFFu;
    ok("refuses a state it did not write",
       c->state_load(ctx, blob, need) == TG_ERR_STATE);
    ok("refuses a truncated state",
       c->state_load(ctx, blob, 3) == TG_ERR_STATE);

    free(frame_a);
    free(blob);
    free(ctx);
    free(rom);
}

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

/* ---- the tilt d-pad ------------------------------------------------------ */

/*
 * The accelerometer as a d-pad.
 *
 * Every failure here is quiet. An uncalibrated neutral holds a direction down
 * forever and looks like a stuck button; a single threshold chatters the
 * direction on and off many times a second and looks like a flaky game; a
 * confused pair of axes gives controls that are merely baffling. None of it
 * shows up as a crash, and none of it can be checked by tilting a device
 * remotely - so it is checked here.
 *
 * The numbers are this device's: the LIS3LV02DL reports +-2304 for +-2 g, and
 * it rested at Y = -1086 on the bench, which is most of a g down the long
 * axis.
 */
/*
 * The tilt d-pad, on the device's real numbers.
 *
 * Full scale is +-2304 for +-2 g, so one gravity is 1152 counts. That matters
 * more than it looks: an axis already reading a full g is pointing at the
 * floor and has nowhere left to travel, which is why the choice of axis is
 * part of what is tested here rather than a constant in the source.
 */
static void test_tilt(void)
{
    enum { MIN = -2304, MAX = 2304, G = 1152 };
    tg_tilt t;

    printf("tilt d-pad:\n");

    /*
     * Upright: gravity down the long axis, which is how this device rests on
     * the bench and how somebody holds it to play. Y is pinned near -1 g, so
     * X and Z are the two that can answer.
     */
    tg_tilt_init(&t, MIN, MAX, 14, 7);
    tg_tilt_set_centre(&t, 11, -1086, 40);

    ok("held upright, the pinned axis is left out",
       t.roll_axis == TG_AX_X && t.pitch_axis == TG_AX_Z);

    ok("level presses nothing", tg_tilt_feed(&t, 11, -1086, 40) == 0);

    /*
     * Left and right, the way round the device actually is.
     *
     * Leaning it left raises X here, which is the opposite of what the first
     * version assumed - and playing it was how that was found. 14% of 2304 is
     * 322 counts, so 400 is past the press angle.
     */
    ok("lean left",  tg_tilt_feed(&t, 11 + 400, -1086, 40) == TG_LEFT);
    ok("lean right", tg_tilt_feed(&t, 11 - 400, -1086, 40) == TG_RIGHT);

    /* Tipping reads Z, which at this posture is the axis with room to move. */
    ok("tip up",   tg_tilt_feed(&t, 11, -1086, 40 + 400) == TG_UP);
    ok("tip down", tg_tilt_feed(&t, 11, -1086, 40 - 400) == TG_DOWN);

    ok("a diagonal is two directions",
       tg_tilt_feed(&t, 11 + 400, -1086, 40 + 400) == (TG_LEFT | TG_UP));

    /*
     * The bug this replaced, stated as a test.
     *
     * Y rests at -1086 and gravity stops at -1152, so Y has sixty-six counts
     * of travel left in that direction against a press threshold of 322.
     * Reading tip from Y could never fire, and it never did.
     */
    ok("the saturated axis is not the one being asked",
       t.pitch_axis != TG_AX_Y);
    ok("...and it could not have answered anyway", G - 1086 < (MAX * 14 / 100));

    /*
     * Calibration still earns its keep.
     *
     * Held upright the default axes happen to miss the pinned one, so the
     * bench position is harmless - but laid flat it is the default pitch axis
     * that is sitting at a full g, and an uncalibrated d-pad holds a
     * direction from the moment it starts.
     */
    tg_tilt_init(&t, MIN, MAX, 14, 7);
    ok("uncalibrated, resting flat holds a direction",
       tg_tilt_feed(&t, 5, -20, -1120) != 0);

    /*
     * Flat on a table is the other posture people use, and there gravity is
     * down Z - so the pair becomes X and Y and tipping reads Y again.
     */
    tg_tilt_init(&t, MIN, MAX, 14, 7);
    tg_tilt_set_centre(&t, 5, -20, -1120);
    ok("laid flat, the pair is the other two",
       t.roll_axis == TG_AX_X && t.pitch_axis == TG_AX_Y);
    ok("and tipping works there too",
       tg_tilt_feed(&t, 5, -20 + 400, -1120) == TG_UP);

    /*
     * Hysteresis. Held at 250 counts - between the 7% release angle (161) and
     * the 14% press angle (322) - the answer must depend on what came before,
     * or a hand resting near the threshold toggles many times a second.
     */
    tg_tilt_init(&t, MIN, MAX, 14, 7);
    tg_tilt_set_centre(&t, 11, -1086, 40);
    tg_tilt_feed(&t, 11, -1086, 40);
    ok("just under the press angle does not press",
       tg_tilt_feed(&t, 11 + 250, -1086, 40) == 0);

    tg_tilt_feed(&t, 11 + 400, -1086, 40);
    ok("...but having pressed, it stays pressed there",
       tg_tilt_feed(&t, 11 + 250, -1086, 40) == TG_LEFT);

    ok("and releases below the release angle",
       tg_tilt_feed(&t, 11 + 100, -1086, 40) == 0);

    /* A caller that asks for nonsense gets hysteresis anyway rather than a
       release angle above the press angle, which would latch forever. */
    tg_tilt_init(&t, MIN, MAX, 10, 50);
    ok("a release angle above the press angle is corrected",
       t.off_pct < t.on_pct);

    /* A device with no usable range must not divide by it. */
    tg_tilt_init(&t, 0, 0, 14, 7);
    ok("a zero range presses nothing", tg_tilt_feed(&t, 999, 999, 999) == 0);
}

/* ---- the palettes ------------------------------------------------------- */

static void test_palette(void)
{
    printf("palettes:\n");

    ok("there are five",                  tg_palette_count() == 5);
    ok("DMG is the default",              strcmp(tg_palette_at(0)->name, "DMG") == 0);
    ok("DMG is the panel's green",        tg_palette_at(0)->shade[0] == 0x9BBC0Fu &&
                                          tg_palette_at(0)->shade[3] == 0x0F380Fu);
    ok("found by name",                   tg_palette_index("Ink") == 4);
    ok("an unknown name is the default",  tg_palette_index("Sepia") == 0);
    ok("NULL is the default",             tg_palette_index(NULL) == 0);
    ok("out of range clamps to default",  tg_palette_at(99) == tg_palette_at(0));

    /* Lightest first is the order the shade index means, and a palette that
       got it backwards would draw every game as a negative. */
    for (unsigned i = 0; i < tg_palette_count(); i++) {
        const uint32_t *s = tg_palette_at(i)->shade;
        unsigned l0 = (s[0] & 0xFF) + ((s[0] >> 8) & 0xFF) + ((s[0] >> 16) & 0xFF);
        unsigned l3 = (s[3] & 0xFF) + ((s[3] >> 8) & 0xFF) + ((s[3] >> 16) & 0xFF);
        char what[64];
        snprintf(what, sizeof what, "%s runs light to dark", tg_palette_at(i)->name);
        ok(what, l0 > l3);
    }
}

/* ---- the top byte, for a compositor that reads it ----------------------- */

static void test_scale_alpha(void)
{
    static tg_scaler s;
    static uint32_t dst[TG_SCALED_W * TG_SCALED_H];
    static uint8_t src[TG_W * TG_H];
    uint32_t pal[4] = { 0xFF9BBC0Fu, 0xFF8BAC0Fu, 0xFF306230u, 0xFF0F380Fu };
    unsigned bad = 0;

    printf("scaler alpha:\n");

    /* Every shade and every pair of shades, so every table entry is used. */
    for (unsigned i = 0; i < sizeof src; i++)
        src[i] = (uint8_t)((i * 7 + i / TG_W) & 3);

    tg_scaler_init(&s, pal, true);
    tg_scale_15(&s, dst, TG_SCALED_W, src);
    for (unsigned i = 0; i < TG_SCALED_W * TG_SCALED_H; i++)
        if ((dst[i] >> 24) != 0xFFu) bad++;
    ok("smooth: every pixel keeps the palette's top byte", bad == 0);

    bad = 0;
    tg_scaler_init(&s, pal, false);
    tg_scale_15(&s, dst, TG_SCALED_W, src);
    for (unsigned i = 0; i < TG_SCALED_W * TG_SCALED_H; i++)
        if ((dst[i] >> 24) != 0xFFu) bad++;
    ok("sharp: every pixel keeps the palette's top byte", bad == 0);

    /* And a palette without one gets none: N31 hands over 0x00RRGGBB and
       the picture it captures back must still match the host byte for byte. */
    pal[0] &= 0x00FFFFFFu; pal[1] &= 0x00FFFFFFu;
    pal[2] &= 0x00FFFFFFu; pal[3] &= 0x00FFFFFFu;
    bad = 0;
    tg_scaler_init(&s, pal, true);
    tg_scale_15(&s, dst, TG_SCALED_W, src);
    for (unsigned i = 0; i < TG_SCALED_W * TG_SCALED_H; i++)
        if ((dst[i] >> 24) != 0) bad++;
    ok("a palette without one adds none", bad == 0);
}

/* ---- text ----------------------------------------------------------------- */

static unsigned count_px(const uint32_t *px, unsigned n, uint32_t c)
{
    unsigned k = 0;

    for (unsigned i = 0; i < n; i++) if (px[i] == c) k++;
    return k;
}

static void test_text(void)
{
    static uint32_t buf[64 * 40];
    tg_surface s = { buf, 64, 40, 64, 0 };
    const tg_font *f = &tg_font_ui;

    printf("text:\n");

    ok("the ui font covers printable ASCII",
       f->first == 32 && f->count == 95 && f->w >= 6 && f->h >= 10);
    ok("width is cells times characters",
       tg_text_width(f, "abc", 1) == 3u * f->w && tg_text_width(f, "abc", 2) == 6u * f->w);
    ok("fit counts whole cells", tg_text_fit(f, "abcdef", 3u * f->w + 2, 1) == 3);

    /* Every glyph has some ink, except space, and none has ink outside
       its cell - which is what a table with a row missing would show. */
    {
        unsigned blank = 0, wide = 0;

        for (unsigned g = 0; g < f->count; g++) {
            unsigned ink = 0;

            for (unsigned r = 0; r < f->h; r++) {
                uint16_t bits = f->rows[g * f->h + r];

                if (bits >> f->w) wide++;
                if (bits) ink++;
            }
            if (!ink && g + f->first != ' ') blank++;
        }
        ok("every glyph but space has ink", blank == 0);
        ok("no glyph spills past its cell", wide == 0);
    }

    /* Drawn where asked, and only there. */
    memset(buf, 0, sizeof buf);
    tg_text_draw(&s, f, 10, 5, "I", 1, 0x00FFFFFFu);
    {
        unsigned outside = 0;

        for (unsigned y = 0; y < 40; y++)
            for (unsigned x = 0; x < 64; x++)
                if (buf[y * 64 + x] &&
                    (x < 10 || x >= 10u + f->w || y < 5 || y >= 5u + f->h))
                    outside++;
        ok("ink lands inside the cell", outside == 0 && count_px(buf, 64 * 40, 0x00FFFFFFu) > 0);
    }

    /* Clipping: text hanging off every edge writes nothing outside. */
    memset(buf, 0, sizeof buf);
    tg_text_draw(&s, f, -5, -5, "WW", 1, 0x00FFFFFFu);
    tg_text_draw(&s, f, 60, 35, "WW", 2, 0x00FFFFFFu);
    ok("clipped at the edges without dying", count_px(buf, 64 * 40, 0x00FFFFFFu) > 0);

    /* The or_mask reaches the pixels. */
    s.or_mask = 0xFF000000u;
    memset(buf, 0, sizeof buf);
    tg_text_draw(&s, f, 0, 0, "A", 1, 0x00123456u);
    ok("the surface's mask is applied", count_px(buf, 64 * 40, 0xFF123456u) > 0 &&
                                        count_px(buf, 64 * 40, 0x00123456u) == 0);
}

/* ---- the pad -------------------------------------------------------------- */

static void test_pad(void)
{
    static uint32_t buf[240 * 432];
    tg_surface s = { buf, 240, 432, 240, 0 };

    printf("pad:\n");

    ok("the picture is not a control",  tg_pad_hit(120, 100) == 0);
    ok("a gap is nothing",              tg_pad_hit(2, 218) == 0);
    ok("d-pad up",                      tg_pad_hit(6 + 60, 248 + 20) == TG_UP);
    ok("d-pad down",                    tg_pad_hit(6 + 60, 248 + 100) == TG_DOWN);
    ok("d-pad left",                    tg_pad_hit(6 + 20, 248 + 60) == TG_LEFT);
    ok("d-pad right",                   tg_pad_hit(6 + 100, 248 + 60) == TG_RIGHT);
    ok("a corner is two directions",    tg_pad_hit(6 + 100, 248 + 100) == (TG_RIGHT | TG_DOWN));
    ok("the centre is the dead spot",   tg_pad_hit(6 + 60, 248 + 60) == 0);
    ok("A",                             tg_pad_hit(208, 274) == TG_A);
    ok("B",                             tg_pad_hit(168, 326) == TG_B);
    ok("a near miss on A still counts", tg_pad_hit(208 + 30, 274) == TG_A);
    ok("Select",                        tg_pad_hit(36 + 10, 386 + 10) == TG_SELECT);
    ok("Start",                         tg_pad_hit(132 + 10, 386 + 10) == TG_START);
    ok("the menu pill",                 tg_pad_hit(120, 216 + 16) == TG_PAD_MENU);
    ok("the pill is not a joypad bit",  (tg_pad_hit(120, 216 + 16) & 0xFF) == 0);

    /* Drawing writes the pad half and nothing above it. */
    memset(buf, 0x11, sizeof buf);
    tg_pad_invalidate();
    tg_pad_draw(&s, 0, true);
    ok("nothing above the pad is touched", count_px(buf, 240 * 216, 0x11111111u) == 240 * 216);
    ok("the pad half was painted",        count_px(buf + 240 * 216, 240 * 216, 0x11111111u) == 0);

    /* The same mask again writes nothing; a new one writes. */
    memset(buf, 0x22, sizeof buf);
    tg_pad_draw(&s, 0, false);
    ok("an unchanged mask is a no-op",    count_px(buf, 240 * 432, 0x22222222u) == 240 * 432);
    tg_pad_draw(&s, TG_A, false);
    ok("a changed mask repaints",         count_px(buf + 240 * 216, 240 * 216, 0x22222222u) == 0);
}

/* ---- the string helpers --------------------------------------------------- */

static void test_util(void)
{
    char buf[8];

    printf("util:\n");

    ok("utoa",                tg_utoa(1048576, buf, sizeof buf) && strcmp(buf, "1048576") == 0);
    ok("utoa zero",           strcmp(tg_utoa(0, buf, sizeof buf), "0") == 0);
    ok("utoa bounded",        strcmp(tg_utoa(123456789, buf, 4), "123") == 0);
    ok("itoa negative",       strcmp(tg_itoa(-42, buf, sizeof buf), "-42") == 0);
    ok("strlcpy truncates",   tg_strlcpy(buf, "abcdefghij", 4) == 10u && strcmp(buf, "abc") == 0);
    ok("strlcat appends",     (tg_strlcpy(buf, "ab", sizeof buf), tg_strlcat(buf, "cd", sizeof buf)) == 4u && strcmp(buf, "abcd") == 0);
    ok("strlcat bounded",     (tg_strlcpy(buf, "abcdef", sizeof buf), tg_strlcat(buf, "ghij", sizeof buf)) == 10u && strcmp(buf, "abcdefg") == 0);
    ok("strrchr",             tg_strrchr("a/b.c/d.gb", '.') && strcmp(tg_strrchr("a/b.c/d.gb", '.'), ".gb") == 0 && tg_strrchr("abc", 'z') == NULL);
    ok("stricmp",             tg_stricmp("Tetris.GB", "tetris.gb") == 0 && tg_stricmp("a", "b") < 0);
    ok("ends with, any case", tg_ends_with_nocase("Zelda.Gb", ".gb") && !tg_ends_with_nocase("x.gbx", ".gb"));
    ok("strcasestr",          tg_strcasestr("Legend of Zelda", "ZELDA") != NULL && tg_strcasestr("abc", "d") == NULL);
}

/* ---- the audio queue, against a pretend audio task ----------------------- */

/*
 * The fake OS: a clock, one voice, and the two things the real audio task
 * does that the queue depends on - it sets a descriptor's +0x64 while it
 * holds it, and at the end of the buffer follows the chain and sets the
 * next one's. On a 64-bit host a pointer does not fit the +0x54 word, so
 * the chain lives in a side table keyed by descriptor.
 */
#define FAKE_N 16
static struct { uint8_t *desc, *next; } s_links[FAKE_N];
static uint32_t s_fake_ms;
static uint8_t *s_fake_cur;          /* the descriptor sounding, or NULL */
static uint32_t s_fake_cur_start;
static unsigned s_fake_plays;
static uint32_t s_fake_voice_rate = 22050;
static uint32_t s_fake_voice[0x20];  /* +0x0C rate, +0x48 counter, as words */

static uint8_t *fake_next_of(const uint8_t *d)
{
    for (int i = 0; i < FAKE_N; i++)
        if (s_links[i].desc == d) return s_links[i].next;
    return NULL;
}

static void fake_ctor(uint8_t *d) { memset(d, 0, TG_AUDQ_DESC_BYTES); }

static void fake_set_next(uint8_t *d, uint8_t *next)
{
    for (int i = 0; i < FAKE_N; i++)
        if (s_links[i].desc == d || !s_links[i].desc) {
            s_links[i].desc = d; s_links[i].next = next; return;
        }
}

static void fake_take_up(uint8_t *d)
{
    s_fake_cur = d;
    s_fake_cur_start = s_fake_ms;
    d[TG_SFX_OFF_PLAYING] = 1;
    s_fake_voice[0x0C / 4] = s_fake_voice_rate;
    /* setSource: the voice pointer lands in the descriptor. */
    memcpy(d + TG_SFX_OFF_VOICE, &(uint32_t){ 0x08800000u }, 4);
}

static bool fake_play(uint8_t *d) { s_fake_plays++; fake_take_up(d); return true; }

static uint32_t fake_voice_rate(const uint8_t *d)
{
    uint32_t v; memcpy(&v, d + TG_SFX_OFF_VOICE, 4);
    return v ? s_fake_voice[0x0C / 4] : 0;
}

static void fake_voice_set_frames(const uint8_t *d, uint32_t frames)
{
    (void)d; s_fake_voice[0x48 / 4] = frames;
}

static uint32_t fake_now_ms(void) { return s_fake_ms; }

static const tg_audq_ops k_fake_ops = {
    fake_ctor, fake_set_next, fake_play, fake_voice_rate,
    fake_voice_set_frames, fake_now_ms,
};

/* The audio task, run forward `ms`: buffers end, chains are followed. */
static void fake_advance(uint32_t ms)
{
    s_fake_ms += ms;
    while (s_fake_cur) {
        uint32_t len; memcpy(&len, s_fake_cur + TG_SFX_OFF_BUF_LEN, 4);
        uint32_t block_ms = (len / 4u) * 1000u / 22050u;

        if (s_fake_ms - s_fake_cur_start < block_ms) break;
        s_fake_cur[TG_SFX_OFF_PLAYING] = 0;
        {
            uint8_t *next = fake_next_of(s_fake_cur);
            uint32_t start = s_fake_cur_start + block_ms;

            s_fake_cur = NULL;
            if (next) { fake_take_up(next); s_fake_cur_start = start; }
        }
    }
}

static void fake_reset(void)
{
    memset(s_links, 0, sizeof s_links);
    s_fake_ms = 1000; s_fake_cur = NULL; s_fake_plays = 0;
    s_fake_voice_rate = 22050;
}

static void push_const(tg_audq *q, unsigned frames, int16_t v)
{
    static int16_t buf[512 * 2];

    while (frames) {
        unsigned n = frames < 512 ? frames : 512;

        for (unsigned i = 0; i < n * 2; i++) buf[i] = v;
        tg_audq_push(q, buf, n);
        frames -= n;
    }
}

static void test_audq(void)
{
    static int16_t pool[TG_AUDQ_POOL_SAMPLES];
    static tg_audq q;
    const unsigned S = TG_AUDQ_SLOT_FRAMES;

    printf("audio queue:\n");

    fake_reset();
    tg_audq_init(&q, &k_fake_ops, pool, 0x4000);

    ok("a slot is a multiple of 441 frames",  S % 441 == 0);
    ok("empty, it wants the whole target",
       tg_audq_frames_wanted(&q) == (int)((TG_AUDQ_TARGET_LEAD + 1) * S));

    /* The first block goes out on a fresh voice; the second is only linked. */
    push_const(&q, S - 1, 1000);
    ok("nothing is handed over before a slot is full", s_fake_plays == 0 && q.qn == 0);
    push_const(&q, 1, 1000);
    ok("a full slot is sealed and played",            s_fake_plays == 1 && q.qn == 1);
    ok("the voice's counter was corrected",           s_fake_voice[0x48 / 4] == S);
    ok("the mixer's rate was learned",                q.out_rate == 22050);
    push_const(&q, S, 1000);
    ok("the next block is chained, not played",
       s_fake_plays == 1 && q.qn == 2 && fake_next_of(q.slot[q.q[0]].desc) == q.slot[q.q[1]].desc);
    ok("its duration is stated exactly",
       *(uint32_t *)(q.slot[q.q[1]].desc + TG_SFX_OFF_DURATION) == 60);
    ok("the first went out with none",                q.zero_duration_joins == 1);

    /* Queued frames follow the clock. */
    fake_advance(30);
    {
        unsigned n = tg_audq_queued_frames(&q);
        ok("queued frames subtract what has played", n > S && n < 2 * S);
    }

    /* Reclaim is FIFO and waits to have seen the flag. */
    tg_audq_tick(&q);
    ok("a sounding block is not reclaimed",           q.qn == 2 && q.slot[q.q[0]].seen_playing);
    fake_advance(31);                     /* the first block ends at 60 ms */
    tg_audq_tick(&q);
    ok("a finished block is reclaimed and the next sounds",
       q.qn == 1 && q.slot[q.q[0]].desc[TG_SFX_OFF_PLAYING] == 1);

    /* Filling to the target stops the emulator being asked for more. */
    while (tg_audq_frames_wanted(&q) > 0) push_const(&q, 100, 500);
    ok("at the target it wants nothing",              tg_audq_frames_wanted(&q) <= 0);
    ok("never more than the slots can hold",          q.dropped_frames == 0);

    /* Pause: the chain is broken, the blocks behind the head are silent,
       the silent block loops itself. */
    {
        uint8_t *head = q.slot[q.q[0]].desc;
        uint8_t *second = q.qn > 1 ? q.slot[q.q[1]].desc : NULL;
        int16_t *second_pcm = q.qn > 1 ? q.slot[q.q[1]].pcm : NULL;
        unsigned plays = s_fake_plays;

        tg_audq_pause(&q);
        ok("pause breaks the chain",                  fake_next_of(head) == NULL);
        ok("pause silences the blocks behind the head",
           second && second_pcm[100] == 0 && fake_next_of(second) == NULL);
        ok("the quiet block is chained to itself",    fake_next_of(q.quiet.desc) == q.quiet.desc);
        ok("and started on its own voice",            s_fake_plays == plays + 1 && q.quiet_armed);
        ok("paused, it wants nothing",                tg_audq_frames_wanted(&q) == 0);
        ok("paused, pushes are dropped silently",     (push_const(&q, 10, 1), q.build < 0));
    }

    /* Resume: the next block fades in and is chained from the silence. */
    tg_audq_resume(&q);
    fake_advance(20);
    tg_audq_tick(&q);
    {
        unsigned plays = s_fake_plays;

        push_const(&q, S, 8000);
        ok("resume chains from the silence, no new voice",
           s_fake_plays == plays && fake_next_of(q.quiet.desc) == q.slot[q.q[0]].desc);
        ok("the first block after a resume fades in",
           q.slot[q.q[0]].pcm[0] < q.slot[q.q[0]].pcm[2 * 800] &&
           q.slot[q.q[0]].pcm[2 * (S - 1)] == 8000);
    }

    /* Starvation: a long gap means the OS took the mixer; start over. */
    fake_advance(5000);
    tg_audq_tick(&q);
    ok("a long gap drops the queue and counts a restart", q.qn == 0 && q.restarts == 1);

    /* Duration exactness at other mixer rates. */
    q.out_rate = 44100; q.duration_bad = false;
    ok("exact at 44100",     tg_audq_duration_ms(&q, S) == 30);
    q.out_rate = 48000;
    ok("not stated at 48000", tg_audq_duration_ms(&q, S) == 0);
    ok("the quiet block is exact where the slots are",
       (q.out_rate = 22050, tg_audq_duration_ms(&q, TG_AUDQ_QUIET_FRAMES) == 320));

    /* No free slot: frames are dropped and counted, never blocked on. */
    fake_reset();
    tg_audq_init(&q, &k_fake_ops, pool, 0x4000);
    push_const(&q, S * (TG_AUDQ_SLOTS + 1), 1);
    ok("pushing past the slots drops and counts", q.dropped_frames > 0 && q.qn == TG_AUDQ_SLOTS);
}

int main(void)
{
    test_header();
    test_registry();
    test_run();
    test_state();
    test_scale();
    test_scale_alpha();
    test_audio_clock();
    test_tilt();
    test_palette();
    test_text();
    test_pad();
    test_util();
    test_audq();

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}

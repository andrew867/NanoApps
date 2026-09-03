/*
 * tg_peanut.c — Peanut-GB behind tg_core.h.
 *
 * Peanut-GB is a single header that emulates an original Game Boy: no
 * allocation, no I/O, a callback per scanline, and a `gb_s` the caller owns.
 * That maps onto tg_core almost exactly, and the three places it does not are
 * all noted below.
 *
 * It is Game Boy only. Not partially colour, not colour with gaps - the
 * vendored source contains no CGB code path. Anything that reports cgb_only
 * needs the second core, which is what tg_core_for_rom is for.
 */

#include "tg_core.h"

#include <string.h>

/* Sound is compiled in, but the rate is baked at build time by minigb_apu (see
   the note on tg_peanut_audio_rate below), so a build with no APU is a real
   configuration rather than an oversight. */
#ifndef TG_WITH_SOUND
# define TG_WITH_SOUND 1
#endif

#if TG_WITH_SOUND
/*
 * minigb_apu is built for one sample format and one rate, both chosen at
 * compile time. The build sets both - S16 interleaved stereo, which is what
 * every sink here wants, and a rate that matches the sink so nothing resamples
 * on the hot path (48000 on Linux, 22050 under RetailOS). They have to come
 * from the build rather than from here because minigb_apu.c is compiled
 * separately and has to agree.
 */
# if !defined(MINIGB_APU_AUDIO_FORMAT_S16SYS)
#  error "build must define MINIGB_APU_AUDIO_FORMAT_S16SYS (see Makefile.host)"
# endif
# include "../vendor/minigb_apu.h"

/*
 * AUDIO_SAMPLES is a division by a double, so it is not a constant expression
 * and cannot size an array. This is an integer bound on it: the APU never
 * emits more than rate/59 frames in one callback, since the real frame rate is
 * 59.7275. At 48000 that reserves 814 where 803 are used.
 */
# define TG_APU_MAX_FRAMES ((AUDIO_SAMPLE_RATE) / 59u + 1u)
/* Peanut-GB calls bare audio_read/audio_write with no context argument, so the
   APU it talks to has to be reachable without one. Declared here, defined
   below over the open session. */
uint8_t audio_read(uint16_t addr);
void    audio_write(uint16_t addr, uint8_t val);
# define ENABLE_SOUND 1
#else
# define ENABLE_SOUND 0
#endif

#define ENABLE_LCD 1
#include "../vendor/peanut_gb.h"

/* ---- the session --------------------------------------------------------- */

typedef struct {
    struct gb_s gb;

    const uint8_t *rom;
    size_t         rom_len;
    uint8_t       *sram;
    size_t         sram_len;

    /* Peanut-GB hands us a scanline at a time; the front end wants a frame. */
    uint8_t frame[TG_W * TG_H];

    void (*serial_fn)(void *user, uint8_t byte);
    void  *serial_user;

#if TG_WITH_SOUND
    struct minigb_apu_ctx apu;
    unsigned  audio_rate;

    /*
     * minigb_apu produces exactly one video frame's worth per callback and no
     * more, so a pull that does not land on a frame boundary needs somewhere to
     * leave the remainder. One callback is the most that can ever be waiting,
     * because this is drained before it is refilled.
     */
    int16_t  spill[TG_APU_MAX_FRAMES * 2];
    unsigned spill_have;   /* frames of stereo audio sitting in spill */
    unsigned spill_taken;
#endif
} peanut_ctx;

#if TG_WITH_SOUND
/* The open session, for the context-free audio_read/audio_write above. TinyGB
   runs one emulator at a time by construction - there is one screen - so this
   is a statement of that, not a limitation being smuggled in. */
static peanut_ctx *s_audio_owner;

uint8_t audio_read(uint16_t addr)
{
    return s_audio_owner ? minigb_apu_audio_read(&s_audio_owner->apu, addr) : 0xFF;
}

void audio_write(uint16_t addr, uint8_t val)
{
    if (s_audio_owner) minigb_apu_audio_write(&s_audio_owner->apu, addr, val);
}
#endif

/* ---- what Peanut-GB asks of us ------------------------------------------- */

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    const peanut_ctx *c = gb->direct.priv;

    /* A cartridge may address beyond a short dump - a truncated file, or a
       mapper banking past the end. Real hardware floats high there; returning
       0xFF keeps a bad ROM to a glitch rather than a read out of bounds. */
    return addr < c->rom_len ? c->rom[addr] : 0xFF;
}

static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    const peanut_ctx *c = gb->direct.priv;

    return addr < c->sram_len ? c->sram[addr] : 0xFF;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                           const uint8_t val)
{
    peanut_ctx *c = gb->direct.priv;

    if (addr < c->sram_len) c->sram[addr] = val;
}

static void on_error(struct gb_s *gb, const enum gb_error_e err,
                     const uint16_t addr)
{
    (void)addr;

    /*
     * An invalid opcode is a dead machine, not a recoverable fault: the program
     * counter is somewhere it should never have reached and running on produces
     * noise. Reset rather than continue, so the failure is a game that restarts
     * instead of a screen that slowly fills with garbage.
     *
     * Invalid reads and writes are survivable and common in ROMs that poke at
     * unmapped space, so they are ignored on purpose.
     */
    if (err == GB_INVALID_OPCODE) gb_reset(gb);
}

static void draw_line(struct gb_s *gb, const uint8_t *pixels,
                      const uint_fast8_t line)
{
    peanut_ctx *c = gb->direct.priv;

    if (line < TG_H) memcpy(c->frame + (size_t)line * TG_W, pixels, TG_W);
}

static void serial_tx(struct gb_s *gb, const uint8_t tx)
{
    peanut_ctx *c = gb->direct.priv;

    if (c->serial_fn) c->serial_fn(c->serial_user, tx);
}

static enum gb_serial_rx_ret_e serial_rx(struct gb_s *gb, uint8_t *rx)
{
    (void)gb;
    (void)rx;
    /* Nothing is plugged into the other end of the cable. Saying so is what
       stops a cartridge waiting forever for a reply. */
    return GB_SERIAL_RX_NO_CONNECTION;
}

static void peanut_set_serial_sink(void *vctx,
                                   void (*fn)(void *user, uint8_t byte),
                                   void *user)
{
    peanut_ctx *c = vctx;

    c->serial_fn = fn;
    c->serial_user = user;
    gb_init_serial(&c->gb, fn ? serial_tx : NULL, fn ? serial_rx : NULL);
}

/* ---- tg_core ------------------------------------------------------------- */

static size_t peanut_sram_size_for(const uint8_t *rom, size_t rom_len)
{
    tg_rom_info info;

    if (tg_rom_probe(rom, rom_len, &info) != TG_OK) return 0;
    return info.sram_size;
}

static enum tg_result peanut_open(void *vctx, const uint8_t *rom, size_t rom_len,
                                  uint8_t *sram, size_t sram_len,
                                  unsigned audio_rate)
{
    peanut_ctx *c = vctx;
    tg_rom_info info;
    enum tg_result r;
    enum gb_init_error_e e;

    if (!c || !rom) return TG_ERR_ROM;

    if ((r = tg_rom_probe(rom, rom_len, &info)) != TG_OK) return r;
    if (info.cgb_only) return TG_ERR_UNSUPPORTED;
    if (info.sram_size > sram_len) return TG_ERR_CAPACITY;

    memset(c, 0, sizeof *c);
    c->rom = rom;
    c->rom_len = rom_len;
    c->sram = sram;
    c->sram_len = sram_len;

    /* priv has to be set before gb_init: gb_init reads the cartridge header
       through gb_rom_read, which reaches back through it. */
    c->gb.direct.priv = c;

    e = gb_init(&c->gb, rom_read, cart_ram_read, cart_ram_write, on_error, c);
    switch (e) {
    case GB_INIT_NO_ERROR:              break;
    case GB_INIT_CARTRIDGE_UNSUPPORTED: return TG_ERR_MAPPER;
    case GB_INIT_INVALID_CHECKSUM:      return TG_ERR_CHECKSUM;
    default:                            return TG_ERR_INTERNAL;
    }

    gb_init_lcd(&c->gb, draw_line);

#if TG_WITH_SOUND
    c->audio_rate = audio_rate;
    if (audio_rate) {
        minigb_apu_audio_init(&c->apu);
        s_audio_owner = c;
    }
#else
    (void)audio_rate;
#endif

    /* A blank frame rather than whatever was on the stack, so a front end that
       draws before the first run_frame shows white and not noise. */
    memset(c->frame, 0, sizeof c->frame);
    return TG_OK;
}

static void peanut_close(void *vctx)
{
#if TG_WITH_SOUND
    if (s_audio_owner == vctx) s_audio_owner = NULL;
#else
    (void)vctx;
#endif
}

static void peanut_reset(void *vctx)
{
    peanut_ctx *c = vctx;

    gb_reset(&c->gb);
#if TG_WITH_SOUND
    if (c->audio_rate) {
        minigb_apu_audio_init(&c->apu);
        c->spill_have = c->spill_taken = 0;
    }
#endif
}

static void peanut_set_buttons(void *vctx, uint8_t held)
{
    peanut_ctx *c = vctx;

    /* TG_* deliberately matches JOYPAD_*, but the register is active-low: a
       held button is a clear bit. */
    c->gb.direct.joypad = (uint8_t)~held;
}

static void peanut_run_frame(void *vctx)
{
    gb_run_frame(&((peanut_ctx *)vctx)->gb);
}

static const uint8_t *peanut_pixels(void *vctx)
{
    return ((peanut_ctx *)vctx)->frame;
}

/*
 * The default four shades, as a real DMG's green LCD renders them rather than
 * as grey. A front end is free to publish its own palette; this is what it gets
 * if it does not.
 */
static const uint32_t k_dmg_palette[4] = {
    0x9BBC0Fu,   /* 0 - lightest */
    0x8BAC0Fu,
    0x306230u,
    0x0F380Fu,   /* 3 - darkest */
};

static const uint32_t *peanut_palette(void *vctx, unsigned *count)
{
    (void)vctx;
    if (count) *count = 4;
    return k_dmg_palette;
}

#if TG_WITH_SOUND
static unsigned peanut_audio_pull(void *vctx, int16_t *dst, unsigned frames)
{
    peanut_ctx *c = vctx;
    unsigned done = 0;

    if (!c->audio_rate || !dst) return 0;

    while (done < frames) {
        unsigned avail = c->spill_have - c->spill_taken;
        unsigned take;

        if (avail == 0) {
            /*
             * One APU callback is one video frame of audio and the size is
             * fixed at build time, so the emulator's frame rate and the sink's
             * appetite have to be reconciled somewhere. Here: generate a block,
             * hand out what is asked for, keep the rest.
             *
             * The block is AUDIO_SAMPLES frames, which is the sample rate
             * divided by 59.7275 and truncated - at 48000 that is 803, and 803
             * frames per video frame is 47961 Hz, 0.08% slow. Over a minute
             * that is about 2300 frames of audio the sink wanted and did not
             * get. The frame pacer has to close that gap (Phase 02); this
             * function's job is only to never return short while it can help
             * it, so an underrun is visibly the pacer's problem and not a
             * mystery here.
             */
            minigb_apu_audio_callback(&c->apu, c->spill);
            c->spill_have = AUDIO_SAMPLES;
            c->spill_taken = 0;
            avail = c->spill_have;
        }

        take = frames - done;
        if (take > avail) take = avail;

        memcpy(dst + (size_t)done * 2,
               c->spill + (size_t)c->spill_taken * 2,
               (size_t)take * 2 * sizeof *dst);

        c->spill_taken += take;
        done += take;
    }
    return done;
}
#endif

const tg_core tg_core_peanut = {
    .id    = "peanut",
    .name  = "Peanut-GB",
    .blurb = "Small and fast. Original Game Boy only.",
    .caps  = TG_CAP_DMG
#if TG_WITH_SOUND
           | TG_CAP_SOUND
#endif
    ,
    .ctx_size      = sizeof(peanut_ctx),
    .sram_size_for = peanut_sram_size_for,
    .open          = peanut_open,
    .close         = peanut_close,
    .reset         = peanut_reset,
    .set_buttons   = peanut_set_buttons,
    .run_frame     = peanut_run_frame,
    .pixels        = peanut_pixels,
    .palette       = peanut_palette,
#if TG_WITH_SOUND
    .audio_pull    = peanut_audio_pull,
#else
    .audio_pull    = NULL,
#endif
    .set_serial_sink = peanut_set_serial_sink,
    /* Peanut-GB has no serialiser of its own. Save states are a second core's
       feature, or ours to add later against a stable gb_s layout. */
    .state_size = NULL,
    .state_save = NULL,
    .state_load = NULL,
};

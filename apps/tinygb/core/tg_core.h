/*
 * tg_core.h — the emulator, as seen by everything that is not the emulator.
 *
 * TinyGB ships one core today (Peanut-GB: small, fast, MIT, and Game Boy only)
 * and expects to gain another (SameBoy: accurate, MIT, and the realistic route
 * to Game Boy Color). Rather than write against Peanut-GB and pay for it later,
 * every front end talks to this header, and a core is chosen by name at run
 * time from the list tg_core_count()/tg_core_at() reports.
 *
 * That is not speculative generality. Peanut-GB as vendored has no CGB code at
 * all - not a partial implementation, none - so "add Game Boy Color" is
 * necessarily "add a second core", and the only question is whether the seam
 * exists before or after three front ends have been written against the first
 * one.
 *
 * The shape is deliberately narrow:
 *
 *   - No allocation. A core states how much context it needs and the caller
 *     provides it. RetailOS and Linux have very different ideas about the
 *     heap, and neither has to be encoded here.
 *   - No file I/O. ROM and save RAM arrive as bytes the caller owns and
 *     outlives the session.
 *   - No time. The caller decides when a frame happens; run_frame does one.
 *   - No audio device. audio_pull fills a buffer at the rate agreed at open,
 *     which is the same shape as en_audio_pull_fn in Entrain's platform/audio.h
 *     so the two can share a sink.
 */

#ifndef TINYGB_CORE_H
#define TINYGB_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Game Boy panel. Both models, both cores: this never changes. */
#define TG_W 160
#define TG_H 144

/* One emulated frame, exactly. 4194304 / 70224 - the number the audio rate and
   the frame pacer both have to agree with, and not 60. */
#define TG_FPS_NUM 4194304
#define TG_FPS_DEN 70224

/* Buttons, as a bitmask. Ordered to match the Game Boy's own joypad register so
   a core that wants the hardware layout gets it for free. */
enum {
    TG_A      = 1u << 0,
    TG_B      = 1u << 1,
    TG_SELECT = 1u << 2,
    TG_START  = 1u << 3,
    TG_RIGHT  = 1u << 4,
    TG_LEFT   = 1u << 5,
    TG_UP     = 1u << 6,
    TG_DOWN   = 1u << 7,
};

/* What a core can do. Checked before offering it, so the UI never lists a core
   that cannot run the cartridge in the slot. */
enum {
    TG_CAP_DMG   = 1u << 0,   /* original Game Boy */
    TG_CAP_CGB   = 1u << 1,   /* Game Boy Color */
    TG_CAP_SOUND = 1u << 2,
    TG_CAP_STATE = 1u << 3,   /* save states, as opposed to battery SRAM */
};

enum tg_result {
    TG_OK = 0,
    TG_ERR_ROM,          /* not a cartridge, or a header we cannot read */
    TG_ERR_MAPPER,       /* a memory bank controller this core lacks */
    TG_ERR_CHECKSUM,
    TG_ERR_NO_CORE,      /* no such core id */
    TG_ERR_CAPACITY,     /* caller's context or SRAM buffer is too small */
    TG_ERR_UNSUPPORTED,  /* a core declining something it does not claim */
    TG_ERR_INTERNAL,
};

const char *tg_strerror(enum tg_result r);

/*
 * Pixels.
 *
 * A frame is TG_W*TG_H bytes, one per pixel, and what a byte means depends on
 * the core. DMG cores report a shade in 0..3 plus, optionally, which palette
 * the pixel came from - the front end needs that to apply the twelve-colour
 * treatment a real Game Boy Color gives a DMG cartridge. A colour core reports
 * an index into the palette it publishes.
 *
 * Keeping this as indices rather than RGB is what lets the palette be a UI
 * setting and the scaler stay a byte-shuffle. Conversion happens once, in the
 * front end, at the point it also scales.
 */
#define TG_PX_SHADE   0x03u   /* 0 lightest .. 3 darkest */
#define TG_PX_OBJ     0x10u   /* pixel came from an object, not the background */
#define TG_PX_OBJ1    0x20u   /* ...and from OBJ palette 1 rather than 0 */

typedef struct tg_session tg_session;

/*
 * A core, as a table of functions.
 *
 * `ctx_size` is how many bytes of scratch the core wants; the caller passes a
 * buffer of at least that size to tg_open and must keep it alive and unmoved
 * for the session. `sram_size_for` lets the caller size the battery-backed save
 * from the cartridge header before committing to a session.
 */
typedef struct tg_core {
    const char *id;       /* stable, lowercase, used in config: "peanut" */
    const char *name;     /* shown to a person: "Peanut-GB" */
    const char *blurb;    /* one line, shown under the name in the core picker */
    unsigned    caps;

    size_t ctx_size;

    /* How much cart RAM this ROM needs. 0 is a valid answer. */
    size_t (*sram_size_for)(const uint8_t *rom, size_t rom_len);

    /* Take up a cartridge. `rom` and `sram` stay owned by the caller and must
       outlive the session. `audio_rate` is what audio_pull will be asked for;
       0 means the caller does not want sound. */
    enum tg_result (*open)(void *ctx, const uint8_t *rom, size_t rom_len,
                           uint8_t *sram, size_t sram_len,
                           unsigned audio_rate);
    void (*close)(void *ctx);

    /* Power cycle, same cartridge. */
    void (*reset)(void *ctx);

    /* Held buttons, as TG_* bits. Latched until the next call. */
    void (*set_buttons)(void *ctx, uint8_t held);

    /* Advance exactly one frame. */
    void (*run_frame)(void *ctx);

    /* The frame just produced: TG_W*TG_H bytes, valid until the next
       run_frame. */
    const uint8_t *(*pixels)(void *ctx);

    /* The palette `pixels` indexes into, as 0x00RRGGBB, and how many entries.
       A DMG core returns 4 (or 12 when it distinguishes object palettes). */
    const uint32_t *(*palette)(void *ctx, unsigned *count);

    /* Fill `frames` frames of interleaved 16-bit stereo at the agreed rate.
       Returns frames written. NULL when the core has no sound. */
    unsigned (*audio_pull)(void *ctx, int16_t *dst, unsigned frames);

    /* The link port, as an output. A cartridge writing to the serial register
       calls `fn` with each byte; this is how a Game Boy talks to another Game
       Boy, and it is also how every Game Boy test ROM ever written reports its
       results. NULL when a core has no link port. */
    void (*set_serial_sink)(void *ctx, void (*fn)(void *user, uint8_t byte),
                            void *user);

    /* Save state. Both NULL when TG_CAP_STATE is absent. `size` is queried by
       passing NULL for `dst`. */
    size_t (*state_size)(void *ctx);
    enum tg_result (*state_save)(void *ctx, void *dst, size_t cap);
    enum tg_result (*state_load)(void *ctx, const void *src, size_t len);
} tg_core;

/* ---- the registry -------------------------------------------------------- */

unsigned       tg_core_count(void);
const tg_core *tg_core_at(unsigned i);
const tg_core *tg_core_by_id(const char *id);

/* The best core for this cartridge: the first registered one whose caps cover
   what the header asks for. Returns NULL if nothing can run it. */
const tg_core *tg_core_for_rom(const uint8_t *rom, size_t rom_len);

/* ---- reading a cartridge without starting one ---------------------------- */

typedef struct {
    char     title[17];   /* NUL-terminated */
    bool     cgb;         /* the cartridge wants Game Boy Color */
    bool     cgb_only;    /* ...and will not run without it */
    bool     sgb;
    uint8_t  mapper_code; /* header byte 0x0147, verbatim */
    size_t   rom_size;    /* as declared by the header, not the file length */
    size_t   sram_size;
    uint8_t  header_checksum;
    bool     header_ok;   /* the 0x134..0x14C checksum agrees */
} tg_rom_info;

enum tg_result tg_rom_probe(const uint8_t *rom, size_t rom_len,
                            tg_rom_info *out);

#endif /* TINYGB_CORE_H */

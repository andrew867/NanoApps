/*
 * tg_core.c — the registry, and reading a cartridge header.
 *
 * The header read lives here rather than in a core because it is the thing you
 * need BEFORE choosing a core: which model the cartridge wants decides which
 * cores can run it, and how much save RAM to allocate decides whether a session
 * can start at all. Asking a core to answer that would mean opening one to find
 * out whether to open it.
 */

#include "tg_core.h"

#include <string.h>

/* Each backend defines one of these. Adding a core is adding an extern and a
   line to the table - deliberately the smallest change that can add one. */
extern const tg_core tg_core_peanut;

static const tg_core *const k_cores[] = {
    &tg_core_peanut,
};

#define N_CORES ((unsigned)(sizeof k_cores / sizeof k_cores[0]))

unsigned tg_core_count(void) { return N_CORES; }

const tg_core *tg_core_at(unsigned i)
{
    return i < N_CORES ? k_cores[i] : NULL;
}

const tg_core *tg_core_by_id(const char *id)
{
    if (!id) return NULL;
    for (unsigned i = 0; i < N_CORES; i++)
        if (!strcmp(k_cores[i]->id, id)) return k_cores[i];
    return NULL;
}

const tg_core *tg_core_for_rom(const uint8_t *rom, size_t rom_len)
{
    tg_rom_info info;

    if (tg_rom_probe(rom, rom_len, &info) != TG_OK) return NULL;

    /*
     * A cartridge that merely *supports* colour still runs on a DMG core - that
     * is the whole point of the compatibility byte - so only cgb_only rules a
     * DMG core out. Getting this backwards would hide every dual-mode cartridge
     * from the one core we currently ship.
     */
    for (unsigned i = 0; i < N_CORES; i++) {
        unsigned caps = k_cores[i]->caps;

        if (info.cgb_only && !(caps & TG_CAP_CGB)) continue;
        if (!info.cgb_only && !(caps & (TG_CAP_DMG | TG_CAP_CGB))) continue;
        return k_cores[i];
    }
    return NULL;
}

const char *tg_strerror(enum tg_result r)
{
    switch (r) {
    case TG_OK:              return "ok";
    case TG_ERR_ROM:         return "not a Game Boy cartridge";
    case TG_ERR_MAPPER:      return "this core does not support that cartridge's mapper";
    case TG_ERR_CHECKSUM:    return "the cartridge header checksum does not match";
    case TG_ERR_NO_CORE:     return "no such core";
    case TG_ERR_CAPACITY:    return "the buffer provided is too small";
    case TG_ERR_UNSUPPORTED: return "the core does not support that";
    case TG_ERR_INTERNAL:    return "internal error";
    }
    return "unknown error";
}

/* ---- the cartridge header ------------------------------------------------ */

/* Declared ROM size is 32 KiB << byte 0x0148. */
#define HDR_TITLE      0x0134
#define HDR_CGB        0x0143
#define HDR_SGB        0x0146
#define HDR_MAPPER     0x0147
#define HDR_ROM_SIZE   0x0148
#define HDR_RAM_SIZE   0x0149
#define HDR_CHECKSUM   0x014D
#define HDR_END        0x0150

/* Byte 0x0149 is a code, not a size, and the codes are not a sequence: 0x01 is
   an unofficial 2 KiB that some homebrew uses, and 0x05 is smaller than 0x04.
   A shift would get two of these wrong. */
static size_t ram_size_from_code(uint8_t code)
{
    switch (code) {
    case 0x00: return 0;
    case 0x01: return 2 * 1024;
    case 0x02: return 8 * 1024;
    case 0x03: return 32 * 1024;
    case 0x04: return 128 * 1024;
    case 0x05: return 64 * 1024;
    default:   return 0;
    }
}

/* Mappers that carry battery-backed RAM regardless of what byte 0x0149 says.
   MBC2 has 512 nibbles built into the chip and declares a RAM size of zero. */
static bool mapper_has_builtin_ram(uint8_t code)
{
    return code == 0x05 || code == 0x06;   /* MBC2, MBC2+BATTERY */
}

enum tg_result tg_rom_probe(const uint8_t *rom, size_t rom_len,
                            tg_rom_info *out)
{
    uint8_t sum = 0;
    uint8_t cgb;

    if (!rom || !out || rom_len < HDR_END) return TG_ERR_ROM;

    memset(out, 0, sizeof *out);

    /*
     * The title field ran to 0x0143 originally, then shrank as Nintendo took
     * bytes off the end for the CGB flag and the manufacturer code. Reading the
     * full sixteen on a colour cartridge picks up those bytes as text, so stop
     * at fifteen when the CGB flag is set and trim trailing padding either way.
     */
    cgb = rom[HDR_CGB];
    out->cgb      = (cgb & 0x80) != 0;
    out->cgb_only = cgb == 0xC0;

    {
        size_t n = out->cgb ? 15 : 16;

        memcpy(out->title, rom + HDR_TITLE, n);
        out->title[n] = 0;
        while (n > 0 && (out->title[n - 1] == ' ' || out->title[n - 1] == 0))
            out->title[--n] = 0;
    }

    out->sgb         = rom[HDR_SGB] == 0x03;
    out->mapper_code = rom[HDR_MAPPER];
    out->rom_size    = (size_t)32 * 1024 << rom[HDR_ROM_SIZE];
    out->sram_size   = ram_size_from_code(rom[HDR_RAM_SIZE]);
    if (out->sram_size == 0 && mapper_has_builtin_ram(out->mapper_code))
        out->sram_size = 512;

    /* The header checksum, which is the one a real Game Boy's boot ROM checks
       and refuses to start on. The global checksum at 0x014E is not checked by
       any hardware and is routinely wrong in good dumps, so it is ignored. */
    out->header_checksum = rom[HDR_CHECKSUM];
    for (uint16_t a = 0x0134; a <= 0x014C; a++)
        sum = (uint8_t)(sum - rom[a] - 1);
    out->header_ok = sum == out->header_checksum;

    return TG_OK;
}

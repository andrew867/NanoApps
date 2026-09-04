/*
 * tg_palette.c — see tg_palette.h.
 */

#include "tg_palette.h"

#include <string.h>

/*
 * Lightest to darkest, every one of them.
 *
 * DMG first, because it is what the hardware looked like and what the core
 * publishes by default - the others are a preference, not a correction.
 *
 * Pocket and Light are the two other panels Nintendo actually shipped: the
 * Pocket's screen was neutral grey rather than green, and the Light's was
 * backlit in a blue-white. Paperwhite and Ink are not historical, they are
 * for reading a text-heavy game on a panel that is far brighter and far
 * sharper than any of the originals.
 */
static const tg_palette k_palettes[] = {
    { "DMG", { 0x9BBC0Fu, 0x8BAC0Fu, 0x306230u, 0x0F380Fu } },
    { "Pocket", { 0xC4CFA1u, 0x8B956Du, 0x4D533Cu, 0x1F1F1Fu } },
    { "Light", { 0x00B581u, 0x009A71u, 0x00694Au, 0x004534u } },
    { "Paperwhite", { 0xF5F5EFu, 0xB8B8AEu, 0x6E6E68u, 0x22222Au } },
    { "Ink", { 0xFFFFFFu, 0xAAAAAAu, 0x555555u, 0x000000u } },
};

#define N (unsigned)(sizeof k_palettes / sizeof k_palettes[0])

unsigned tg_palette_count(void)
{
    return N;
}

const tg_palette *tg_palette_at(unsigned i)
{
    return &k_palettes[i < N ? i : 0];
}

unsigned tg_palette_index(const char *name)
{
    unsigned i;

    if (!name || !*name)
        return 0;

    for (i = 0; i < N; i++)
        if (strcmp(k_palettes[i].name, name) == 0)
            return i;

    return 0;
}

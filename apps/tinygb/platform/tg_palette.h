/*
 * tg_palette.h — which four shades the picture is drawn in.
 *
 * A DMG core reports a shade in 0..3, not a colour, so what those four shades
 * look like is entirely the front end's decision. The core publishes a default
 * - Peanut-GB's is the green of a real DMG panel - and this offers the rest.
 *
 * Four entries, lightest first, as 0x00RRGGBB. That is the order tg_scale.c
 * expects and the order the shade index means: 0 is paper, 3 is ink.
 */

#ifndef TINYGB_PALETTE_H
#define TINYGB_PALETTE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint32_t    shade[4];
} tg_palette;

/* How many there are, and one of them. Index 0 is the default. */
unsigned          tg_palette_count(void);
const tg_palette *tg_palette_at(unsigned i);

/* By name, for a setting read back from a file. Returns 0 - the default -
   when the name is not one of these, rather than failing: a palette is not
   worth refusing to start over. */
unsigned tg_palette_index(const char *name);

#endif /* TINYGB_PALETTE_H */

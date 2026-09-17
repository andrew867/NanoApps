/*
 * tg_roms.h — the shelf the cartridges sit on.
 *
 * On the device TinyGB is an n31os app, which means it is a folder rather
 * than a binary: /mnt/disk/n31os/apps/tinygb/ holds the executable, and the
 * cartridges go in roms/ beside it. So the library is wherever this binary
 * happens to be running from, plus /roms - not a compiled-in path, because
 * the same build also runs from /tmp while it is being tested, and from the
 * initramfs when no volume is mounted.
 *
 * Found from /proc/self/exe rather than argv[0]: argv[0] is whatever the
 * caller felt like passing, and n31-autostart invokes apps by bare name.
 * TINYGB_ROMS overrides the lot.
 *
 * The scan is what the menu will read when there is a menu. Until then it is
 * what lets `tinygb` with no arguments do something useful, and what lets a
 * cartridge be named without spelling out the path to it - which matters on a
 * device whose ROM file names are sixty characters of parenthesised region
 * codes.
 */

#ifndef TINYGB_ROMS_H
#define TINYGB_ROMS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char   dir[512];    /* the library directory, no trailing slash */
    char **name;        /* file names within it, sorted, case-insensitive */
    unsigned n;
} tg_rom_list;

/*
 * Where the cartridges live: $TINYGB_ROMS, or roms/ beside this binary.
 *
 * Always writes a path, even when nothing is there - a caller that wants to
 * report "no cartridges in X" needs the X.
 */
void tg_roms_dir(char *out, size_t cap);

/*
 * List the cartridges, sorted. Anything ending .gb or .gbc, in any case.
 *
 * Returns false when the directory cannot be read at all. An empty directory
 * is a success with n == 0: no cartridges is an ordinary state, and a fresh
 * install is exactly that.
 */
bool tg_roms_scan(tg_rom_list *l);

void tg_roms_free(tg_rom_list *l);

/*
 * Turn what someone typed into a path that can be opened.
 *
 * A readable file is used as it is. Otherwise it is looked for in the library:
 * first an exact file name, then any cartridge whose name contains what was
 * typed, ignoring case - so "zelda" finds "Legend of Zelda, The - Link's
 * Awakening (USA, Europe) (Rev 2).gb", which no leading match ever would.
 * Something matching two cartridges is refused rather than guessed at.
 *
 * Returns false if nothing matched, or if it matched more than one -
 * *ambiguous is set to say which of the two happened, so the caller can print
 * the right thing.
 */
bool tg_roms_resolve(const char *arg, char *out, size_t cap, bool *ambiguous);

#endif /* TINYGB_ROMS_H */

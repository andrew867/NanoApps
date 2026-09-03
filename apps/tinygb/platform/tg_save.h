/*
 * tg_save.h — the battery in the cartridge.
 *
 * A Game Boy cartridge with a save keeps it in RAM backed by a coin cell, and
 * the game writes to it whenever it likes - there is no "save" operation to
 * hook. So the emulator's job is to hand the core a buffer at startup, and get
 * that buffer onto disk before the machine goes away.
 *
 * Which is the awkward part on a handheld: the machine goes away by having its
 * power button held, and nothing gets to run at that point. Writing only on a
 * clean exit would lose an afternoon of Pokemon to a flat battery, so this also
 * writes when the contents have changed and a little time has passed.
 */

#ifndef TINYGB_SAVE_H
#define TINYGB_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     path[512];
    uint8_t *ram;
    size_t   len;

    uint32_t on_disk;      /* checksum of what was last written */
    long long next_check;  /* monotonic ns; when to look again */
} tg_save;

/*
 * Where a cartridge's save lives: beside the ROM, same name, .sav instead.
 *
 * The convention every Game Boy emulator uses, which means a save can be moved
 * between them - worth more than any cleverness about save directories.
 */
void tg_save_path_for(const char *rom_path, char *out, size_t cap);

/*
 * Attach `ram` to `path` and read any existing save into it.
 *
 * A file that is not the size the cartridge expects is refused rather than
 * padded: it is either a save from a different game or a different emulator's
 * format, and quietly loading half of it corrupts a real save. Returns false
 * when nothing was loaded, which includes the ordinary case of a new game.
 */
bool tg_save_load(tg_save *s, const char *path, uint8_t *ram, size_t len);

/*
 * Write it out if it has changed. Cheap enough to call every frame: it only
 * looks at the RAM every few seconds, and only writes when the contents
 * actually differ from what is on disk.
 */
void tg_save_tick(tg_save *s, long long now_ns);

/* Write it out now, changed or not - for the way out. */
bool tg_save_flush(tg_save *s);

#endif /* TINYGB_SAVE_H */

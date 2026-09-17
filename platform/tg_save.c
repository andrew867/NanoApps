/*
 * tg_save.c — see tg_save.h.
 *
 * Written against tg_sys.h rather than stdio, so the same file backs both
 * the Linux front end and the RetailOS one. The only thing that differs
 * between them is how often the RAM is looked at, and that is a build
 * setting: RetailOS has no exit callback, so a change there has to reach the
 * disk within a couple of seconds or a press of Home loses it.
 */

#include "tg_save.h"
#include "tg_sys.h"
#include "tg_util.h"

#include <string.h>

#ifndef TG_SAVE_CHECK_MS
# define TG_SAVE_CHECK_MS 5000
#endif
#define CHECK_EVERY_NS ((long long)TG_SAVE_CHECK_MS * 1000000LL)

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

void tg_save_path_for(const char *rom_path, char *out, size_t cap)
{
    size_t n;
    const char *dot;

    if (!rom_path || !out || !cap) return;

    n = strlen(rom_path);
    /*
     * Replace the extension, but only if the last dot is in the FILENAME. A
     * directory like /mnt/disk/n31os.old/tetris would otherwise have its path
     * truncated at the dot, and the save would be written somewhere else
     * entirely - or not at all.
     */
    dot = tg_strrchr(rom_path, '.');
    {
        const char *slash = tg_strrchr(rom_path, '/');

        if (dot && (!slash || dot > slash)) n = (size_t)(dot - rom_path);
    }

    if (n + 5 >= cap) n = cap > 5 ? cap - 5 : 0;
    memcpy(out, rom_path, n);
    memcpy(out + n, ".sav", 5);
}

bool tg_save_load(tg_save *s, const char *path, uint8_t *ram, size_t len)
{
    long size;
    bool ok = false;

    memset(s, 0, sizeof *s);
    s->ram = ram;
    s->len = len;
    tg_strlcpy(s->path, path ? path : "", sizeof s->path);

    if (!ram || !len || !s->path[0]) return false;

    size = tg_file_size(s->path);
    if (size >= 0) {
        if ((size_t)size == len) {
            ok = tg_file_read(s->path, ram, len) == (long)len;
        } else {
            /* Wrong size is a save for something else. Say so and start
               fresh rather than load part of it over a real one. */
            char line[160], num[24];

            tg_strlcpy(line, s->path, sizeof line);
            tg_strlcat(line, " is ", sizeof line);
            tg_strlcat(line, tg_utoa((unsigned long)size, num, sizeof num), sizeof line);
            tg_strlcat(line, " bytes, this cartridge wants ", sizeof line);
            tg_strlcat(line, tg_utoa((unsigned long)len, num, sizeof num), sizeof line);
            tg_strlcat(line, " - ignoring it", sizeof line);
            tg_log(line);
        }
    }

    s->on_disk = crc32_of(ram, len);
    return ok;
}

bool tg_save_flush(tg_save *s)
{
    uint32_t now;

    if (!s || !s->ram || !s->len || !s->path[0]) return false;

    now = crc32_of(s->ram, s->len);

    if (!tg_file_write(s->path, s->ram, s->len)) {
        char line[160];

        tg_strlcpy(line, "cannot write ", sizeof line);
        tg_strlcat(line, s->path, sizeof line);
        tg_log(line);
        return false;
    }

    s->on_disk = now;
    return true;
}

void tg_save_tick(tg_save *s, long long now_ns)
{
    if (!s || !s->ram || !s->len) return;

    if (now_ns < s->next_check) return;
    s->next_check = now_ns + CHECK_EVERY_NS;

    /*
     * Checksum first, write only if it differs. A cartridge writes its save
     * RAM constantly - Pokemon keeps the party there - so a timer alone would
     * rewrite 32 KiB to flash every few seconds forever, and this volume is
     * NAND behind an FTL that nobody wants to wear out for nothing.
     */
    if (crc32_of(s->ram, s->len) != s->on_disk) tg_save_flush(s);
}

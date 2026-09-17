/*
 * tg_save.c — see tg_save.h.
 */

#include "tg_save.h"

#include <stdio.h>
#include <string.h>

#define CHECK_EVERY_NS 5000000000LL   /* five seconds */

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
    dot = strrchr(rom_path, '.');
    {
        const char *slash = strrchr(rom_path, '/');

        if (dot && (!slash || dot > slash)) n = (size_t)(dot - rom_path);
    }

    if (n + 5 >= cap) n = cap > 5 ? cap - 5 : 0;
    memcpy(out, rom_path, n);
    memcpy(out + n, ".sav", 5);
}

bool tg_save_load(tg_save *s, const char *path, uint8_t *ram, size_t len)
{
    FILE *f;
    long size;
    bool ok = false;

    memset(s, 0, sizeof *s);
    s->ram = ram;
    s->len = len;
    snprintf(s->path, sizeof s->path, "%s", path ? path : "");

    if (!ram || !len || !s->path[0]) return false;

    if ((f = fopen(s->path, "rb"))) {
        if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0) {
            rewind(f);
            if ((size_t)size == len && fread(ram, 1, len, f) == len) {
                ok = true;
            } else if ((size_t)size != len) {
                /* Wrong size is a save for something else. Say so and start
                   fresh rather than load part of it over a real one. */
                fprintf(stderr,
                        "tinygb: %s is %ld bytes, this cartridge wants %zu"
                        " - ignoring it\n", s->path, size, len);
            }
        }
        fclose(f);
    }

    s->on_disk = crc32_of(ram, len);
    return ok;
}

bool tg_save_flush(tg_save *s)
{
    FILE *f;
    uint32_t now;

    if (!s || !s->ram || !s->len || !s->path[0]) return false;

    now = crc32_of(s->ram, s->len);

    if (!(f = fopen(s->path, "wb"))) {
        fprintf(stderr, "tinygb: cannot write %s\n", s->path);
        return false;
    }
    if (fwrite(s->ram, 1, s->len, f) != s->len) {
        fprintf(stderr, "tinygb: short write to %s\n", s->path);
        fclose(f);
        return false;
    }
    /* Flushed before the handle goes away, so a power cut a moment later
       loses nothing that this call claimed to have written. */
    fflush(f);
    fclose(f);

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

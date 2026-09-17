/*
 * tg_roms_hb.c — tg_roms.h on RetailOS: the cartridges under /Apps/Data.
 *
 * The same interface as the Linux one, backed by a fixed table instead of a
 * heap: there is no malloc here, and sixty-four names of ninety-six
 * characters is more cartridges than fit on the screen of anyone's mind.
 * Beyond that the rest are counted, not lost - the library says "and 12
 * more" so the owner knows to thin the folder.
 */

#include "tg_roms.h"
#include "tg_util.h"

#include "hb_sdk.h"

#include <string.h>

#define ROMS_DIR "/Apps/Data/TinyGB/roms"
#define MAX_N    64
#define NAME_CAP 96

static char  s_pool[MAX_N][NAME_CAP];
static char *s_ptrs[MAX_N];

void tg_roms_dir(char *out, size_t cap)
{
    tg_strlcpy(out, ROMS_DIR, cap);
}

static bool is_rom(const char *name)
{
    /* Leading dot: a Mac resource fork or an editor's leavings. */
    if (name[0] == '.') return false;
    return tg_ends_with_nocase(name, ".gb") || tg_ends_with_nocase(name, ".gbc");
}

bool tg_roms_scan(tg_rom_list *l)
{
    hb_dir_t d;
    char name[128];
    bool is_dir;

    memset(l, 0, sizeof *l);
    tg_strlcpy(l->dir, ROMS_DIR, sizeof l->dir);
    l->name = s_ptrs;

    /* Make the folder exist, so the owner has somewhere to put things and
       the empty-library screen can name a path that is really there. */
    hb_fs_mkdir(ROMS_DIR);

    if (!hb_fs_dir_open(&d, ROMS_DIR, false)) return false;

    while (hb_fs_dir_next(&d, name, sizeof name, &is_dir)) {
        unsigned i;

        if (is_dir || !is_rom(name)) continue;
        if (l->n >= MAX_N) { l->skipped++; continue; }

        /* Insertion sort, case-insensitive, as it arrives. n is small. */
        for (i = l->n; i > 0 && tg_stricmp(s_pool[i - 1], name) > 0; i--)
            memcpy(s_pool[i], s_pool[i - 1], NAME_CAP);
        tg_strlcpy(s_pool[i], name, NAME_CAP);
        l->n++;
    }
    hb_fs_dir_close(&d);

    for (unsigned i = 0; i < l->n; i++) s_ptrs[i] = s_pool[i];
    return true;
}

void tg_roms_free(tg_rom_list *l)
{
    l->n = 0;
}

bool tg_roms_resolve(const char *arg, char *out, size_t cap, bool *ambiguous)
{
    tg_rom_list l;
    int hit = -1;

    if (ambiguous) *ambiguous = false;
    if (!arg || !*arg) return false;

    /* A path that exists is used as it is. */
    if (arg[0] == '/' && hb_fs_exists(arg)) {
        tg_strlcpy(out, arg, cap);
        return true;
    }

    if (!tg_roms_scan(&l)) return false;

    /* An exact file name wins outright. */
    for (unsigned i = 0; i < l.n; i++) {
        if (tg_stricmp(l.name[i], arg) == 0) { hit = (int)i; break; }
    }

    /* Otherwise a unique case-insensitive substring. */
    if (hit < 0) {
        for (unsigned i = 0; i < l.n; i++) {
            if (!tg_strcasestr(l.name[i], arg)) continue;
            if (hit >= 0) {
                if (ambiguous) *ambiguous = true;
                return false;
            }
            hit = (int)i;
        }
    }

    if (hit < 0) return false;
    tg_strlcpy(out, l.dir, cap);
    tg_strlcat(out, "/", cap);
    tg_strlcat(out, l.name[hit], cap);
    return true;
}

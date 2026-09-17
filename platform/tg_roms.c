/*
 * tg_roms.c — see tg_roms.h.
 */

#include "tg_roms.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- where ---------------------------------------------------------------- */

static bool self_dir(char *out, size_t cap)
{
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    char *slash;

    if (n <= 0)
        return false;
    out[n] = '\0';

    slash = strrchr(out, '/');
    if (!slash)
        return false;

    /* Keep the slash off, so callers can always append "/whatever". A binary
       sitting in the root directory would leave an empty string here, which
       is the one case where the slash has to stay. */
    if (slash == out)
        out[1] = '\0';
    else
        *slash = '\0';

    return true;
}

void tg_roms_dir(char *out, size_t cap)
{
    const char *env = getenv("TINYGB_ROMS");
    char base[512];

    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return;
    }

    if (self_dir(base, sizeof base))
        snprintf(out, cap, "%s/roms", base);
    else
        snprintf(out, cap, "roms");
}

/* ---- what is on it -------------------------------------------------------- */

static bool is_rom(const char *name)
{
    const char *dot = strrchr(name, '.');

    /* A leading dot is an extension only to the shell; ".gb" is a hidden file
       with no name, and FAT volumes that have been near a Mac are full of
       ._ resource forks that would otherwise all look like cartridges. */
    if (!dot || dot == name || name[0] == '.')
        return false;

    return strcasecmp(dot, ".gb") == 0 || strcasecmp(dot, ".gbc") == 0;
}

/*
 * needle anywhere in haystack, ignoring case.
 *
 * strcasestr is a GNU extension and this builds against musl with the plain
 * standard on; twenty lines is cheaper than a feature-test macro that changes
 * what every other header in the file declares.
 */
static bool contains_nocase(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *p;

    if (nlen == 0)
        return true;

    for (p = haystack; *p; p++)
        if (strncasecmp(p, needle, nlen) == 0)
            return true;

    return false;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

bool tg_roms_scan(tg_rom_list *l)
{
    struct dirent *e;
    unsigned cap = 0;
    DIR *d;

    memset(l, 0, sizeof *l);
    tg_roms_dir(l->dir, sizeof l->dir);

    if (!(d = opendir(l->dir)))
        return false;

    while ((e = readdir(d))) {
        char *copy;

        if (!is_rom(e->d_name))
            continue;

        if (l->n == cap) {
            unsigned want = cap ? cap * 2 : 16;
            char **bigger = realloc(l->name, want * sizeof *bigger);

            if (!bigger)
                break;
            l->name = bigger;
            cap = want;
        }

        if (!(copy = strdup(e->d_name)))
            break;
        l->name[l->n++] = copy;
    }

    closedir(d);

    if (l->n > 1)
        qsort(l->name, l->n, sizeof *l->name, by_name);

    return true;
}

void tg_roms_free(tg_rom_list *l)
{
    unsigned i;

    for (i = 0; i < l->n; i++)
        free(l->name[i]);
    free(l->name);
    l->name = NULL;
    l->n = 0;
}

/* ---- picking one ---------------------------------------------------------- */

static bool readable(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool tg_roms_resolve(const char *arg, char *out, size_t cap, bool *ambiguous)
{
    tg_rom_list l;
    unsigned i, hits = 0, hit = 0;
    bool found = false;

    *ambiguous = false;

    if (readable(arg)) {
        snprintf(out, cap, "%s", arg);
        return true;
    }

    if (!tg_roms_scan(&l))
        return false;

    /*
     * An exact file name wins outright, and stops the search: it is the one
     * answer that cannot be ambiguous, and a menu passing a name from the
     * list must never be told its own entry matched two things.
     */
    for (i = 0; i < l.n; i++) {
        if (strcasecmp(l.name[i], arg) == 0) {
            hit = i;
            hits = 1;
            break;
        }
        if (contains_nocase(l.name[i], arg)) {
            if (hits++ == 0)
                hit = i;
        }
    }

    if (hits == 1) {
        snprintf(out, cap, "%s/%s", l.dir, l.name[hit]);
        found = true;
    } else if (hits > 1) {
        *ambiguous = true;
    }

    tg_roms_free(&l);
    return found;
}

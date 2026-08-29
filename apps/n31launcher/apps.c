/*
 * apps.c — building the app list. See apps.h.
 */

#include "apps.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

n31_app_t n31_apps[N31_MAX_APPS];
uint8_t   n31_app_count;
uint8_t   n31_extra_first;

/* ---- the floor ------------------------------------------------------------ */

/*
 * The factory image. These are listed first and in this order rather than
 * alphabetically: they are the two that are always there, so they are always in
 * the same place in the list, and muscle memory survives apps coming and going
 * on the volume behind them.
 *
 * This is a fallback for their labels, not their definition. When the volume is
 * mounted these two have manifests like everything else and those win, so there
 * is one place to change what an app is called.
 */
static const struct {
    const char *name, *tagline, *prog, *glyph;
    uint32_t accent;
    bool console;
} k_builtin[] = {
    { "Radio+",  "FM, RDS, recording", "radioplus", "FM", 0x22D3EE, false },
    { "TinyPod", "Music",              "tinypod",   "TP", 0x34D399, true  },
};

/* The home screen hands each of these a button, so the two must agree. */
_Static_assert(sizeof k_builtin / sizeof k_builtin[0] == N31_BUILTIN_COUNT,
               "N31_BUILTIN_COUNT does not match the builtin table");

/*
 * A colour for an app whose manifest does not name one. Picked from the app's
 * own name rather than its position, because a list whose colours shuffle when
 * something is installed above them is worse than no colour at all.
 */
static const uint32_t k_palette[] = {
    0xF43F5E, 0xA78BFA, 0xFBBF24, 0x38BDF8,
    0xFB923C, 0x4ADE80, 0xF472B6, 0x2DD4BF,
};

static uint32_t colour_for(const char *s)
{
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return k_palette[h % (sizeof k_palette / sizeof k_palette[0])];
}

/* ---- small helpers -------------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!cap) return;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

/*
 * A runnable file - and the regular-file test is the point of it. X_OK on a
 * directory means "searchable", which every directory is, so access() alone
 * says yes to the app folder itself. The flat-copy fallback below looks at
 * <apps>/<name>, which IS that folder, so without this every folder that
 * happened to contain no binary was listed as an app that then did nothing.
 */
static bool executable(const char *path)
{
    struct stat st;

    if (!path || !*path) return false;
    if (stat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;

    return access(path, X_OK) == 0;
}

/*
 * Two initials from a name, for an app whose manifest gives no glyph. Word
 * starts rather than the first two letters, so "Free Cell" reads FC and not FR.
 */
static void initials(const char *name, char *out, size_t cap)
{
    size_t n = 0;
    bool at_start = true;

    for (const char *p = name; *p && n + 1 < cap && n < 2; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '-' || c == '_') { at_start = true; continue; }
        if (at_start) {
            out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
            at_start = false;
        }
    }
    out[n] = 0;
    if (!n) copy_str(out, cap, "?");
}

/* ---- app.json ------------------------------------------------------------- */

/*
 * Enough JSON to read a manifest we wrote ourselves: find "key", expect a colon
 * and a string. It is not a JSON parser and does not try to be - anything it
 * cannot make sense of leaves the caller with its fallback, so a broken
 * manifest costs an app its label rather than its row.
 */
static bool json_str(const char *js, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);

    const char *p = strstr(js, pat);
    if (!p) return false;
    p += strlen(pat);

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;    /* one level, which is all we emit */
        out[n++] = *p++;
    }
    out[n] = 0;

    /* An unterminated string is a truncated file, not a value. */
    return *p == '"';
}

static bool parse_hex_colour(const char *s, uint32_t *out)
{
    if (!s || *s != '#') return false;
    s++;

    uint32_t v = 0;
    int n = 0;
    for (; *s && n < 6; s++, n++) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    if (n != 6) return false;

    *out = v;
    return true;
}

static bool read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n > 0;
}

/*
 * Overlay an app folder's manifest onto an entry, and report the executable
 * name it asks for.
 *
 * This is our own file and our own keys. A NanoApps Info.plist describes an
 * .hbapp for RetailOS - a different program built a different way - so one
 * sitting in the same folder is ignored rather than half-read.
 */
static void apply_manifest(const char *apps_dir, const char *folder,
                           n31_app_t *a, char *exec, size_t exec_cap)
{
    copy_str(exec, exec_cap, folder);

    char path[224], js[4096], v[64];
    snprintf(path, sizeof path, "%s/%s/app.json", apps_dir, folder);
    if (!read_file(path, js, sizeof js)) return;

    if (json_str(js, "name", v, sizeof v) && *v)
        copy_str(a->name, sizeof a->name, v);
    if (json_str(js, "exec", v, sizeof v) && *v)
        copy_str(exec, exec_cap, v);
    if (json_str(js, "tagline", v, sizeof v))
        copy_str(a->tagline, sizeof a->tagline, v);
    if (json_str(js, "glyph", v, sizeof v) && *v)
        copy_str(a->glyph, sizeof a->glyph, v);

    uint32_t c;
    if (json_str(js, "color", v, sizeof v) && parse_hex_colour(v, &c))
        a->accent = c;

    /* Anything other than "console" draws its own pixels. Defaulting that way
       round matters: a framebuffer app that wrongly gets the console back has
       kernel messages drawn over it, which is ugly; a console app that wrongly
       does not shows nothing at all, which looks broken. Neither is good, but
       most of these apps are LVGL. */
    if (json_str(js, "screen", v, sizeof v))
        a->console = !strcmp(v, "console");
}

/* ---- the list ------------------------------------------------------------- */

static bool have_prog(const n31_app_list_t *l, const char *prog)
{
    for (uint8_t i = 0; i < l->count; i++)
        if (!strcmp(l->apps[i].prog, prog)) return true;
    return false;
}

/*
 * Resolve a program the way n31-autostart does, and in the same order, so the
 * launcher never lists something the thing that starts apps would fail to find.
 * <exec>-start wins because several apps ship a wrapper beside the binary that
 * checks the hardware and says what is missing.
 */
static bool resolve_in(const char *apps_dir, const char *folder,
                       const char *exec, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%s-start", apps_dir, folder, exec);
    if (executable(out)) return true;

    snprintf(out, cap, "%s/%s/%s", apps_dir, folder, exec);
    if (executable(out)) return true;

    /* A flat copy, if someone staged one. */
    snprintf(out, cap, "%s/%s", apps_dir, folder);
    if (executable(out)) return true;

    *out = 0;
    return false;
}

static bool resolve_builtin(const char *prog, char *out, size_t cap)
{
    static const char *forms[] = { "/bin/%s-start", "/bin/%s-boot", "/bin/%s" };

    for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        snprintf(out, cap, forms[i], prog);
        if (executable(out)) return true;
    }
    *out = 0;
    return false;
}

/*
 * Places that are not a mount.
 *
 * /tmp/n31os/apps is the development path: scp an app folder there on a running
 * device and it appears in the list within a couple of seconds, with no mount,
 * no remount and no restart. It is checked first so a copy staged there shadows
 * the volume's version of the same app, which is what you want when testing a
 * build against the installed one.
 */
static const char *k_fixed_roots[] = {
    "/tmp/n31os/apps",
    "/run/n31os/apps",
};

/* Every place n31os/apps might be, handed one at a time to `fn`.
   The volume's mount point is not settled, so this looks under everything in
   /mnt rather than naming one - and /mnt/disk, which is what n31-autostart
   uses today, is simply one of the answers that turns up. */
static void for_each_apps_dir(void (*fn)(const char *apps_dir, void *ctx),
                              void *ctx)
{
    for (unsigned i = 0; i < sizeof k_fixed_roots / sizeof k_fixed_roots[0];
         i++) {
        DIR *f = opendir(k_fixed_roots[i]);
        if (!f) continue;
        closedir(f);
        fn(k_fixed_roots[i], ctx);
    }

    DIR *mnt = opendir("/mnt");
    if (!mnt) return;

    struct dirent *e;
    while ((e = readdir(mnt))) {
        if (e->d_name[0] == '.') continue;

        char apps_dir[128];
        snprintf(apps_dir, sizeof apps_dir, "/mnt/%s/n31os/apps", e->d_name);

        DIR *d = opendir(apps_dir);
        if (!d) continue;
        closedir(d);

        fn(apps_dir, ctx);
    }
    closedir(mnt);
}

static void add_from_dir(const char *apps_dir, const char *folder,
                         n31_app_list_t *l)
{
    if (l->count >= N31_MAX_APPS) return;
    if (folder[0] == '.') return;
    if (have_prog(l, folder)) return;          /* a builtin already covers it */

    n31_app_t a;
    memset(&a, 0, sizeof a);
    copy_str(a.prog, sizeof a.prog, folder);
    copy_str(a.name, sizeof a.name, folder);

    /* Where it came from, until a manifest says something better. It answers
       the question a user actually has when a row appears or does not, which
       is whether the volume is mounted. */
    copy_str(a.tagline, sizeof a.tagline, "on disk");
    a.accent = colour_for(folder);

    char exec[32];
    apply_manifest(apps_dir, folder, &a, exec, sizeof exec);

    if (!a.glyph[0]) initials(a.name, a.glyph, sizeof a.glyph);
    if (!resolve_in(apps_dir, folder, exec, a.path, sizeof a.path)) return;

    l->apps[l->count++] = a;
}

/*
 * Sort the discovered apps by name, leaving the first `fixed` entries alone.
 * Insertion sort because the list is at most a couple of dozen entries and this
 * runs every couple of seconds - the point is to be obviously correct, not
 * fast. Case-insensitive, so "entrain" does not sort below "Zoo".
 */
static int name_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void sort_from(n31_app_list_t *l, uint8_t fixed)
{
    for (uint8_t i = fixed + 1; i < l->count; i++) {
        n31_app_t v = l->apps[i];
        uint8_t j = i;
        while (j > fixed && name_cmp(l->apps[j - 1].name, v.name) > 0) {
            l->apps[j] = l->apps[j - 1];
            j--;
        }
        l->apps[j] = v;
    }
}

static void scan_one_dir(const char *apps_dir, void *ctx)
{
    n31_app_list_t *l = ctx;
    DIR *d = opendir(apps_dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) && l->count < N31_MAX_APPS)
        add_from_dir(apps_dir, e->d_name, l);
    closedir(d);
}

/* A builtin staged on the volume: the disk copy is the newer one when both
   exist, and it is the one with a manifest beside it. */
struct builtin_hit { n31_app_t *a; const char *prog; bool found; };

static void find_builtin(const char *apps_dir, void *ctx)
{
    struct builtin_hit *h = ctx;
    if (h->found) return;

    char exec[32];
    apply_manifest(apps_dir, h->prog, h->a, exec, sizeof exec);
    h->found = resolve_in(apps_dir, h->prog, exec,
                          h->a->path, sizeof h->a->path);
}

/* ---- the cheap check ------------------------------------------------------ */

/*
 * A hash of what is on offer, without reading any of it: the folder names, and
 * the mtime and size of each folder and of its manifest. That catches an app
 * appearing, disappearing, or having its app.json rewritten, which is every
 * change that alters this list.
 *
 * It deliberately does not catch a binary replaced under the same name. That
 * does not change the list, and the launcher execs the binary fresh on every
 * launch, so the new one is picked up regardless.
 */
static void fingerprint_dir(const char *apps_dir, void *ctx)
{
    uint32_t *h = ctx;
    DIR *d = opendir(apps_dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        for (const char *c = e->d_name; *c; c++) {
            *h ^= (unsigned char)*c;
            *h *= 16777619u;
        }

        char p[224];
        struct stat st;

        snprintf(p, sizeof p, "%s/%s", apps_dir, e->d_name);
        if (stat(p, &st) == 0) {
            *h ^= (uint32_t)st.st_mtime; *h *= 16777619u;
            *h ^= (uint32_t)st.st_size;  *h *= 16777619u;
        }

        snprintf(p, sizeof p, "%s/%s/app.json", apps_dir, e->d_name);
        if (stat(p, &st) == 0) {
            *h ^= (uint32_t)st.st_mtime; *h *= 16777619u;
            *h ^= (uint32_t)st.st_size;  *h *= 16777619u;
        }
    }
    closedir(d);
}

static n31_scan_state_t s_scan_state;

void n31_apps_invalidate(void)
{
    s_scan_state.valid = false;
}

/*
 * The whole scan, into a list the caller owns.
 *
 * No globals, no LVGL, no static scratch: everything it touches is either on
 * its own stack or reached through `out` and `state`. That is what makes it
 * safe to run on the scanner thread while the main loop keeps drawing.
 */
/*
 * The set of mounts, folded into the fingerprint.
 *
 * The folder walk below notices a volume appearing, because its folders appear
 * with it. This notices the mount itself, which is the actual event - a volume
 * unmounted, or swapped for another one whose folders happen to look identical.
 * One small read of a procfs file that is already in memory.
 */
static void fingerprint_mounts(uint32_t *h)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;

    int c;
    while ((c = fgetc(f)) != EOF) {
        *h ^= (unsigned char)c;
        *h *= 16777619u;
    }
    fclose(f);
}

bool n31_apps_scan_into(n31_app_list_t *out, n31_scan_state_t *state)
{
    uint32_t fp = 2166136261u;                      /* FNV-1a */
    fingerprint_mounts(&fp);
    for_each_apps_dir(fingerprint_dir, &fp);

    /* Nothing anywhere has changed since last time, so neither can the list.
       This is the case almost every poll, and it costs a readdir and a few
       stats instead of a manifest read per app. */
    if (state->valid && fp == state->hash)
        return false;

    state->hash = fp;
    state->valid = true;

    memset(out, 0, sizeof *out);

    for (unsigned i = 0; i < sizeof k_builtin / sizeof k_builtin[0]; i++) {
        n31_app_t a;
        memset(&a, 0, sizeof a);
        copy_str(a.name,    sizeof a.name,    k_builtin[i].name);
        copy_str(a.tagline, sizeof a.tagline, k_builtin[i].tagline);
        copy_str(a.prog,    sizeof a.prog,    k_builtin[i].prog);
        copy_str(a.glyph,   sizeof a.glyph,   k_builtin[i].glyph);
        a.accent  = k_builtin[i].accent;
        a.builtin = true;
        a.console = k_builtin[i].console;

        struct builtin_hit h = { &a, k_builtin[i].prog, false };
        for_each_apps_dir(find_builtin, &h);

        if (!h.found && !resolve_builtin(a.prog, a.path, sizeof a.path))
            continue;   /* not installed: a row that opens nothing is worse
                           than a missing row - it looks like our bug */

        out->apps[out->count++] = a;
    }

    out->extra_first = out->count;
    for_each_apps_dir(scan_one_dir, out);
    sort_from(out, out->extra_first);

    return true;
}

void n31_apps_publish(const n31_app_list_t *list)
{
    memcpy(n31_apps, list->apps, sizeof n31_apps);
    n31_app_count   = list->count;
    n31_extra_first = list->extra_first;
}

/*
 * The synchronous form, for the tests and for anything that has no thread to
 * spare. The launcher does not use it: it would do NAND-backed reads on the
 * loop that draws.
 */
bool n31_apps_scan(void)
{
    n31_app_list_t list;

    if (!n31_apps_scan_into(&list, &s_scan_state))
        return false;

    /* The fingerprint changed, but the list it produces may not have - an app
       touched without being altered, say. Compare before claiming a change. */
    bool same = (list.count == n31_app_count) &&
                memcmp(list.apps, n31_apps,
                       sizeof(n31_app_t) * list.count) == 0;

    n31_apps_publish(&list);
    return !same;
}

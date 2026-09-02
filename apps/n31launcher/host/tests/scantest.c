/*
 * Exercise the app scan against a real directory tree.
 *
 * The two things worth proving without a device: that an app dropped into the
 * development overlay is found, and that the fingerprint makes a repeat scan
 * free without making it blind.
 */
#include "apps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void ok(const char *what, int cond)
{
    printf("  %-46s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static int have(const char *prog)
{
    for (uint8_t i = 0; i < n31_app_count; i++)
        if (!strcmp(n31_apps[i].prog, prog)) return 1;
    return 0;
}

static const n31_app_t *find(const char *prog)
{
    for (uint8_t i = 0; i < n31_app_count; i++)
        if (!strcmp(n31_apps[i].prog, prog)) return &n31_apps[i];
    return 0;
}

int main(void)
{
    printf("app scan:\n");

    n31_apps_scan();
    ok("finds the app staged in the overlay", have("demo"));

    const n31_app_t *a = find("demo");
    if (a) {
        ok("reads the name from app.json", !strcmp(a->name, "Demo App"));
        ok("reads the tagline",            !strcmp(a->tagline, "a test"));
        ok("reads the glyph",              !strcmp(a->glyph, "DA"));
        ok("reads the colour",             a->accent == 0xF43F5Eu);
        ok("honours screen=console",       a->console);
    } else {
        fails++;
    }

    ok("an app with no manifest still lists", have("bare"));
    a = find("bare");
    if (a) {
        ok("falls back to the folder name",  !strcmp(a->name, "bare"));
        ok("derives initials for the glyph", !strcmp(a->glyph, "B"));
        ok("defaults to framebuffer",        !a->console);
    }

    ok("a folder with no executable is skipped", !have("empty"));

    /*
     * Which copy of a builtin runs.
     *
     * The volume is searched before the copy baked into the initramfs, and it
     * used to win outright - so a rebuilt, repacked and reflashed app would
     * boot with the days-old binary on the read-only volume shadowing it, and
     * nothing said a word. The rule now is whichever was built last, and this
     * is the part of it that can be checked without a device: the fixture
     * stages the same program in both places and touches one of them.
     */
    {
        const n31_app_t *r;

        /* Older on disk, newer in /bin: the /bin one. */
        r = find("radioplus");
        ok("a stale volume copy loses to the baked-in one",
           r && !strncmp(r->path, getenv("N31_BUILTIN_DIR"),
                         strlen(getenv("N31_BUILTIN_DIR"))));

        /* And the other way round, because the volume copy is the one with an
           app's data beside it and it has to keep winning when it is current. */
        r = find("tinypod");
        ok("a current volume copy still wins",
           r && !strncmp(r->path, "/tmp/n31os/apps/", 16));
    }

    /* Nothing changed, so the second scan must be a no-op. That is what makes
       polling every two seconds affordable on a NAND-backed volume. */
    ok("an unchanged rescan reports no change", n31_apps_scan() == false);

    /* ...but it must not be blind: invalidate and it works again. */
    n31_apps_invalidate();
    n31_apps_scan();
    ok("still finds everything after invalidate", have("demo") && have("bare"));

    /*
     * A volume going away. The launcher cannot unmount anything from here, but
     * what it observes is identical: the folders stop existing. Its apps have
     * to leave the list with them, or it offers to run something that is not
     * there any more.
     */
    if (system("rm -rf /tmp/n31os/apps/demo") != 0)
        printf("  (could not remove the folder)\n");

    ok("a removed app is dropped", n31_apps_scan() && !have("demo"));
    ok("and the others stay",      have("bare"));

    /* And the whole root disappearing - the actual unmount case. */
    if (system("rm -rf /tmp/n31os") != 0)
        printf("  (could not remove the root)\n");

    n31_apps_scan();
    ok("everything on the volume goes", !have("bare") && n31_extra_count == 0);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}

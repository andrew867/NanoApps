/*
 * apps.h — what the launcher can start, found at runtime.
 *
 * Two kinds of app, and the difference is where they live rather than what they
 * are. Radio+ and TinyPod ship in the factory image, so they are compiled in as
 * a floor: the launcher has something to show even with no storage at all.
 * Everything else lives in n31os/apps on the internal volume and is discovered,
 * because that set changes without this binary changing.
 *
 * Discovery reads an app.json beside each binary - the same JSON everything else
 * here reads and writes - carrying the name, the executable, and the colour that
 * tells one row from another. That is also what makes the planned touch
 * springboard a change of layout rather than a new format to fill in.
 *
 * An app is listed only if something executable was actually found for it. A row
 * that cannot start anything is worse than a missing row: it looks like a bug in
 * the launcher rather than an app that is not installed.
 *
 * Nothing here starts anything. That belongs to n31-autostart, which resolves a
 * name against the same locations and does the per-app setup - the wad path for
 * fbdoom, the writable home for radioplus, since the volume is mounted
 * read-only. Duplicating that here would mean two places to fix.
 */

#ifndef N31_LAUNCHER_APPS_H
#define N31_LAUNCHER_APPS_H

#include <stdbool.h>
#include <stdint.h>

/* From the button drivers: gpio-s5l8740.c reports the volume keys, and
   gpio-d1830.c the PMIC ones. Taken from the source rather than guessed,
   because a launcher whose buttons do nothing is indistinguishable from a
   launcher that crashed. */
#define N31_KEY_VOLUMEDOWN 114
#define N31_KEY_VOLUMEUP   115
#define N31_KEY_PLAYPAUSE  164
#define N31_KEY_HOMEPAGE   172

#define N31_MAX_APPS 24

/* Radio+ and TinyPod. The home screen gives them a fixed button each, and the
   extras list is everything after them - so this is also where one list ends
   and the other begins. */
#define N31_BUILTIN_COUNT 2

typedef struct {
    char     name[32];      /* CFBundleName, else the folder name */
    char     tagline[32];   /* what it is, or where it came from */
    char     prog[32];      /* the name n31-autostart resolves */
    char     path[192];     /* the executable found, or "" for a /bin builtin */
    char     glyph[4];      /* one or two characters for the icon block */
    uint32_t accent;
    bool     builtin;

    /*
     * How it draws. A console app needs the framebuffer console put back and
     * the screen cleared before it starts, or its output goes to a terminal
     * that is not being displayed; a framebuffer app needs the console kept
     * off, or kernel messages land on top of it. Nothing about the binary says
     * which, so the manifest does.
     */
    bool     console;

    /*
     * Whether the app handles its own way out.
     *
     * HOME used to be the launcher's: it watched for it while a child was
     * running and terminated whatever that was. Which meant no app could ever
     * use its fourth button, and that quitting was a SIGTERM arriving without
     * warning - no chance to write a config, finish a save, or stop a sink.
     *
     * So the buttons stay with the app. An app that says "keys": "own" gets
     * all of them and is expected to exit by itself; anything else - fbDOOM,
     * anything third-party, anything that has never heard of this launcher -
     * is closed with a short press of the Sleep button, which is the one
     * button no app on this device does anything with.
     */
    bool     owns_keys;
} n31_app_t;

/*
 * A complete list, self-contained. The scan fills one of these rather than the
 * globals so it can run on a worker thread: reading app manifests off the
 * internal volume can block for a long time, and the loop that draws must not
 * be the one waiting for NAND.
 */
typedef struct {
    n31_app_t apps[N31_MAX_APPS];
    uint8_t   count;
    uint8_t   extra_first;
} n31_app_list_t;

/* What the scan remembers between runs so it can skip work. Caller-owned, so
   two scanners would not fight over it. */
typedef struct {
    uint32_t hash;
    bool     valid;
} n31_scan_state_t;

/*
 * Scan into `out`. Touches no globals and no LVGL, so it is safe to call from
 * a thread. Returns true if the result differs from what `state` last saw; on
 * false, `out` is left untouched and nothing needs redrawing.
 */
bool n31_apps_scan_into(n31_app_list_t *out, n31_scan_state_t *state);

/* Copy a finished list into the globals the UI reads. Main thread only. */
void n31_apps_publish(const n31_app_list_t *list);

extern n31_app_t n31_apps[N31_MAX_APPS];
extern uint8_t   n31_app_count;

/*
 * Where the builtins end and the discovered apps begin. The home screen already
 * gives each builtin a button, so the extras list starts here - and this is not
 * simply N31_BUILTIN_COUNT, because a builtin that is not installed is skipped.
 */
extern uint8_t   n31_extra_first;

/* How many apps the extras list has. */
#define n31_extra_count ((uint8_t)(n31_app_count - n31_extra_first))

/*
 * Build the list: the compiled-in apps that resolve, then whatever is in
 * n31os/apps. Safe to call repeatedly - the internal volume is not always
 * mounted when the launcher starts, so the list is rebuilt periodically and an
 * app that appears later simply appears.
 *
 * Cheap to call often. It fingerprints the app folders first - a readdir and a
 * stat apiece - and only re-reads the manifests when that changes, because on
 * the internal volume every manifest read is a NAND-backed sector and this runs
 * every couple of seconds for as long as the device is on.
 *
 * Returns true if the list changed.
 */
bool n31_apps_scan(void);

/*
 * Force the next scan to do the full work, ignoring the fingerprint. For the
 * moment a volume finishes mounting: the folders were not readable a second
 * ago, so their fingerprint is not evidence of anything.
 */
void n31_apps_invalidate(void);

#endif /* N31_LAUNCHER_APPS_H */

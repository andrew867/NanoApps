/*
 * tg_settings.h — the five things a player can change, in a text file.
 *
 * key=value, one per line, so it can be read by eye and edited with
 * anything. Unknown keys are ignored and an unknown palette name means the
 * default: a setting is never worth refusing to start over.
 */

#ifndef TINYGB_SETTINGS_H
#define TINYGB_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char palette[24];   /* a tg_palette name */
    bool smooth;
    bool tilt;
    bool overlay;       /* fps and queue counters over the pad */
    char last_rom[96];  /* file name within the library */
} tg_settings;

void   tg_settings_defaults(tg_settings *s, bool tilt_default);

/* Parse `text` (need not be NUL-terminated) over whatever `s` already
   holds. Returns how many recognised keys were seen. */
unsigned tg_settings_parse(tg_settings *s, const char *text, size_t len);

/* Write the file's contents. Returns the length it would have had; the
   buffer is always NUL-terminated when cap > 0. */
size_t tg_settings_format(const tg_settings *s, char *out, size_t cap);

/* Through tg_sys. load() leaves defaults in place for a missing file and
   returns false then. */
bool tg_settings_load(tg_settings *s, const char *path, bool tilt_default);
bool tg_settings_save(const tg_settings *s, const char *path);

#endif /* TINYGB_SETTINGS_H */

/*
 * tg_settings.c — see tg_settings.h.
 */

#include "tg_settings.h"
#include "tg_palette.h"
#include "tg_sys.h"
#include "tg_util.h"

#include <string.h>

void tg_settings_defaults(tg_settings *s, bool tilt_default)
{
    memset(s, 0, sizeof *s);
    tg_strlcpy(s->palette, tg_palette_at(0)->name, sizeof s->palette);
    s->smooth = true;
    s->tilt = tilt_default;
    s->overlay = false;
}

static bool truthy(const char *v)
{
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

unsigned tg_settings_parse(tg_settings *s, const char *text, size_t len)
{
    unsigned seen = 0;
    size_t i = 0;

    while (i < len) {
        char key[24], val[96];
        size_t k = 0, v = 0;

        /* key */
        while (i < len && text[i] != '=' && text[i] != '\n') {
            if (k < sizeof key - 1 && text[i] != ' ' && text[i] != '\r') key[k++] = text[i];
            i++;
        }
        key[k] = 0;
        if (i < len && text[i] == '=') {
            i++;
            while (i < len && text[i] != '\n') {
                if (v < sizeof val - 1 && text[i] != '\r') val[v++] = text[i];
                i++;
            }
        }
        val[v] = 0;
        if (i < len) i++;                     /* the newline */

        if (!k) continue;
        if (strcmp(key, "palette") == 0) {
            /* Validated through the table: a name that is not one of ours
               comes back as the default's. */
            tg_strlcpy(s->palette, tg_palette_at(tg_palette_index(val))->name,
                       sizeof s->palette);
            seen++;
        } else if (strcmp(key, "smooth") == 0)   { s->smooth = truthy(val);  seen++; }
        else   if (strcmp(key, "tilt") == 0)     { s->tilt = truthy(val);    seen++; }
        else   if (strcmp(key, "overlay") == 0)  { s->overlay = truthy(val); seen++; }
        else   if (strcmp(key, "last_rom") == 0) { tg_strlcpy(s->last_rom, val, sizeof s->last_rom); seen++; }
    }
    return seen;
}

size_t tg_settings_format(const tg_settings *s, char *out, size_t cap)
{
    size_t n;

    if (cap) out[0] = 0;
    n  = tg_strlcat(out, "palette=", cap);
    n  = tg_strlcat(out, s->palette, cap);
    n  = tg_strlcat(out, "\nsmooth=", cap);
    n  = tg_strlcat(out, s->smooth ? "1" : "0", cap);
    n  = tg_strlcat(out, "\ntilt=", cap);
    n  = tg_strlcat(out, s->tilt ? "1" : "0", cap);
    n  = tg_strlcat(out, "\noverlay=", cap);
    n  = tg_strlcat(out, s->overlay ? "1" : "0", cap);
    n  = tg_strlcat(out, "\nlast_rom=", cap);
    n  = tg_strlcat(out, s->last_rom, cap);
    n  = tg_strlcat(out, "\n", cap);
    return n;
}

bool tg_settings_load(tg_settings *s, const char *path, bool tilt_default)
{
    char buf[512];
    long got;

    tg_settings_defaults(s, tilt_default);
    got = tg_file_read(path, buf, sizeof buf);
    if (got <= 0) return false;
    tg_settings_parse(s, buf, (size_t)got);
    return true;
}

bool tg_settings_save(const tg_settings *s, const char *path)
{
    char buf[256];
    size_t n = tg_settings_format(s, buf, sizeof buf);

    if (n >= sizeof buf) return false;
    return tg_file_write(path, buf, n);
}

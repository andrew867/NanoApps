/*
 * store.h — what Radio+ writes down: presets, and the RDS sidecar for a
 * recording.
 *
 * JSON, because these are files a person may well want to read, edit or copy
 * between devices, and because a recording outlives the app that made it. A
 * sidecar full of packed binary would be useless in five years; a sidecar full
 * of JSON is still readable with nothing but a text editor.
 *
 * The writer here is deliberately small - object, array, key, value, escape -
 * rather than a general JSON library. It emits into a caller-supplied buffer
 * and never allocates, so the same code serves the Linux build writing through
 * stdio and the RetailOS one writing through hb_fs_stream_write.
 *
 * The reader is equally small and only understands the shapes this file writes.
 * That is a deliberate limit and not a hidden one: en_preset_load() is
 * documented to be forgiving about whitespace and ordering and nothing else.
 *
 * The sidecar records raw groups alongside decoded fields. Raw groups mean a
 * recording can be decoded again later, by a better decoder than the one that
 * made it - which matters here specifically, because the FIFO framing in
 * core/rds.c is not yet confirmed and every recording made before it is
 * confirmed would otherwise be undecodable afterwards.
 */

#ifndef RADIOPLUS_STORE_H
#define RADIOPLUS_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "rds.h"

/* ---- a very small JSON writer -------------------------------------------- */

typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
    uint8_t  depth;
    bool     need_comma;
    bool     overflow;   /* sticky: once set, nothing more is written */
} en_json_t;

void en_json_init(en_json_t *j, char *buf, uint32_t cap);
void en_json_obj_open(en_json_t *j, const char *key);
void en_json_obj_close(en_json_t *j);
void en_json_arr_open(en_json_t *j, const char *key);
void en_json_arr_close(en_json_t *j);
void en_json_str(en_json_t *j, const char *key, const char *value);
void en_json_int(en_json_t *j, const char *key, int32_t value);
void en_json_uint(en_json_t *j, const char *key, uint32_t value);
void en_json_bool(en_json_t *j, const char *key, bool value);
void en_json_hex16(en_json_t *j, const char *key, uint16_t value);

/* Length written, or 0 if the buffer overflowed at any point. Callers must
   check: a truncated preset file is worse than none, because it looks valid. */
uint32_t en_json_done(en_json_t *j);

/* ---- presets ------------------------------------------------------------- */

#define EN_PRESET_MAX      40
#define EN_PRESET_NAME_LEN 24

typedef struct {
    uint32_t khz;
    char     name[EN_PRESET_NAME_LEN + 1];   /* usually the RDS station name */
    uint16_t pi;                             /* 0 when unknown */
    uint8_t  pty;
    bool     rbds;                           /* which table pty came from */
} en_preset_t;

typedef struct {
    en_preset_t list[EN_PRESET_MAX];
    uint8_t     count;
    char        region[24];
} en_presets_t;

void    en_presets_init(en_presets_t *p, const char *region);
int     en_preset_find(const en_presets_t *p, uint32_t khz);
bool    en_preset_add(en_presets_t *p, const en_preset_t *entry);
bool    en_preset_remove(en_presets_t *p, uint32_t khz);
void    en_preset_sort(en_presets_t *p);

uint32_t en_presets_save(const en_presets_t *p, char *buf, uint32_t cap);
bool     en_presets_load(en_presets_t *p, const char *json, uint32_t len);

/* ---- the RDS sidecar ----------------------------------------------------- */

/*
 * Written alongside a recording as name.rds.json. Groups are appended as they
 * arrive with the offset into the recording, so a player can show what was on
 * the display at any point rather than only what was there at the end.
 *
 * Streaming rather than assembled: a recording can run for an hour, so the
 * sidecar is opened, appended to, and closed, and each append is a complete
 * line that stands on its own if the file is truncated.
 */
typedef struct {
    en_json_t j;
    uint32_t  groups;
    bool      open;
} en_sidecar_t;

/* Write the header - everything known about the station at the start. */
uint32_t en_sidecar_begin(en_sidecar_t *s, char *buf, uint32_t cap,
                          uint32_t khz, const char *region, bool rbds,
                          const en_rds_t *r);

/* One group, with its offset into the recording in milliseconds. Returns the
   bytes written into the buffer, which the caller then flushes and reuses. */
uint32_t en_sidecar_group(en_sidecar_t *s, char *buf, uint32_t cap,
                          uint32_t ms, const uint16_t blk[4], uint8_t valid);

/* Close the array and the object, and write the final decoded state. */
uint32_t en_sidecar_end(en_sidecar_t *s, char *buf, uint32_t cap,
                        uint32_t duration_ms, const en_rds_t *r);

#endif /* RADIOPLUS_STORE_H */

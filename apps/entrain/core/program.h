/*
 * program.h — bands, presets, programs, and the loop planner.
 *
 * The loop planner is the piece that makes the whole app possible on this
 * device. RetailOS refuses to load any sound file over 1 MiB (see
 * AUDIO_NOTES.md), so a steady preset has to fit its entire seamless loop into
 * that budget, and a loop is only seamless if it contains a whole number of
 * cycles of both carriers.
 *
 * The rule, and en_plan_loop implements exactly it:
 *
 *   - The beat is the point of the preset. Never move it to make the maths
 *     work.
 *   - The carrier is arbitrary within a wide range. 200.00 -> 200.05 Hz is
 *     inaudible. Move that instead.
 *   - Whatever comes out, store and display the REALISED beat, not the
 *     requested one.
 */

#ifndef ENTRAIN_PROGRAM_H
#define ENTRAIN_PROGRAM_H

#include <stdint.h>
#include "render.h"

/* ---- bands ------------------------------------------------------------- */

typedef enum {
    EN_BAND_DELTA = 0,   /* 0.5 - 4  Hz */
    EN_BAND_THETA,       /* 4   - 8  Hz */
    EN_BAND_ALPHA,       /* 8   - 13 Hz */
    EN_BAND_BETA,        /* 13  - 30 Hz */
    EN_BAND_GAMMA,       /* 30  - 100 Hz */
    EN_BAND_COUNT
} en_band_t;

en_band_t   en_band_of(double beat_hz);
const char *en_band_name(en_band_t band);
uint32_t    en_band_color(en_band_t band);   /* 0xRRGGBB */

/* ---- loop planning ------------------------------------------------------ */

/* The loader's hard ceiling. Anything at or over this is rejected outright. */
#define EN_MAX_LOAD_BYTES 1048576u
/* What we actually aim for. Every load allocates 1.2x the file plus 8 KB from
   the shared OS heap, through an allocator that panics rather than failing, so
   sitting right on the ceiling is not worth the risk. */
#define EN_TARGET_BYTES    786432u

typedef struct {
    uint32_t sample_rate;
    uint32_t frames;        /* N */
    uint32_t cycles_l;      /* whole cycles of f_l in the loop */
    uint32_t cycles_beat;   /* cycles_r - cycles_l */
    double   t_seconds;     /* frames / sample_rate */
    double   f_l;           /* realised left carrier */
    double   f_r;           /* realised right carrier */
    double   beat_hz;       /* realised beat = cycles_beat / t_seconds */
    double   beat_error;    /* realised - requested */
    uint32_t bytes;         /* the resulting file size */
} en_loop_plan_t;

/* Sample rates we are willing to ship, best first. Lower rates buy longer
   loops for the same bytes, and a 400 Hz carrier does not care: 11025 Hz still
   leaves a Nyquist of 5.5 kHz. Which of these the device actually accepts is
   what harness T1 measures. */
extern const uint32_t EN_RATES[];
extern const int      EN_RATES_COUNT;

/* Find the best cycle-exact loop for this beat within `max_bytes`.
   Returns 1 on success, 0 if nothing fits (a beat too slow for the budget).
   Tries every rate in `rates` and keeps the one with the smallest beat error,
   breaking ties toward the longest loop. */
int en_plan_loop(double beat_hz, double carrier_hz,
                 const uint32_t *rates, int n_rates,
                 uint32_t max_bytes, en_loop_plan_t *out);

/* Fill a segment that renders `plan` as a seamless loop. */
void en_plan_to_segment(const en_loop_plan_t *plan, en_mode_t mode,
                        en_noise_kind_t noise, double tone_level,
                        double noise_level, en_segment_t *out);

/* ---- presets (steady) --------------------------------------------------- */

typedef struct {
    const char     *name;
    const char     *detail;      /* what it does, technically. No claims. */
    double          beat_hz;     /* requested */
    double          carrier_hz;  /* requested */
    en_mode_t       mode;
    en_noise_kind_t noise;
    double          noise_level;
} en_preset_t;

const en_preset_t *en_presets(int *count);

/* ---- programs (timelines) ----------------------------------------------- */

typedef struct {
    double          beat_start;
    double          beat_end;
    double          carrier_hz;
    uint32_t        seconds;
    en_noise_kind_t noise;
    double          noise_level;
} en_prog_seg_t;

typedef struct {
    const char          *name;
    const char          *detail;
    en_mode_t            mode;
    const en_prog_seg_t *segs;
    int                  n_segs;
} en_program_t;

const en_program_t *en_programs(int *count);

uint32_t  en_program_seconds(const en_program_t *p);
/* Band of the program's longest segment — what the list chip should colour. */
en_band_t en_program_band(const en_program_t *p);
/* Beat frequency at `t` seconds in, for the Now Playing readout. */
double    en_program_beat_at(const en_program_t *p, double t_seconds);
/* Index of the segment covering `t`, or -1 past the end. */
int       en_program_seg_at(const en_program_t *p, double t_seconds);

/* ---- user programs loaded from disk ------------------------------------- */

#define EN_USER_MAX_SEGS 16
#define EN_NAME_MAX      32

typedef struct {
    char            name[EN_NAME_MAX];
    en_mode_t       mode;
    double          carrier_hz;
    en_noise_kind_t noise;
    double          noise_level;
    en_prog_seg_t   segs[EN_USER_MAX_SEGS];
    int             n_segs;
} en_user_program_t;

typedef enum {
    EN_PARSE_OK = 0,
    EN_PARSE_BAD_KEY = -1,
    EN_PARSE_BAD_VALUE = -2,
    EN_PARSE_TOO_MANY_SEGS = -3,
    EN_PARSE_NO_SEGS = -4
} en_parse_result_t;

/* Parse the user-program text format documented in README.md. Read-only over
   `text`; no allocation, no stdio. On failure returns a negative code and sets
   *err_line (1-based) to the offending line. */
int en_parse_program(const char *text, uint32_t len,
                     en_user_program_t *out, int *err_line);

const char *en_parse_error_text(int code);

#endif /* ENTRAIN_PROGRAM_H */

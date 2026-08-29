/*
 * program.c — see program.h.
 */

#include "program.h"
#include "wavout.h"

/* ---- bands -------------------------------------------------------------- */

en_band_t en_band_of(double beat_hz)
{
    if (beat_hz < 4.0)  return EN_BAND_DELTA;
    if (beat_hz < 8.0)  return EN_BAND_THETA;
    if (beat_hz < 13.0) return EN_BAND_ALPHA;
    if (beat_hz < 30.0) return EN_BAND_BETA;
    return EN_BAND_GAMMA;
}

const char *en_band_name(en_band_t band)
{
    switch (band) {
    case EN_BAND_DELTA: return "Delta";
    case EN_BAND_THETA: return "Theta";
    case EN_BAND_ALPHA: return "Alpha";
    case EN_BAND_BETA:  return "Beta";
    case EN_BAND_GAMMA: return "Gamma";
    default:            return "?";
    }
}

uint32_t en_band_color(en_band_t band)
{
    switch (band) {
    case EN_BAND_DELTA: return 0x4C5BD4u;   /* indigo  */
    case EN_BAND_THETA: return 0x8B5CF6u;   /* violet  */
    case EN_BAND_ALPHA: return 0x14B8A6u;   /* teal    */
    case EN_BAND_BETA:  return 0xF59E0Bu;   /* amber   */
    case EN_BAND_GAMMA: return 0xEC4899u;   /* magenta */
    default:            return 0x8A8F98u;
    }
}

/* ---- loop planning ------------------------------------------------------ */

const uint32_t EN_RATES[] = { 11025u, 22050u, 44100u };
const int      EN_RATES_COUNT = (int)(sizeof EN_RATES / sizeof EN_RATES[0]);

static double dabs(double x) { return x < 0.0 ? -x : x; }

static uint32_t round_u32(double x)
{
    if (x <= 0.0) return 0;
    return (uint32_t)(x + 0.5);
}

/* Best loop for one rate. See the header for the rule this implements. */
static int plan_for_rate(double beat_hz, double carrier_hz, uint32_t sr,
                         uint32_t max_bytes, en_loop_plan_t *out)
{
    if (beat_hz <= 0.0 || sr == 0 || max_bytes <= EN_WAV_HEADER_BYTES)
        return 0;

    const uint32_t max_frames = (max_bytes - EN_WAV_HEADER_BYTES) / 4u;
    if (max_frames < 2) return 0;

    int      found = 0;
    double   best_err = 0.0;
    uint32_t best_n = 0, best_dn = 0;

    /* cycles_beat = dn. For each dn the ideal loop length is dn/beat seconds;
       round it to a whole number of samples and see how far the realised beat
       lands from the request. dn grows the loop, so stop as soon as the ideal
       length overflows the budget. */
    for (uint32_t dn = 1; dn < 1000000u; dn++) {
        double n_ideal = (double)dn * (double)sr / beat_hz;
        if (n_ideal > (double)max_frames + 0.5) break;

        uint32_t n = round_u32(n_ideal);
        if (n < 2 || n > max_frames) continue;

        double realised = (double)dn * (double)sr / (double)n;
        double err = dabs(realised - beat_hz);

        /* Prefer accuracy; among equally accurate options take the longest
           loop, because a longer loop means fewer seams to hide. Treat
           differences under a micro-hertz as equal — they are far below what
           any listener or any test can resolve. */
        if (!found || err < best_err - 1e-9 ||
            (err < best_err + 1e-9 && n > best_n)) {
            found = 1;
            best_err = err;
            best_n = n;
            best_dn = dn;
        }
    }

    if (!found) return 0;

    double t = (double)best_n / (double)sr;
    uint32_t cyc_l = round_u32(carrier_hz * t);
    if (cyc_l < 1) cyc_l = 1;

    out->sample_rate = sr;
    out->frames      = best_n;
    out->cycles_l    = cyc_l;
    out->cycles_beat = best_dn;
    out->t_seconds   = t;
    out->f_l         = (double)cyc_l / t;
    out->f_r         = (double)(cyc_l + best_dn) / t;
    out->beat_hz     = (double)best_dn / t;
    out->beat_error  = out->beat_hz - beat_hz;
    out->bytes       = en_wav_size(best_n, 2);
    return 1;
}

int en_plan_loop(double beat_hz, double carrier_hz,
                 const uint32_t *rates, int n_rates,
                 uint32_t max_bytes, en_loop_plan_t *out)
{
    en_loop_plan_t best;
    int have = 0;

    for (int i = 0; i < n_rates; i++) {
        en_loop_plan_t p;
        if (!plan_for_rate(beat_hz, carrier_hz, rates[i], max_bytes, &p))
            continue;
        double e = dabs(p.beat_error);
        double be = have ? dabs(best.beat_error) : 0.0;
        if (!have || e < be - 1e-9 ||
            (e < be + 1e-9 && p.t_seconds > best.t_seconds)) {
            best = p;
            have = 1;
        }
    }

    if (!have) return 0;
    *out = best;
    return 1;
}

void en_plan_to_segment(const en_loop_plan_t *plan, en_mode_t mode,
                        en_noise_kind_t noise, double tone_level,
                        double noise_level, en_segment_t *out)
{
    out->sample_rate = plan->sample_rate;
    out->mode        = mode;
    out->noise       = noise;
    out->frames      = plan->frames;
    out->fade_in_s   = 0.0;   /* a loop never fades; the player does that */
    out->fade_out_s  = 0.0;

    out->start.carrier_hz  = plan->f_l;
    out->start.beat_hz     = plan->beat_hz;
    out->start.tone_level  = tone_level;
    out->start.noise_level = noise_level;
    out->end = out->start;    /* steady by definition */
}

/* ---- presets ------------------------------------------------------------ */

static const en_preset_t s_presets[] = {
    { "Delta 2 Hz",   "Binaural • 100 Hz carrier",
      2.0,  100.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Theta 6 Hz",   "Binaural • 150 Hz carrier",
      6.0,  150.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Schumann",     "Binaural • 200 Hz carrier",
      7.83, 200.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Alpha 10 Hz",  "Binaural • 200 Hz carrier",
      10.0, 200.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Beta 16 Hz",   "Binaural • 250 Hz carrier",
      16.0, 250.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Gamma 40 Hz",  "Binaural • 300 Hz carrier",
      40.0, 300.0, EN_MODE_BINAURAL, EN_NOISE_NONE, 0.0 },
    { "Alpha + Pink", "Binaural • 200 Hz • pink bed",
      10.0, 200.0, EN_MODE_BINAURAL, EN_NOISE_PINK, 0.18 },
    { "Theta Speaker","Isochronic • 150 Hz carrier",
      6.0,  150.0, EN_MODE_ISOCHRONIC, EN_NOISE_NONE, 0.0 },
    { "Delta Brown",  "Binaural • 100 Hz • brown bed",
      2.0,  100.0, EN_MODE_BINAURAL, EN_NOISE_BROWN, 0.22 },
};

const en_preset_t *en_presets(int *count)
{
    if (count) *count = (int)(sizeof s_presets / sizeof s_presets[0]);
    return s_presets;
}

/* ---- programs ----------------------------------------------------------- */

static const en_prog_seg_t s_wind_down[] = {
    { 10.0, 6.0, 200.0, 1500, EN_NOISE_NONE, 0.0 },   /* 25 min alpha -> theta */
    {  6.0, 2.0, 150.0, 1200, EN_NOISE_NONE, 0.0 },   /* 20 min theta -> delta */
};

static const en_prog_seg_t s_power_nap[] = {
    {  8.0, 4.0, 180.0,  240, EN_NOISE_NONE, 0.0 },   /*  4 min descent  */
    {  4.0, 3.0, 150.0,  840, EN_NOISE_NONE, 0.0 },   /* 14 min low hold */
    {  3.0,12.0, 220.0,  120, EN_NOISE_NONE, 0.0 },   /*  2 min wake ramp */
};

static const en_prog_seg_t s_deep_focus[] = {
    { 14.0,18.0, 250.0, 1800, EN_NOISE_PINK, 0.16 },  /* 30 min rise  */
    { 18.0,14.0, 250.0, 1800, EN_NOISE_PINK, 0.16 },  /* 30 min back  */
};

static const en_prog_seg_t s_creative_drift[] = {
    {  7.5, 7.5, 180.0, 1800, EN_NOISE_NONE, 0.0 },   /* 30 min steady */
};

static const en_prog_seg_t s_morning_lift[] = {
    {  6.0,10.0, 200.0,  450, EN_NOISE_NONE, 0.0 },   /* 7.5 min theta -> alpha */
    { 10.0,18.0, 260.0,  450, EN_NOISE_NONE, 0.0 },   /* 7.5 min alpha -> beta  */
};

static const en_prog_seg_t s_meditation[] = {
    { 11.0, 8.0, 190.0,  900, EN_NOISE_NONE, 0.0 },   /* 15 min slow alpha */
    {  8.0, 5.5, 160.0,  900, EN_NOISE_NONE, 0.0 },   /* 15 min into theta */
};

#define PROG(arr) arr, (int)(sizeof arr / sizeof arr[0])

static const en_program_t s_programs[] = {
    { "Wind Down",
      "10-6-2 Hz • 200 Hz carrier",
      EN_MODE_BINAURAL, PROG(s_wind_down) },
    { "Power Nap",
      "8-3 Hz, then a wake ramp",
      EN_MODE_BINAURAL, PROG(s_power_nap) },
    { "Deep Focus",
      "14-18-14 Hz • pink bed",
      EN_MODE_BINAURAL, PROG(s_deep_focus) },
    { "Creative Drift",
      "Steady 7.5 Hz • 180 Hz",
      EN_MODE_BINAURAL, PROG(s_creative_drift) },
    { "Morning Lift",
      "6-18 Hz • carrier 200-260 Hz",
      EN_MODE_BINAURAL, PROG(s_morning_lift) },
    { "Meditation Descent",
      "11-5.5 Hz • long fades",
      EN_MODE_BINAURAL, PROG(s_meditation) },
};

const en_program_t *en_programs(int *count)
{
    if (count) *count = (int)(sizeof s_programs / sizeof s_programs[0]);
    return s_programs;
}

uint32_t en_program_seconds(const en_program_t *p)
{
    uint32_t t = 0;
    for (int i = 0; i < p->n_segs; i++) t += p->segs[i].seconds;
    return t;
}

en_band_t en_program_band(const en_program_t *p)
{
    uint32_t longest = 0;
    double beat = 10.0;
    for (int i = 0; i < p->n_segs; i++) {
        if (p->segs[i].seconds > longest) {
            longest = p->segs[i].seconds;
            beat = 0.5 * (p->segs[i].beat_start + p->segs[i].beat_end);
        }
    }
    return en_band_of(beat);
}

int en_program_seg_at(const en_program_t *p, double t_seconds)
{
    double acc = 0.0;
    for (int i = 0; i < p->n_segs; i++) {
        double next = acc + (double)p->segs[i].seconds;
        if (t_seconds < next) return i;
        acc = next;
    }
    return -1;
}

double en_program_beat_at(const en_program_t *p, double t_seconds)
{
    if (p->n_segs == 0) return 0.0;
    if (t_seconds < 0.0) t_seconds = 0.0;

    double acc = 0.0;
    for (int i = 0; i < p->n_segs; i++) {
        double dur = (double)p->segs[i].seconds;
        if (t_seconds < acc + dur) {
            double u = dur > 0.0 ? (t_seconds - acc) / dur : 0.0;
            return p->segs[i].beat_start
                 + (p->segs[i].beat_end - p->segs[i].beat_start) * u;
        }
        acc += dur;
    }
    return p->segs[p->n_segs - 1].beat_end;
}

/* ---- user program parser ------------------------------------------------ */
/* Freestanding: no stdio, no strtod, no locale. Just a scanner over a buffer. */

static int is_space(char c)   { return c == ' ' || c == '\t' || c == '\r'; }
static int is_digit(char c)   { return c >= '0' && c <= '9'; }

static void skip_space(const char **p, const char *end)
{
    while (*p < end && is_space(**p)) (*p)++;
}

/* Read a bare word into `dst`. Returns its length. */
static int read_word(const char **p, const char *end, char *dst, int cap)
{
    skip_space(p, end);
    int n = 0;
    while (*p < end && !is_space(**p) && **p != '\n') {
        if (n < cap - 1) dst[n] = **p;
        n++;
        (*p)++;
    }
    dst[n < cap ? n : cap - 1] = 0;
    return n;
}

/* Read a decimal number, optionally signed, optionally fractional. */
static int read_num(const char **p, const char *end, double *out)
{
    skip_space(p, end);
    const char *s = *p;
    int neg = 0;
    if (s < end && (*s == '-' || *s == '+')) { neg = (*s == '-'); s++; }

    int any = 0;
    double v = 0.0;
    while (s < end && is_digit(*s)) { v = v * 10.0 + (*s - '0'); s++; any = 1; }
    if (s < end && *s == '.') {
        s++;
        double scale = 0.1;
        while (s < end && is_digit(*s)) {
            v += (*s - '0') * scale;
            scale *= 0.1;
            s++;
            any = 1;
        }
    }
    if (!any) return 0;
    *p = s;
    *out = neg ? -v : v;
    return 1;
}

static int word_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void copy_rest_of_line(const char **p, const char *end,
                              char *dst, int cap)
{
    skip_space(p, end);
    int n = 0;
    while (*p < end && **p != '\n') {
        if (n < cap - 1) dst[n] = **p;
        n++;
        (*p)++;
    }
    /* trim trailing space */
    if (n > cap - 1) n = cap - 1;
    while (n > 0 && is_space(dst[n - 1])) n--;
    dst[n] = 0;
}

const char *en_parse_error_text(int code)
{
    switch (code) {
    case EN_PARSE_OK:             return "ok";
    case EN_PARSE_BAD_KEY:        return "unknown keyword";
    case EN_PARSE_BAD_VALUE:      return "bad value";
    case EN_PARSE_TOO_MANY_SEGS:  return "too many seg lines";
    case EN_PARSE_NO_SEGS:        return "no seg lines";
    default:                      return "error";
    }
}

int en_parse_program(const char *text, uint32_t len,
                     en_user_program_t *out, int *err_line)
{
    const char *p = text;
    const char *end = text + len;
    int line = 0;

    out->name[0] = 0;
    out->mode = EN_MODE_BINAURAL;
    out->carrier_hz = 200.0;
    out->noise = EN_NOISE_NONE;
    out->noise_level = 0.0;
    out->n_segs = 0;

    while (p < end) {
        line++;
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;

        const char *q = p;
        char key[16];
        skip_space(&q, line_end);

        if (q < line_end && *q != '#') {
            read_word(&q, line_end, key, sizeof key);

            if (word_eq(key, "name")) {
                copy_rest_of_line(&q, line_end, out->name, EN_NAME_MAX);

            } else if (word_eq(key, "mode")) {
                char v[16];
                read_word(&q, line_end, v, sizeof v);
                if      (word_eq(v, "binaural"))   out->mode = EN_MODE_BINAURAL;
                else if (word_eq(v, "isochronic")) out->mode = EN_MODE_ISOCHRONIC;
                else if (word_eq(v, "monaural"))   out->mode = EN_MODE_MONAURAL;
                else { if (err_line) *err_line = line; return EN_PARSE_BAD_VALUE; }

            } else if (word_eq(key, "carrier")) {
                double v;
                if (!read_num(&q, line_end, &v) || v < 20.0 || v > 1000.0) {
                    if (err_line) *err_line = line;
                    return EN_PARSE_BAD_VALUE;
                }
                out->carrier_hz = v;

            } else if (word_eq(key, "noise")) {
                char v[16];
                read_word(&q, line_end, v, sizeof v);
                if      (word_eq(v, "none"))  out->noise = EN_NOISE_NONE;
                else if (word_eq(v, "white")) out->noise = EN_NOISE_WHITE;
                else if (word_eq(v, "pink"))  out->noise = EN_NOISE_PINK;
                else if (word_eq(v, "brown")) out->noise = EN_NOISE_BROWN;
                else { if (err_line) *err_line = line; return EN_PARSE_BAD_VALUE; }
                double lv;
                out->noise_level = read_num(&q, line_end, &lv) ? lv : 0.15;
                if (out->noise_level < 0.0) out->noise_level = 0.0;
                if (out->noise_level > 1.0) out->noise_level = 1.0;

            } else if (word_eq(key, "seg")) {
                if (out->n_segs >= EN_USER_MAX_SEGS) {
                    if (err_line) *err_line = line;
                    return EN_PARSE_TOO_MANY_SEGS;
                }
                double b0, b1, secs;
                if (!read_num(&q, line_end, &b0) ||
                    !read_num(&q, line_end, &b1) ||
                    !read_num(&q, line_end, &secs) ||
                    b0 <= 0.0 || b1 <= 0.0 || b0 > 100.0 || b1 > 100.0 ||
                    secs < 1.0) {
                    if (err_line) *err_line = line;
                    return EN_PARSE_BAD_VALUE;
                }
                en_prog_seg_t *s = &out->segs[out->n_segs++];
                s->beat_start  = b0;
                s->beat_end    = b1;
                s->carrier_hz  = out->carrier_hz;
                s->seconds     = (uint32_t)secs;
                s->noise       = out->noise;
                s->noise_level = out->noise_level;

            } else {
                if (err_line) *err_line = line;
                return EN_PARSE_BAD_KEY;
            }
        }

        p = line_end < end ? line_end + 1 : end;
    }

    if (out->n_segs == 0) {
        if (err_line) *err_line = line;
        return EN_PARSE_NO_SEGS;
    }
    if (out->name[0] == 0) {
        const char *d = "Untitled";
        int i = 0;
        while (d[i] && i < EN_NAME_MAX - 1) { out->name[i] = d[i]; i++; }
        out->name[i] = 0;
    }
    return EN_PARSE_OK;
}

/*
 * render.c — see render.h.
 */

#include "render.h"

void en_render_init(en_render_t *r, uint32_t sample_rate,
                    en_noise_kind_t noise, uint32_t seed)
{
    en_osc_init();
    r->sample_rate = sample_rate;
    r->osc_l.phase = 0; r->osc_l.step = 0;
    r->osc_r.phase = 0; r->osc_r.step = 0;
    r->osc_gate.phase = 0; r->osc_gate.step = 0;
    en_noise_init(&r->noise, noise, seed);
}

double en_softclip(double x)
{
    if (x <= -1.5) return -1.0;
    if (x >=  1.5) return  1.0;
    return x - (4.0 / 27.0) * x * x * x;
}

static int16_t to_s16(double x)
{
    double v = en_softclip(x) * 32767.0;
    if (v >  32767.0) v =  32767.0;
    if (v < -32768.0) v = -32768.0;
    return (int16_t)(v < 0.0 ? v - 0.5 : v + 0.5);
}

/* Raised-cosine gain, rising over [0,1]. Zero slope at both ends, which is what
   keeps a fade from clicking at the moment it finishes. */
static double raised_cos(double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return 0.5 - 0.5 * en_sin_turns(0.25 + t * 0.5);
}

static double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

/* The isochronic gate. A raised cosine at the beat rate rather than a square
   wave: a square gate is all harmonics and clicks twice per cycle, which is
   exactly the artefact this app cannot afford. */
static double gate_value(uint32_t phase)
{
    /* cos(x) = sin(x + quarter turn) */
    double c = (double)en_sine_lookup(phase + 0x40000000u) / 32767.0;
    return 0.5 - 0.5 * c;
}

static void render_inner(en_render_t *r, const en_segment_t *seg, int16_t *out,
                         int apply_fades)
{
    const uint32_t sr = seg->sample_rate ? seg->sample_rate : r->sample_rate;
    const uint32_t n = seg->frames;
    if (n == 0) return;

    const double fade_in_frames  = apply_fades ? seg->fade_in_s  * (double)sr : 0.0;
    const double fade_out_frames = apply_fades ? seg->fade_out_s * (double)sr : 0.0;
    const double inv_n = 1.0 / (double)n;

    double tone_level = seg->start.tone_level;
    double noise_level = seg->start.noise_level;

    /* The segment decides the noise kind, not whoever called en_render_init.
       Without this a preset with a pink bed renders through whatever generator
       the renderer happened to be initialised with — and if that was NONE, the
       bed is silent while the level says otherwise. Re-seed only on a real
       change, so a steady render keeps one continuous noise stream. */
    if (seg->noise != r->noise.kind)
        en_noise_init(&r->noise, seg->noise, 0x9E3779B9u);

    for (uint32_t i = 0; i < n; i += EN_CTRL_BLOCK) {
        uint32_t block = n - i;
        if (block > EN_CTRL_BLOCK) block = EN_CTRL_BLOCK;

        /* Control rate: recompute the ramped parameters and the phase steps
           once per block, then run the block with fixed steps. */
        double t = (double)i * inv_n;
        double carrier = lerp(seg->start.carrier_hz, seg->end.carrier_hz, t);
        double beat    = lerp(seg->start.beat_hz,    seg->end.beat_hz,    t);
        tone_level     = lerp(seg->start.tone_level, seg->end.tone_level, t);
        noise_level    = lerp(seg->start.noise_level, seg->end.noise_level, t);

        switch (seg->mode) {
        case EN_MODE_BINAURAL:
        case EN_MODE_MONAURAL:
            en_osc_set_freq(&r->osc_l, carrier, sr);
            en_osc_set_freq(&r->osc_r, carrier + beat, sr);
            break;
        case EN_MODE_ISOCHRONIC:
            en_osc_set_freq(&r->osc_l, carrier, sr);
            en_osc_set_freq(&r->osc_gate, beat, sr);
            break;
        }

        for (uint32_t k = 0; k < block; k++) {
            uint32_t idx = i + k;

            double env = 1.0;
            if (apply_fades) {
                if (fade_in_frames > 0.0 && (double)idx < fade_in_frames)
                    env = raised_cos((double)idx / fade_in_frames);
                if (fade_out_frames > 0.0) {
                    double left = (double)(n - 1 - idx);
                    if (left < fade_out_frames) {
                        double g = raised_cos(left / fade_out_frames);
                        if (g < env) env = g;
                    }
                }
            }

            double l, rr;
            switch (seg->mode) {
            case EN_MODE_BINAURAL: {
                l  = (double)en_osc_next(&r->osc_l) / 32767.0;
                rr = (double)en_osc_next(&r->osc_r) / 32767.0;
                break;
            }
            case EN_MODE_MONAURAL: {
                double a = (double)en_osc_next(&r->osc_l) / 32767.0;
                double b = (double)en_osc_next(&r->osc_r) / 32767.0;
                l = rr = 0.5 * (a + b);
                break;
            }
            case EN_MODE_ISOCHRONIC:
            default: {
                double a = (double)en_osc_next(&r->osc_l) / 32767.0;
                double g = gate_value(r->osc_gate.phase);
                r->osc_gate.phase += r->osc_gate.step;
                l = rr = a * g;
                break;
            }
            }

            l  *= tone_level * env;
            rr *= tone_level * env;

            if (seg->noise != EN_NOISE_NONE && noise_level > 0.0) {
                /* One noise stream, same in both ears. Decorrelated noise would
                   widen the image and fight the binaural cue we are trying to
                   present cleanly. */
                double nz = en_noise_next(&r->noise) * noise_level * env;
                l  += nz;
                rr += nz;
            }

            out[2 * idx + 0] = to_s16(l  * EN_HEADROOM);
            out[2 * idx + 1] = to_s16(rr * EN_HEADROOM);
        }
    }
}

void en_render_segment(en_render_t *r, const en_segment_t *seg, int16_t *out)
{
    render_inner(r, seg, out, 1);
}

/* Fixed seed for looped renders, so a preset always renders to identical bytes
   and the render cache key stays honest. */
#define EN_LOOP_SEED 0x9E3779B9u

/* Frames of noise crossfade at the loop seam. 256 frames is 23 ms at 11025 Hz
   — long enough to hide the join, short enough that the buffer is 1 KB. */
#define EN_LOOP_XFADE 256

void en_render_loop(en_render_t *r, const en_segment_t *seg, int16_t *out)
{
    /* A loop must render identically every time and must wrap onto itself, so
       it starts from a known phase rather than wherever the last segment left
       off. The caller guarantees whole cycle counts; see en_plan_loop. */
    r->osc_l.phase = 0;
    r->osc_r.phase = 0;
    r->osc_gate.phase = 0;
    en_noise_init(&r->noise, seg->noise, EN_LOOP_SEED);
    render_inner(r, seg, out, 0);

    /* The tones wrap exactly — whole cycle counts see to that. The noise does
       not: sample N-1 and sample 0 come from unrelated points in the stream,
       and for brown noise that is an audible step, not a hiss.
       Fix it by crossfading the stream's natural continuation past the end
       into the first few frames. After the blend, frame N-1 is followed by
       what the generator would genuinely have produced next, so the wrap is
       as continuous as any interior sample. */
    if (seg->noise != EN_NOISE_NONE && seg->start.noise_level > 0.0 &&
        seg->frames > 2 * EN_LOOP_XFADE) {

        float cont[EN_LOOP_XFADE];
        for (int k = 0; k < EN_LOOP_XFADE; k++)
            cont[k] = (float)en_noise_next(&r->noise);   /* continues past N */

        /* Regenerate the head of the stream — deterministic from the seed. */
        en_noise_t head;
        en_noise_init(&head, seg->noise, EN_LOOP_SEED);

        const uint32_t sr = seg->sample_rate ? seg->sample_rate : r->sample_rate;
        const double lvl = seg->start.tone_level;
        const double nlvl = seg->start.noise_level;
        uint32_t step_l = en_osc_step(seg->start.carrier_hz, sr);
        uint32_t step_r = en_osc_step(seg->start.carrier_hz + seg->start.beat_hz, sr);
        uint32_t step_g = en_osc_step(seg->start.beat_hz, sr);

        for (int k = 0; k < EN_LOOP_XFADE; k++) {
            double w = (double)k / (double)EN_LOOP_XFADE;   /* 0 -> 1 */
            double nz = ((double)cont[k] * (1.0 - w)
                         + en_noise_next(&head) * w) * nlvl;

            /* Recompute this frame's tone. A loop is steady, so the phase at
               frame k is exactly k * step and no ramp state is needed. */
            double l, rr;
            switch (seg->mode) {
            case EN_MODE_BINAURAL:
                l  = (double)en_sine_lookup((uint32_t)k * step_l) / 32767.0;
                rr = (double)en_sine_lookup((uint32_t)k * step_r) / 32767.0;
                break;
            case EN_MODE_MONAURAL: {
                double a = (double)en_sine_lookup((uint32_t)k * step_l) / 32767.0;
                double b = (double)en_sine_lookup((uint32_t)k * step_r) / 32767.0;
                l = rr = 0.5 * (a + b);
                break;
            }
            case EN_MODE_ISOCHRONIC:
            default: {
                double a = (double)en_sine_lookup((uint32_t)k * step_l) / 32767.0;
                l = rr = a * gate_value((uint32_t)k * step_g);
                break;
            }
            }

            out[2 * k + 0] = to_s16((l  * lvl + nz) * EN_HEADROOM);
            out[2 * k + 1] = to_s16((rr * lvl + nz) * EN_HEADROOM);
        }
    }

    r->osc_l.phase = 0;
    r->osc_r.phase = 0;
    r->osc_gate.phase = 0;
}

uint32_t en_zero_cut(const int16_t *pcm, uint32_t lo, uint32_t hi)
{
    if (hi <= lo + 1) return hi;

    uint32_t best = hi;
    int32_t  best_cost = 0x7FFFFFFF;

    for (uint32_t k = lo; k + 1 < hi; k++) {
        int32_t l0 = pcm[2 * k + 0], r0 = pcm[2 * k + 1];
        int32_t l1 = pcm[2 * (k + 1) + 0], r1 = pcm[2 * (k + 1) + 1];

        /* The LARGEST of the samples straddling the boundary, across both
           channels. Not their sum: what a gap costs is the biggest single step
           the output takes, and a boundary with one small sample and one large
           one is no good however flattering its total. */
        int32_t cost = (l0 < 0 ? -l0 : l0);
        int32_t v;
        v = (r0 < 0 ? -r0 : r0); if (v > cost) cost = v;
        v = (l1 < 0 ? -l1 : l1); if (v > cost) cost = v;
        v = (r1 < 0 ? -r1 : r1); if (v > cost) cost = v;

        if (cost < best_cost) { best_cost = cost; best = k + 1; }
    }
    return best;
}

const char *en_mode_name(en_mode_t mode)
{
    switch (mode) {
    case EN_MODE_BINAURAL:   return "Binaural";
    case EN_MODE_ISOCHRONIC: return "Isochronic";
    case EN_MODE_MONAURAL:   return "Monaural";
    default:                 return "?";
    }
}

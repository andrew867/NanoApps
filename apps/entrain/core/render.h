/*
 * render.h — turn a parameter ramp into interleaved 16-bit stereo PCM.
 *
 * The renderer holds its oscillator phases across calls. That is the whole
 * reason it is a struct and not a function: a program is a chain of segments,
 * and if segment N+1 restarted its phase at zero, every join would click. Call
 * en_render_segment() repeatedly on one en_render_t and the joins are
 * continuous by construction.
 *
 * Parameters ramp linearly from `start` to `end` across the segment, updated
 * once per 64-sample control block — 5.8 ms at 11025 Hz, far finer than the
 * 50-200 ms glide the UI asks for and far coarser than recomputing a phase step
 * per sample.
 */

#ifndef ENTRAIN_RENDER_H
#define ENTRAIN_RENDER_H

#include <stdint.h>
#include "osc.h"
#include "noise.h"

/* Control-block size. Anything from 16 to 256 is defensible; 64 keeps the
   glide smooth without recomputing steps every sample. */
#define EN_CTRL_BLOCK 64

/* Peak headroom for the mix. -3 dB, so two loops overlapping at a crossfade
   cannot sum past full scale and hit the soft clipper. */
#define EN_HEADROOM 0.707

typedef enum {
    /* Different frequency to each ear. The beat exists only in the listener;
       nothing in the signal oscillates at the beat rate. Headphones required. */
    EN_MODE_BINAURAL = 0,
    /* One carrier, gated at the beat rate. The modulation is in the signal, so
       it survives speakers. */
    EN_MODE_ISOCHRONIC,
    /* Both tones in both ears. The beat is a real amplitude beat in the air. */
    EN_MODE_MONAURAL
} en_mode_t;

typedef struct {
    double carrier_hz;    /* binaural: the left frequency. Others: the carrier */
    double beat_hz;
    double tone_level;    /* 0..1 */
    double noise_level;   /* 0..1 */
} en_params_t;

typedef struct {
    uint32_t        sample_rate;
    en_mode_t       mode;
    en_noise_kind_t noise;
    en_params_t     start;
    en_params_t     end;      /* equal to start for a steady segment */
    uint32_t        frames;
    double          fade_in_s;   /* raised cosine; 0 for a mid-program segment */
    double          fade_out_s;
} en_segment_t;

typedef struct {
    uint32_t   sample_rate;
    en_osc_t   osc_l;     /* left / sole carrier */
    en_osc_t   osc_r;     /* right carrier (binaural, monaural) */
    en_osc_t   osc_gate;  /* isochronic amplitude gate */
    en_noise_t noise;
} en_render_t;

void en_render_init(en_render_t *r, uint32_t sample_rate,
                    en_noise_kind_t noise, uint32_t seed);

/* Render `seg->frames` frames into `out`, which must hold frames*2 int16.
   Advances the renderer's phases, so the next call continues seamlessly. */
void en_render_segment(en_render_t *r, const en_segment_t *seg, int16_t *out);

/* Render a seamless loop: no fades, and the phases are seeded from zero and
   land back on zero at the wrap, provided the caller passed cycle-exact
   frequencies (see program.h's loop planner). Leaves the renderer's phases
   where the loop started, so calling it twice gives identical bytes. */
void en_render_loop(en_render_t *r, const en_segment_t *seg, int16_t *out);

const char *en_mode_name(en_mode_t mode);

/* Cubic soft clip: unity slope through zero, saturating smoothly at +/-1.
   Exposed for the tests, which assert the output never wraps. */
double en_softclip(double x);

#endif /* ENTRAIN_RENDER_H */

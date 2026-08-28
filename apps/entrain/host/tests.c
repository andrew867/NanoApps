/*
 * tests.c — host unit tests for apps/entrain/core.
 *
 * These run on the development machine, not the iPod. core/ is pure C99 with
 * no LVGL and no SDK, precisely so that the DSP can be tested (and heard) here
 * rather than by squinting at a 240x432 screen over USB.
 *
 * The two that matter most are the click test and the loop-seam test. Every
 * other property can be a bit off and the app still sounds fine; a click at a
 * segment join or a loop wrap is the one artefact that makes the whole thing
 * feel broken, and it is exactly the kind of bug that creeps back in.
 *
 *     make -f Makefile.host test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../core/osc.h"
#include "../core/noise.h"
#include "../core/render.h"
#include "../core/program.h"
#include "../core/wavout.h"

static int g_checks, g_fails;

#define CHECK(cond, ...) do {                                                 \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fails++;                                                        \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static void section(const char *name)
{
    printf("\n== %s\n", name);
}

/* ---- helpers ------------------------------------------------------------ */

/* Frequency of one channel by positive-going zero crossings, interpolated.
   Accurate to well under a millihertz on a clean sine over a few seconds. */
static double measure_freq(const int16_t *pcm, uint32_t frames, int channel,
                           uint32_t sample_rate)
{
    double first = -1.0, last = -1.0;
    long crossings = 0;

    for (uint32_t i = 1; i < frames; i++) {
        int16_t a = pcm[2 * (i - 1) + channel];
        int16_t b = pcm[2 * i + channel];
        if (a < 0 && b >= 0) {
            /* linear interpolation of the crossing instant */
            double frac = (double)(-a) / (double)(b - a);
            double t = ((double)(i - 1) + frac) / (double)sample_rate;
            if (first < 0.0) first = t;
            last = t;
            crossings++;
        }
    }
    if (crossings < 2) return 0.0;
    return (double)(crossings - 1) / (last - first);
}

/* Largest absolute sample-to-sample step in one channel over [from, to). */
static int max_step(const int16_t *pcm, uint32_t from, uint32_t to, int channel)
{
    int m = 0;
    for (uint32_t i = from + 1; i < to; i++) {
        int d = pcm[2 * i + channel] - pcm[2 * (i - 1) + channel];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

static int peak_abs(const int16_t *pcm, uint32_t frames)
{
    int m = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        int v = pcm[i];
        if (v < 0) v = -v;
        if (v > m) m = v;
    }
    return m;
}

/* ---- 1. oscillator and sine table --------------------------------------- */

static void test_sine(void)
{
    section("sine table + oscillator");
    en_osc_init();

    /* The series-based reference has to be right before the table can be
       judged against it. */
    CHECK(fabs(en_sin_turns(0.0)) < 1e-12, "sin(0)");
    CHECK(fabs(en_sin_turns(0.25) - 1.0) < 1e-9, "sin(pi/2) = %.12f",
          en_sin_turns(0.25));
    CHECK(fabs(en_sin_turns(0.5)) < 1e-9, "sin(pi) = %.12f", en_sin_turns(0.5));
    CHECK(fabs(en_sin_turns(0.75) + 1.0) < 1e-9, "sin(3pi/2)");

    double worst = 0.0;
    for (int i = 0; i < 100000; i++) {
        double turns = (double)i / 100000.0;
        double want = en_sin_turns(turns);
        double got = (double)en_sine_lookup((uint32_t)(turns * 4294967296.0))
                     / 32767.0;
        double e = fabs(want - got);
        if (e > worst) worst = e;
    }
    /* 2048 entries with linear interpolation: the interpolation error dominates
       and lands near 2e-6 of full scale. Allow a little slack for the 16-bit
       quantisation of the table itself. */
    printf("  worst LUT error: %.3e of full scale\n", worst);
    CHECK(worst < 1e-4, "LUT interpolation error %.3e too large", worst);

    /* The accumulator must not drift: after a whole number of cycles the phase
       must land back within rounding of where it started. */
    en_osc_t o = { 0, 0 };
    uint32_t sr = 11025;
    en_osc_set_freq(&o, 200.0, sr);
    for (uint32_t i = 0; i < sr; i++) o.phase += o.step;   /* exactly 1 second */
    double turns = en_osc_phase_turns(&o);
    double off = turns > 0.5 ? 1.0 - turns : turns;
    printf("  phase after 200 Hz x 1 s: %.9f turns from zero\n", off);
    CHECK(off < 1e-5, "accumulator drifted %.9f turns", off);
}

/* ---- 2. the loop planner ------------------------------------------------- */

static void test_planner(void)
{
    section("loop planner");

    en_loop_plan_t p;
    int ok = en_plan_loop(7.83, 200.0, EN_RATES, EN_RATES_COUNT,
                          EN_TARGET_BYTES, &p);
    CHECK(ok, "no plan for Schumann");
    if (ok) {
        printf("  Schumann: SR=%u N=%u T=%.6f s beat=%.9f Hz "
               "f_L=%.4f f_R=%.4f %u bytes\n",
               p.sample_rate, p.frames, p.t_seconds, p.beat_hz,
               p.f_l, p.f_r, p.bytes);
        /* This one has an exact solution and the planner must find it. */
        CHECK(fabs(p.beat_error) < 1e-9,
              "Schumann beat error %.9f Hz, expected exact", p.beat_error);
        CHECK(p.bytes <= EN_TARGET_BYTES, "plan is %u bytes, over budget",
              p.bytes);
        CHECK(p.bytes < EN_MAX_LOAD_BYTES,
              "plan would be rejected by the loader");
        /* Whole cycle counts are what make the seam work at all. */
        CHECK(fabs(p.f_l * p.t_seconds - (double)p.cycles_l) < 1e-6,
              "f_L is not a whole number of cycles");
        CHECK(fabs(p.f_r * p.t_seconds
                   - (double)(p.cycles_l + p.cycles_beat)) < 1e-6,
              "f_R is not a whole number of cycles");
        /* The carrier may move; the beat may not. */
        CHECK(fabs(p.f_l - 200.0) < 1.0,
              "carrier moved %.3f Hz, too far", fabs(p.f_l - 200.0));
    }

    /* Every shipped preset must plan, stay in budget, and land close on beat. */
    int n;
    const en_preset_t *ps = en_presets(&n);
    for (int i = 0; i < n; i++) {
        en_loop_plan_t q;
        int good = en_plan_loop(ps[i].beat_hz, ps[i].carrier_hz,
                                EN_RATES, EN_RATES_COUNT, EN_TARGET_BYTES, &q);
        CHECK(good, "%s: no plan", ps[i].name);
        if (!good) continue;
        printf("  %-14s %5u Hz  T=%6.3f s  beat %.6f (%+.6f)  %7u B\n",
               ps[i].name, q.sample_rate, q.t_seconds, q.beat_hz,
               q.beat_error, q.bytes);
        CHECK(fabs(q.beat_error) < 0.01,
              "%s: realised beat off by %.6f Hz", ps[i].name, q.beat_error);
        CHECK(q.bytes <= EN_TARGET_BYTES, "%s: %u bytes over budget",
              ps[i].name, q.bytes);
    }

    /* A beat too slow to fit a whole cycle in the budget must fail cleanly
       rather than return nonsense. */
    en_loop_plan_t tiny;
    CHECK(en_plan_loop(0.01, 200.0, EN_RATES, EN_RATES_COUNT, 4096, &tiny) == 0,
          "planner should refuse an impossible budget");
}

/* ---- 3. beat accuracy --------------------------------------------------- */

static void test_beat_accuracy(void)
{
    section("beat accuracy");

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 10;    /* 10 s */
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    struct { double carrier, beat; } cases[] = {
        { 200.0,  7.83 }, { 100.0, 2.0 }, { 250.0, 16.0 }, { 300.0, 40.0 },
    };

    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        en_render_t r;
        en_render_init(&r, sr, EN_NOISE_NONE, 1);

        en_segment_t seg;
        memset(&seg, 0, sizeof seg);
        seg.sample_rate = sr;
        seg.mode = EN_MODE_BINAURAL;
        seg.noise = EN_NOISE_NONE;
        seg.frames = frames;
        seg.start.carrier_hz = cases[c].carrier;
        seg.start.beat_hz = cases[c].beat;
        seg.start.tone_level = 0.8;
        seg.start.noise_level = 0.0;
        seg.end = seg.start;
        en_render_segment(&r, &seg, pcm);

        double fl = measure_freq(pcm, frames, 0, sr);
        double fr = measure_freq(pcm, frames, 1, sr);
        double beat = fr - fl;
        printf("  carrier %.2f beat %.2f -> L=%.5f R=%.5f measured beat %.5f\n",
               cases[c].carrier, cases[c].beat, fl, fr, beat);
        CHECK(fabs(beat - cases[c].beat) < 0.01,
              "beat measured %.5f, wanted %.5f", beat, cases[c].beat);
        CHECK(fabs(fl - cases[c].carrier) < 0.01,
              "left carrier measured %.5f, wanted %.5f", fl, cases[c].carrier);
    }

    free(pcm);
}

/* ---- 4. the click test -------------------------------------------------- */

/* Render a chain of segments through one renderer and assert that the joins
   are not visible in the waveform. This is the test that keeps the app feeling
   premium; if it ever fails, something is resetting phase between segments. */
static void test_clicks(void)
{
    section("click test: segment joins");

    const uint32_t sr = 11025;
    const uint32_t seg_frames = sr * 2;    /* 2 s each */
    const int n_segs = 5;
    const uint32_t total = seg_frames * n_segs;
    int16_t *pcm = malloc(total * 2 * sizeof *pcm);

    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_NONE, 1);

    /* A deliberately awkward chain: the beat and the carrier both move at
       every join, which is the case a naive renderer gets wrong. */
    double beats[]    = { 10.0, 8.0, 6.0, 4.0, 2.0, 2.0 };
    double carriers[] = { 200.0, 190.0, 180.0, 160.0, 140.0, 140.0 };

    for (int s = 0; s < n_segs; s++) {
        en_segment_t seg;
        memset(&seg, 0, sizeof seg);
        seg.sample_rate = sr;
        seg.mode = EN_MODE_BINAURAL;
        seg.noise = EN_NOISE_NONE;
        seg.frames = seg_frames;
        seg.start.carrier_hz = carriers[s];
        seg.start.beat_hz    = beats[s];
        seg.start.tone_level = 0.8;
        seg.end.carrier_hz   = carriers[s + 1];
        seg.end.beat_hz      = beats[s + 1];
        seg.end.tone_level   = 0.8;
        seg.end.noise_level  = 0.0;
        en_render_segment(&r, &seg, pcm + (size_t)s * seg_frames * 2);
    }

    for (int ch = 0; ch < 2; ch++) {
        /* Baseline: the largest step anywhere well away from a join. */
        int baseline = 0;
        for (int s = 0; s < n_segs; s++) {
            uint32_t a = s * seg_frames + 64;
            uint32_t b = (s + 1) * seg_frames - 64;
            int m = max_step(pcm, a, b, ch);
            if (m > baseline) baseline = m;
        }
        for (int s = 1; s < n_segs; s++) {
            uint32_t j = s * seg_frames;
            int d = pcm[2 * j + ch] - pcm[2 * (j - 1) + ch];
            if (d < 0) d = -d;
            printf("  ch%d join %d: step %d (interior max %d)\n",
                   ch, s, d, baseline);
            CHECK(d <= baseline + 8,
                  "ch%d join %d steps %d against an interior max of %d "
                  "- that is a click", ch, s, d, baseline);
        }
    }

    free(pcm);
}

/* ---- 5. the loop-seam test ---------------------------------------------- */

static void test_loop_seams(void)
{
    section("loop seam: every shipped preset");

    int n;
    const en_preset_t *ps = en_presets(&n);

    for (int i = 0; i < n; i++) {
        en_loop_plan_t plan;
        if (!en_plan_loop(ps[i].beat_hz, ps[i].carrier_hz,
                          EN_RATES, EN_RATES_COUNT, EN_TARGET_BYTES, &plan)) {
            CHECK(0, "%s: no plan", ps[i].name);
            continue;
        }

        en_segment_t seg;
        en_plan_to_segment(&plan, ps[i].mode, ps[i].noise, 0.8,
                           ps[i].noise_level, &seg);

        int16_t *pcm = malloc((size_t)plan.frames * 2 * sizeof *pcm);
        en_render_t r;
        en_render_init(&r, plan.sample_rate, ps[i].noise, 1);
        en_render_loop(&r, &seg, pcm);

        int worst_seam = 0, worst_base = 0;
        for (int ch = 0; ch < 2; ch++) {
            /* Skip the noise crossfade region when taking the baseline, so the
               baseline reflects the steady state rather than the blend. */
            int baseline = max_step(pcm, 512, plan.frames - 2, ch);
            int seam = pcm[2 * 0 + ch] - pcm[2 * (plan.frames - 1) + ch];
            if (seam < 0) seam = -seam;
            if (seam > worst_seam) worst_seam = seam;
            if (baseline > worst_base) worst_base = baseline;
        }
        printf("  %-14s seam step %5d vs interior max %5d  (%.3f s loop)\n",
               ps[i].name, worst_seam, worst_base, plan.t_seconds);
        /* The wrap must look like any other sample transition. A little slack
           because a wrap can legitimately land on the steepest part of the
           waveform where an interior sample did not. */
        CHECK(worst_seam <= (worst_base * 5) / 4 + 8,
              "%s: loop seam steps %d against an interior max of %d",
              ps[i].name, worst_seam, worst_base);

        /* Rendering the same plan twice must give identical bytes, or the
           render cache is lying about what it holds. */
        int16_t *again = malloc((size_t)plan.frames * 2 * sizeof *again);
        en_render_t r2;
        en_render_init(&r2, plan.sample_rate, ps[i].noise, 1);
        en_render_loop(&r2, &seg, again);
        CHECK(memcmp(pcm, again, (size_t)plan.frames * 2 * sizeof *pcm) == 0,
              "%s: render is not deterministic", ps[i].name);

        free(again);
        free(pcm);
    }
}

/* A preset that asks for a noise bed must actually get one. The renderer used
   to take its noise kind from en_render_init rather than from the segment, so
   a pink-bed preset rendered through a NONE generator and the bed was silent
   while the level said otherwise. */
static void test_noise_bed(void)
{
    section("noise beds actually produce noise");

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 2;
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    en_noise_kind_t kinds[] = { EN_NOISE_WHITE, EN_NOISE_PINK, EN_NOISE_BROWN };
    const char *names[] = { "white", "pink", "brown" };

    for (int k = 0; k < 3; k++) {
        en_render_t r;
        /* Deliberately initialise with the WRONG kind: the segment must win. */
        en_render_init(&r, sr, EN_NOISE_NONE, 1);

        en_segment_t seg;
        memset(&seg, 0, sizeof seg);
        seg.sample_rate = sr;
        seg.mode = EN_MODE_BINAURAL;
        seg.noise = kinds[k];
        seg.frames = frames;
        seg.start.carrier_hz = 200.0;
        seg.start.beat_hz = 10.0;
        seg.start.tone_level = 0.0;    /* noise only, so it is measurable */
        seg.start.noise_level = 0.5;
        seg.end = seg.start;
        en_render_segment(&r, &seg, pcm);

        int peak = peak_abs(pcm, frames);
        printf("  %-6s bed peak %d\n", names[k], peak);
        CHECK(peak > 500, "%s bed is silent (peak %d) - the segment's kind "
              "was ignored", names[k], peak);
    }

    /* And NONE must stay silent even when a level is set. */
    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_PINK, 1);
    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_NONE;
    seg.frames = frames;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 0.0;
    seg.start.noise_level = 0.5;
    seg.end = seg.start;
    en_render_segment(&r, &seg, pcm);
    CHECK(peak_abs(pcm, frames) == 0, "EN_NOISE_NONE produced sound");

    free(pcm);
}

/* ---- overlap-add reconstruction ------------------------------------------
 *
 * The device plays a window at a time and overlap-adds consecutive windows,
 * because nothing can silence a voice already sounding and the pool mixes
 * rather than cuts. This checks the join maths, which has been wrong once
 * already: a short crossfade only sums to unity when the two windows line up
 * exactly, so a late tick turned into a 40% amplitude dip twice a second - an
 * audible tremolo. Overlap-add with a full-advance triangle makes the same
 * lateness cost d/advance instead of d/crossfade.
 *
 * The gain formula here mirrors audio_device.c exactly. */
static int32_t ola_gain(uint32_t i, uint32_t advance)
{
    return (i < advance)
         ? (int32_t)((i * 4096u) / advance)
         : 4096 - (int32_t)(((i - advance) * 4096u) / advance);
}

static void test_overlap_add(void)
{
    section("overlap-add reconstruction");

    const uint32_t sr = 22050;
    const uint32_t advance = sr;          /* 1 s advance, 2 s buffers */
    const uint32_t buflen = advance * 2;
    const uint32_t total = advance * 6;

    /* One continuous signal, the reference the joins must reproduce. */
    int16_t *ref = malloc((size_t)total * 2 * sizeof *ref);
    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_NONE, 1);
    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_NONE;
    seg.frames = total;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 0.8;
    seg.end = seg.start;
    en_render_segment(&r, &seg, ref);

    /* Now rebuild it the way the device does: windows of 2 x advance, each
       triangular, summed at one advance apart. */
    int32_t *acc = calloc((size_t)total * 2, sizeof *acc);
    for (uint32_t start = 0; start + buflen <= total; start += advance) {
        for (uint32_t i = 0; i < buflen; i++) {
            int32_t g = ola_gain(i, advance);
            acc[2 * (start + i) + 0] += (ref[2 * (start + i) + 0] * g) >> 12;
            acc[2 * (start + i) + 1] += (ref[2 * (start + i) + 1] * g) >> 12;
        }
    }

    /* Only the fully-covered interior is expected to reconstruct: the first
       and last advance are covered by one window each, which is the app's
       fade in and fade out. */
    int worst = 0;
    for (uint32_t i = advance; i < total - buflen; i++) {
        for (int ch = 0; ch < 2; ch++) {
            int d = acc[2 * i + ch] - ref[2 * i + ch];
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
    }
    printf("  aligned: worst reconstruction error %d of 32767\n", worst);
    CHECK(worst <= 4, "overlap-add does not reconstruct (off by %d)", worst);

    /* And the property that actually matters: being LATE must degrade
       gently. Shift every other window by a plausible tick and measure the
       amplitude dip. */
    for (uint32_t late_ms = 16; late_ms <= 32; late_ms += 16) {
        uint32_t d = (sr * late_ms) / 1000u;
        memset(acc, 0, (size_t)total * 2 * sizeof *acc);
        uint32_t k = 0;
        for (uint32_t start = 0; start + buflen + d <= total; start += advance, k++) {
            uint32_t at = start + ((k & 1) ? d : 0);
            for (uint32_t i = 0; i < buflen; i++) {
                int32_t g = ola_gain(i, advance);
                acc[2 * (at + i) + 0] += (ref[2 * (at + i) + 0] * g) >> 12;
                acc[2 * (at + i) + 1] += (ref[2 * (at + i) + 1] * g) >> 12;
            }
        }
        /* Compare envelopes rather than samples: a shifted window reproduces
           the same waveform at a slightly different gain. */
        int peak_ref = 0, peak_min = 32767;
        for (uint32_t i = advance * 2; i < total - buflen - d; i += advance / 4) {
            int hi = 0, lo = 32767;
            for (uint32_t j = i; j < i + advance / 4 && j < total; j++) {
                int v = acc[2 * j] < 0 ? -acc[2 * j] : acc[2 * j];
                if (v > hi) hi = v;
                int w = ref[2 * j] < 0 ? -ref[2 * j] : ref[2 * j];
                if (w > peak_ref) peak_ref = w;
            }
            (void)lo;
            if (hi < peak_min) peak_min = hi;
        }
        int dip_pct = peak_ref ? 100 - (peak_min * 100) / peak_ref : 100;
        printf("  %2u ms late: worst dip %d%%\n", late_ms, dip_pct);
        /* The short-crossfade version dipped 40-75%% here. Anything in single
           figures is inaudible. */
        CHECK(dip_pct <= 12, "%u ms of lateness dips %d%% - that is audible",
              late_ms, dip_pct);
    }

    free(acc);
    free(ref);
}

/* ---- 6. phase continuity across a glide --------------------------------- */

static void test_glide(void)
{
    section("phase continuity across a parameter glide");

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 4;
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_NONE, 1);

    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_NONE;
    seg.frames = frames;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 0.8;
    seg.end.carrier_hz = 260.0;     /* a big, fast sweep */
    seg.end.beat_hz = 4.0;
    seg.end.tone_level = 0.8;
    seg.end.noise_level = 0.0;
    en_render_segment(&r, &seg, pcm);

    /* A frequency sweep should be smooth everywhere. The control blocks update
       the phase step every 64 samples; if that were done by resetting phase
       rather than by changing the increment, there would be a step at every
       block boundary. Compare the worst step at a block boundary against the
       worst step away from one. */
    for (int ch = 0; ch < 2; ch++) {
        int on_boundary = 0, off_boundary = 0;
        for (uint32_t i = 1; i < frames; i++) {
            int d = pcm[2 * i + ch] - pcm[2 * (i - 1) + ch];
            if (d < 0) d = -d;
            if (i % EN_CTRL_BLOCK == 0) {
                if (d > on_boundary) on_boundary = d;
            } else {
                if (d > off_boundary) off_boundary = d;
            }
        }
        printf("  ch%d worst step on a control boundary %d, elsewhere %d\n",
               ch, on_boundary, off_boundary);
        CHECK(on_boundary <= off_boundary + 8,
              "ch%d: control-block boundaries are audible (%d vs %d)",
              ch, on_boundary, off_boundary);
    }

    free(pcm);
}

/* ---- 7. levels, fades and clipping -------------------------------------- */

static void test_levels(void)
{
    section("levels, fades, soft clip");

    /* Soft clip: unity through zero, bounded, monotonic. */
    CHECK(fabs(en_softclip(0.0)) < 1e-12, "softclip(0)");
    CHECK(fabs(en_softclip(0.001) - 0.001) < 1e-6, "softclip near zero");
    CHECK(en_softclip(100.0) <= 1.0, "softclip does not bound above");
    CHECK(en_softclip(-100.0) >= -1.0, "softclip does not bound below");
    double prev = -2.0;
    for (double x = -3.0; x <= 3.0; x += 0.01) {
        double y = en_softclip(x);
        CHECK(y >= prev - 1e-9, "softclip is not monotonic at %.3f", x);
        prev = y;
    }

    const uint32_t sr = 11025;
    const uint32_t frames = sr * 4;
    int16_t *pcm = malloc(frames * 2 * sizeof *pcm);

    en_render_t r;
    en_render_init(&r, sr, EN_NOISE_PINK, 7);

    en_segment_t seg;
    memset(&seg, 0, sizeof seg);
    seg.sample_rate = sr;
    seg.mode = EN_MODE_BINAURAL;
    seg.noise = EN_NOISE_PINK;
    seg.frames = frames;
    seg.start.carrier_hz = 200.0;
    seg.start.beat_hz = 10.0;
    seg.start.tone_level = 1.0;
    seg.start.noise_level = 0.3;
    seg.end = seg.start;
    seg.fade_in_s = 1.5;
    seg.fade_out_s = 1.5;
    en_render_segment(&r, &seg, pcm);

    /* Nothing may wrap. A wrapped sample is the loudest click a 16-bit file
       can make, so this assertion is not a formality. */
    CHECK(peak_abs(pcm, frames) <= 32767, "output wrapped");

    /* The fades must actually start and end at silence. */
    CHECK(abs(pcm[0]) < 64 && abs(pcm[1]) < 64,
          "fade-in does not start from silence (%d, %d)", pcm[0], pcm[1]);
    CHECK(abs(pcm[2 * (frames - 1)]) < 64,
          "fade-out does not reach silence (%d)", pcm[2 * (frames - 1)]);

    /* ...and the middle must be at a sensible level, not crushed. */
    int mid = 0;
    for (uint32_t i = frames / 2 - 1000; i < frames / 2 + 1000; i++) {
        int v = abs(pcm[2 * i]);
        if (v > mid) mid = v;
    }
    printf("  peak %d, mid-segment peak %d\n", peak_abs(pcm, frames), mid);
    CHECK(mid > 8000, "mid-segment level is only %d, too quiet", mid);

    free(pcm);
}

/* ---- 8. WAV header ------------------------------------------------------ */

static void test_wav_header(void)
{
    section("wav header");

    uint8_t h[EN_WAV_HEADER_BYTES];
    uint32_t total = en_wav_header(h, 11025, 2, 122500);

    CHECK(memcmp(h + 0, "RIFF", 4) == 0, "no RIFF tag");
    CHECK(memcmp(h + 8, "WAVE", 4) == 0, "no WAVE tag");
    CHECK(memcmp(h + 12, "fmt ", 4) == 0, "no fmt chunk");
    CHECK(memcmp(h + 36, "data", 4) == 0, "data chunk is not at offset 36");

    uint32_t data_bytes = (uint32_t)h[40] | ((uint32_t)h[41] << 8)
                        | ((uint32_t)h[42] << 16) | ((uint32_t)h[43] << 24);
    CHECK(data_bytes == 122500u * 4u, "data size %u wrong", data_bytes);
    CHECK(total == 44u + 122500u * 4u, "total %u wrong", total);
    CHECK(total == en_wav_size(122500, 2), "en_wav_size disagrees");

    uint32_t rate = (uint32_t)h[24] | ((uint32_t)h[25] << 8)
                  | ((uint32_t)h[26] << 16) | ((uint32_t)h[27] << 24);
    CHECK(rate == 11025u, "sample rate %u wrong", rate);
    CHECK(h[22] == 2 && h[23] == 0, "channel count wrong");
    CHECK(h[34] == 16 && h[35] == 0, "bit depth wrong");
}

/* ---- 9. programs and the user-program parser ---------------------------- */

static void test_programs(void)
{
    section("programs");

    int n;
    const en_program_t *ps = en_programs(&n);
    CHECK(n == 6, "expected 6 built-in programs, got %d", n);

    for (int i = 0; i < n; i++) {
        uint32_t secs = en_program_seconds(&ps[i]);
        printf("  %-20s %2u:%02u  %s  %s\n", ps[i].name, secs / 60, secs % 60,
               en_band_name(en_program_band(&ps[i])), ps[i].detail);
        CHECK(secs > 0, "%s has zero duration", ps[i].name);
        CHECK(ps[i].n_segs > 0, "%s has no segments", ps[i].name);

        /* The beat must be continuous across segment boundaries, or the audio
           would have to jump. */
        for (int s = 1; s < ps[i].n_segs; s++) {
            CHECK(fabs(ps[i].segs[s].beat_start - ps[i].segs[s - 1].beat_end)
                      < 1e-9,
                  "%s: beat jumps from %.3f to %.3f at segment %d",
                  ps[i].name, ps[i].segs[s - 1].beat_end,
                  ps[i].segs[s].beat_start, s);
        }

        /* Sampling the timeline must agree with the segment table. */
        CHECK(fabs(en_program_beat_at(&ps[i], 0.0) - ps[i].segs[0].beat_start)
                  < 1e-9, "%s: beat at t=0 wrong", ps[i].name);
        CHECK(en_program_seg_at(&ps[i], 0.0) == 0, "%s: seg at t=0", ps[i].name);
        CHECK(en_program_seg_at(&ps[i], (double)secs + 1.0) == -1,
              "%s: seg past the end should be -1", ps[i].name);
    }

    section("user program parser");

    const char *good =
        "# a user program\n"
        "name    Evening Slide\n"
        "mode    isochronic\n"
        "carrier 180\n"
        "noise   pink 0.2\n"
        "seg     10 6 900\n"
        "seg     6 2.5 1200\n";

    en_user_program_t up;
    int line = 0;
    int rc = en_parse_program(good, (uint32_t)strlen(good), &up, &line);
    CHECK(rc == EN_PARSE_OK, "parse failed: %s at line %d",
          en_parse_error_text(rc), line);
    CHECK(strcmp(up.name, "Evening Slide") == 0, "name is '%s'", up.name);
    CHECK(up.mode == EN_MODE_ISOCHRONIC, "mode wrong");
    CHECK(fabs(up.carrier_hz - 180.0) < 1e-9, "carrier %.3f", up.carrier_hz);
    CHECK(up.noise == EN_NOISE_PINK, "noise kind wrong");
    CHECK(fabs(up.noise_level - 0.2) < 1e-9, "noise level %.3f", up.noise_level);
    CHECK(up.n_segs == 2, "%d segments", up.n_segs);
    CHECK(fabs(up.segs[1].beat_end - 2.5) < 1e-9, "fractional beat not parsed");
    CHECK(up.segs[0].seconds == 900, "seconds wrong");

    /* A file with no name still loads, under a default. */
    const char *nameless = "seg 10 10 600\n";
    rc = en_parse_program(nameless, (uint32_t)strlen(nameless), &up, &line);
    CHECK(rc == EN_PARSE_OK, "nameless program should parse");
    CHECK(up.name[0] != 0, "nameless program got no default name");

    /* Bad input must be rejected with the offending line, not silently
       half-accepted. */
    struct { const char *text; int want; } bad[] = {
        { "name X\nwibble 3\nseg 10 10 60\n", EN_PARSE_BAD_KEY },
        { "mode sideways\nseg 10 10 60\n",    EN_PARSE_BAD_VALUE },
        { "carrier 5\nseg 10 10 60\n",        EN_PARSE_BAD_VALUE },
        { "seg 10 10\n",                      EN_PARSE_BAD_VALUE },
        { "name Empty\n",                     EN_PARSE_NO_SEGS },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        rc = en_parse_program(bad[i].text, (uint32_t)strlen(bad[i].text),
                              &up, &line);
        CHECK(rc == bad[i].want, "case %u: got %d (%s), wanted %d",
              i, rc, en_parse_error_text(rc), bad[i].want);
    }

    /* Too many segments must be refused rather than overrun the array. */
    char many[2048];
    int off = 0;
    off += sprintf(many + off, "name Many\n");
    for (int i = 0; i < EN_USER_MAX_SEGS + 4; i++)
        off += sprintf(many + off, "seg 10 10 60\n");
    rc = en_parse_program(many, (uint32_t)off, &up, &line);
    CHECK(rc == EN_PARSE_TOO_MANY_SEGS, "overlong program was accepted (%d)",
          rc);
}

/* ---- 10. bands ---------------------------------------------------------- */

static void test_bands(void)
{
    section("bands");
    CHECK(en_band_of(2.0)  == EN_BAND_DELTA, "2 Hz is not Delta");
    CHECK(en_band_of(6.0)  == EN_BAND_THETA, "6 Hz is not Theta");
    CHECK(en_band_of(7.83) == EN_BAND_THETA, "7.83 Hz is not Theta");
    CHECK(en_band_of(10.0) == EN_BAND_ALPHA, "10 Hz is not Alpha");
    CHECK(en_band_of(16.0) == EN_BAND_BETA,  "16 Hz is not Beta");
    CHECK(en_band_of(40.0) == EN_BAND_GAMMA, "40 Hz is not Gamma");
    /* Boundaries land in the upper band. */
    CHECK(en_band_of(4.0)  == EN_BAND_THETA, "4 Hz should be Theta");
    CHECK(en_band_of(8.0)  == EN_BAND_ALPHA, "8 Hz should be Alpha");
    CHECK(en_band_of(13.0) == EN_BAND_BETA,  "13 Hz should be Beta");
    CHECK(en_band_of(30.0) == EN_BAND_GAMMA, "30 Hz should be Gamma");

    for (int b = 0; b < EN_BAND_COUNT; b++) {
        CHECK(en_band_color((en_band_t)b) != 0, "band %d has no colour", b);
        CHECK(en_band_name((en_band_t)b)[0] != '?', "band %d has no name", b);
    }
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    printf("entrain core tests\n");

    test_sine();
    test_planner();
    test_beat_accuracy();
    test_clicks();
    test_loop_seams();
    test_noise_bed();
    test_overlap_add();
    test_glide();
    test_levels();
    test_wav_header();
    test_programs();
    test_bands();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

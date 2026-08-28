/*
 * audio_device.c — audio.h on the iPod under RetailOS.
 *
 * Generates audio a block at a time straight into RAM. No files, no loop, and
 * no crossfade.
 *
 * Four findings from RetailOS 1.1.2 shape this, all confirmed on device:
 *
 *   1. The OS sound player will play a buffer we allocate and fill ourselves.
 *      voice::setSource (VA 0x0862fd24) stores the DESCRIPTOR pointer at
 *      voice+0x78 rather than copying the audio, and its container-type table
 *      (desc+0x10) has a type 0 whose framesPerPacket = 1 and
 *      bytesPerFrame = (bits/8) * channels — linear PCM and nothing else.
 *      loadFile was only ever a parser, so we skip it: no files, no FAT
 *      writes, and no 1 MiB ceiling (which limited the FILE, not the buffer).
 *
 *   2. One voice playing one buffer sounds clean. Worth stating plainly,
 *      because everything below exists to preserve that.
 *
 *   3. Nothing can silence a voice that is already playing, and the pool MIXES
 *      rather than cutting — a second play() sums with the first.
 *
 *   4. play() is cheap once no file is involved, but WHEN the new voice starts
 *      is not ours to choose. We arm from a UI tick, so the start lands
 *      somewhere inside a frame period.
 *
 * (3) and (4) together are what wrecked the previous two versions. Both tried
 * to hide the join by overlapping consecutive buffers and crossfading. But
 * consecutive buffers are consecutive samples of ONE signal, so overlapping
 * them sums that signal with a time-shifted copy of itself — a comb filter,
 * whose notches sweep as the timing drifts. That is a wobble, and it is worse
 * than the click it was meant to hide. Lengthening the crossfade only slowed
 * the wobble down. No crossfade length fixes it, because the crossfade IS the
 * defect.
 *
 * So this joins the way a DDS does: it never resets a phase, and it never sums
 * two copies of anything.
 *
 *   - The renderer carries its phase accumulators across blocks, so block N+1
 *     opens on the sample after block N closes. Phase stays continuous through
 *     glides, retunes and program joins by construction, not by fading.
 *
 *   - Each block is cut where BOTH channels are crossing zero. The renderer
 *     produces a block plus a slack, en_zero_cut() picks the quietest boundary
 *     in that slack, and the block is committed at exactly that length. So when
 *     the next voice starts late, the gap is silence between two samples that
 *     were already near zero: no step at either end.
 *
 *   - The join is deliberately biased LATE, and armed to the millisecond
 *     rather than to the frame. Early is the one case a cut point cannot
 *     rescue — it overlaps a block's last milliseconds with the next block's
 *     first, and both are full-scale that far from the boundary, which the
 *     tests measure at 199% of peak. So the tick spins out the last few
 *     milliseconds to the deadline instead of arming whenever the next frame
 *     lands, which took the gap from a frame period down to two or three
 *     milliseconds — under a cycle of the carrier.
 *
 *   - Blocks are rendered a whole block ahead, right after the previous one is
 *     armed, so the work never lands on a boundary and there is no rendering
 *     stage for anyone to look at.
 *
 * Pause and stop do not tear a block off mid-flight. The voice reads our buffer
 * live, so they write a short fade to zero into the part of it that has not
 * been reached yet, then silence. While paused the backend keeps arming a short
 * silent block, which holds the DAC and the voice chain up so resuming is a
 * ramp rather than a cold start.
 *
 * Ownership: buffers come from hb_os_alloc and are never handed to loadFile, so
 * the OS never frees them. The descriptor is rebuilt before every play and its
 * constructor zeroes the buffer pointers without freeing, so there is no path
 * to a double free.
 */

#include "audio.h"
#include "sys.h"
#include "../core/render.h"

#include "hb_sdk.h"
#include "hb_heap.h"

#define SFX_CTOR_ADDR        (0x08417efcu | 1u)
#define SFX_PLAYER_INST_ADDR (0x08417eb8u | 1u)
#define SFX_PLAYER_PLAY_ADDR (0x0841828cu | 1u)

/* Descriptor layout, from SoundEffectDescriptor::ctor (VA 0x08417efc) and
   voice::setSource (VA 0x0862fd24). */
#define SFX_OFF_BUF_A    0x04
#define SFX_OFF_BUF_B    0x08
#define SFX_OFF_BUF_LEN  0x0C
#define SFX_OFF_TYPE     0x10   /* 0 = linear PCM */
#define SFX_OFF_RATE     0x14
#define SFX_OFF_CHANNELS 0x18
#define SFX_OFF_BITS     0x1C
#define SFX_OFF_VOLUME   0x24
#define SFX_OFF_TRIM_LO  0x38
#define SFX_OFF_TRIM_HI  0x3C
#define SFX_OFF_PLAYMODE 0x51
#define SFX_OFF_FLAGS    0x52
#define SFX_OFF_NEXTSFX  0x54

#define SFX_TYPE_LPCM 0

/* 22050 Hz: proven by harness T9, and half the render cost and half the memory
   of 44.1k for carriers that never go above a few hundred hertz. */
#define DEVICE_RATE 22050u

/* Block length. Also the app's worst-case response time to a retune or a
   preset change, so it does not want to grow without reason — and now the
   joins are silent there is nothing to buy by growing it. */
#define BLOCK_MS 2500

/* Search room for the zero crossing. A binaural pair only crosses zero in both
   channels together once per beat period, so this has to cover the slowest
   beat on offer: half a hertz is a two second period, and half of that reaches
   a crossing from anywhere in the cycle. */
#define SLACK_MS 1000

/* How far ahead of the estimated play cursor an in-place write has to stay.
   The cursor comes from the wall clock, so it is only good to a frame period or
   so; this is the margin for that plus anything the DAC has prefetched. */
#define SAFETY_MS 250

/* Fade to and from silence for pause, resume and stop. Long enough not to
   click, short enough to feel immediate. */
#define FADE_MS 80

/* What gets armed while paused, purely to keep the voice chain and the DAC
   alive so resuming is a ramp and not a cold start. */
#define QUIET_MS 250

/* Deliberately join late.
 *
 * Early is the one failure that no cut point can rescue: it overlaps a block's
 * last milliseconds with the next block's first, and both are full-scale that
 * far from the boundary, so they comb and they clip. The tests measure 200% of
 * peak. Late merely leaves a gap, and a gap between two samples the cut chooser
 * has already put on a zero crossing is a couple of milliseconds of silence
 * with no step at either end.
 *
 * So aim a shade past the end of the block and never before it. */
#define JOIN_BIAS_MS 2

/* How close to the deadline the tick will hold the frame and poll the clock
   rather than going away and coming back.
 *
 * This is the whole reason the join is measured in milliseconds. Arming from
 * the frame callback meant arming whenever the next frame happened to land -
 * 16 to 30 ms late, every block, which is an audible stutter. Spinning out the
 * last few milliseconds costs one dropped frame every couple of seconds and
 * buys two orders of magnitude of accuracy. Bounded by construction: the tick
 * only enters the spin once the deadline is within this. */
#define SPIN_MAX_MS 35

#define MS_FRAMES(ms) ((DEVICE_RATE * (uint32_t)(ms)) / 1000u)

#define BLOCK_FRAMES  MS_FRAMES(BLOCK_MS)
#define SLACK_FRAMES  MS_FRAMES(SLACK_MS)
#define CAP_FRAMES    (BLOCK_FRAMES + SLACK_FRAMES)
#define SAFETY_FRAMES MS_FRAMES(SAFETY_MS)
#define FADE_FRAMES   MS_FRAMES(FADE_MS)
#define QUIET_FRAMES  MS_FRAMES(QUIET_MS)

/* Nothing has run us for this long: the OS put another app in front. Whatever
   was sounding finished long ago and unheard, so start clean rather than
   replaying a backlog. */
#define STARVED_MS (BLOCK_MS * 3)

typedef void *(*sfx_ctor_t)(void *self);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *cb, void *cbdata);

typedef struct {
    uint8_t  desc[0x80];
    int16_t *pcm;
    uint32_t frames;        /* committed length, chosen by en_zero_cut */
    bool     ready;
} block_t;

#define BLK_A 0
#define BLK_B 1
#define BLK_Q 2             /* the silence armed while paused */

static block_t  g_blk[3];
static int      g_playing = -1;    /* index of the sounding block, or -1 */
static int      g_pending = -1;    /* rendered and waiting its turn */
static uint32_t g_started_ms;      /* when g_playing was armed */
static uint32_t g_playing_ms;      /* how long g_playing lasts */
static uint64_t g_frames_out;

static en_audio_pull_fn s_pull;
static void            *s_pull_ctx;
static bool             s_source_done;

static bool g_paused;
static bool g_stopping;
static bool g_fade_in;             /* next armed block ramps up from silence */
static int  g_volume = 45;
static en_audio_state_t g_state = EN_AUDIO_IDLE;

/* ---- helpers ------------------------------------------------------------- */

static uint32_t os_volume(void)
{
    int v = g_volume < 0 ? 0 : (g_volume > 100 ? 100 : g_volume);
    return (0x7fffu * (uint32_t)v) / 100u;
}

static bool reserve(int16_t **pcm, uint32_t bytes)
{
    if (*pcm) return true;
    /* Refuse rather than risk the OS allocator, which panics and reboots the
       device on failure. Starting a preset must never be able to do that. */
    if (hb_os_heap_largest() < bytes + (128u * 1024u)) return false;
    *pcm = (int16_t *)hb_os_alloc(bytes);
    return *pcm != (int16_t *)0;
}

static bool buffers_ready(void)
{
    if (!reserve(&g_blk[BLK_A].pcm, CAP_FRAMES * 4u)) return false;
    if (!reserve(&g_blk[BLK_B].pcm, CAP_FRAMES * 4u)) return false;
    if (!reserve(&g_blk[BLK_Q].pcm, QUIET_FRAMES * 4u)) return false;

    for (uint32_t i = 0; i < QUIET_FRAMES * 2u; i++) g_blk[BLK_Q].pcm[i] = 0;
    g_blk[BLK_Q].frames = QUIET_FRAMES;
    g_blk[BLK_Q].ready = true;
    return true;
}

/* Where the sounding block has most likely got to. Derived from the wall clock,
   so it is an estimate — every use of it keeps SAFETY_FRAMES clear. */
static uint32_t play_cursor(void)
{
    if (g_playing < 0) return 0;
    uint32_t elapsed = hb_time_uptime_ms() - g_started_ms;
    return MS_FRAMES(elapsed);
}

/* Render the next block and choose where to cut it.
 *
 * Two calls, one state. The first is a peek: it renders a block plus the slack
 * without moving the source on. The second asks for exactly the length the cut
 * chose and commits — and because the source had not moved, it re-renders the
 * identical samples that were measured. */
static bool prepare(int idx)
{
    block_t *b = &g_blk[idx];
    if (!s_pull || s_source_done) return false;

    uint32_t got = s_pull(b->pcm, CAP_FRAMES, 0, s_pull_ctx);
    if (got == 0) { s_source_done = true; return false; }

    uint32_t lo = (got > BLOCK_FRAMES) ? BLOCK_FRAMES : got;
    uint32_t n  = en_zero_cut(b->pcm, lo, got);
    if (n == 0 || n > got) n = got;

    if (s_pull(b->pcm, n, n, s_pull_ctx) == 0) {
        s_source_done = true;
        return false;
    }

    b->frames = n;
    b->ready = true;
    return true;
}

/* Ramp the head of a block up from silence, in place. Used on the first block
   of a run and on the first after a resume, so neither starts as a step. */
static void fade_in_head(block_t *b)
{
    uint32_t n = FADE_FRAMES < b->frames ? FADE_FRAMES : b->frames;
    for (uint32_t i = 0; i < n; i++) {
        int32_t g = (int32_t)((i * 4096u) / n);
        b->pcm[2 * i + 0] = (int16_t)((b->pcm[2 * i + 0] * g) >> 12);
        b->pcm[2 * i + 1] = (int16_t)((b->pcm[2 * i + 1] * g) >> 12);
    }
}

/* Fade the SOUNDING block down to zero and silence its remainder, in place and
 * ahead of the play cursor.
 *
 * This is what makes pause and stop immediate. The voice reads our buffer live,
 * so rewriting the part it has not reached yet takes effect within a fade
 * rather than at the end of the block — and it does that without starting a
 * second voice, which would sum rather than replace. */
static void fade_out_live(void)
{
    if (g_playing < 0) return;
    block_t *b = &g_blk[g_playing];
    if (!b->pcm) return;

    uint32_t from = play_cursor() + SAFETY_FRAMES;
    if (from >= b->frames) return;      /* it ends before a fade could finish */

    uint32_t n = FADE_FRAMES;
    if (from + n > b->frames) n = b->frames - from;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = from + i;
        int32_t g = 4096 - (int32_t)((i * 4096u) / n);
        b->pcm[2 * k + 0] = (int16_t)((b->pcm[2 * k + 0] * g) >> 12);
        b->pcm[2 * k + 1] = (int16_t)((b->pcm[2 * k + 1] * g) >> 12);
    }
    for (uint32_t k = from + n; k < b->frames; k++) {
        b->pcm[2 * k + 0] = 0;
        b->pcm[2 * k + 1] = 0;
    }
}

static bool arm(int idx)
{
    block_t *b = &g_blk[idx];
    if (!b->ready || !b->pcm || !b->frames) return false;

    if (g_fade_in && idx != BLK_Q) { fade_in_head(b); g_fade_in = false; }

    uint8_t *d = b->desc;
    ((sfx_ctor_t)SFX_CTOR_ADDR)(d);

    *(volatile uint32_t *)(d + SFX_OFF_BUF_A)    = (uint32_t)(uintptr_t)b->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_B)    = (uint32_t)(uintptr_t)b->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_LEN)  = b->frames * 4u;
    d[SFX_OFF_TYPE] = SFX_TYPE_LPCM;
    *(volatile uint32_t *)(d + SFX_OFF_RATE)     = DEVICE_RATE;
    *(volatile uint32_t *)(d + SFX_OFF_CHANNELS) = 2;
    *(volatile uint32_t *)(d + SFX_OFF_BITS)     = 16;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_LO)  = 0;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_HI)  = 0;
    *(volatile uint32_t *)(d + SFX_OFF_VOLUME)   = os_volume();
    d[SFX_OFF_PLAYMODE] = 1;
    d[SFX_OFF_FLAGS] = 0;
    *(volatile void **)(d + SFX_OFF_NEXTSFX) = (void *)0;

    void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();
    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, d, (void *)0, (void *)0);

    g_playing = idx;
    g_started_ms = hb_time_uptime_ms();
    g_playing_ms = (b->frames * 1000u) / DEVICE_RATE;

    if (idx != BLK_Q) {
        b->ready = false;
        g_frames_out += b->frames;
        g_state = EN_AUDIO_PLAYING;
    }
    return true;
}

/* ---- the interface ------------------------------------------------------- */

bool en_audio_init(void)
{
    g_state = EN_AUDIO_IDLE;
    return true;
}

void en_audio_shutdown(void)
{
    fade_out_live();
    g_stopping = true;
    s_pull = (en_audio_pull_fn)0;
    g_playing = -1;
    g_pending = -1;
    g_state = EN_AUDIO_IDLE;
    /* Buffers are deliberately left allocated: the app is going away, and
       freeing one while a voice may still be reading it would be worse than
       leaking for the moment until the heap goes too. */
}

bool en_audio_can_stream(void) { return true; }

uint32_t en_audio_preferred_rate(void) { return DEVICE_RATE; }

bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx)
{
    (void)sample_rate;             /* the descriptor is told DEVICE_RATE */
    if (!pull) return false;
    if (!buffers_ready()) { g_state = EN_AUDIO_FAILED; return false; }

    /* Anything already sounding is stale content. Fade it out where it stands
       rather than letting it run: the new source would otherwise sum with it,
       not replace it. */
    fade_out_live();

    s_pull = pull;
    s_pull_ctx = ctx;
    s_source_done = false;
    g_frames_out = 0;
    g_paused = false;
    g_stopping = false;
    g_fade_in = true;
    g_blk[BLK_A].ready = g_blk[BLK_B].ready = false;
    g_pending = -1;

    /* Never the block that is sounding. The voice reads our buffer live — that
       is what makes the in-place pause fade work — so rendering into it would
       rewrite audio out from under the DAC. */
    int first = (g_playing == BLK_A) ? BLK_B : BLK_A;
    if (!prepare(first)) { g_state = EN_AUDIO_FAILED; return false; }
    g_pending = first;

    /* If real audio is still sounding, its fade has to finish before new audio
       goes on top of it: the pool mixes, so the two would sum. The tick arms
       the pending block the moment the old one ends — one block at worst.
       Pause silence is not real audio and can simply be played over. */
    if (g_playing < 0 || g_playing == BLK_Q) {
        if (!arm(first)) { g_state = EN_AUDIO_FAILED; return false; }
        g_pending = -1;
    }

    g_state = EN_AUDIO_PLAYING;
    return true;
}

/* The file-shaped half of the interface is unreachable: the engine checks
   en_audio_can_stream() and takes the pull path. Defined so the backend
   satisfies audio.h in full. */
bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t advance_frames,
                     uint32_t sample_rate, bool loop)
{
    (void)key; (void)pcm; (void)frames; (void)advance_frames;
    (void)sample_rate; (void)loop;
    return false;
}

bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t advance_frames,
                    uint32_t sample_rate)
{
    (void)key; (void)pcm; (void)frames; (void)advance_frames;
    (void)sample_rate;
    return false;
}

bool en_audio_wants_next(void) { return false; }

double en_audio_remaining(void) { return s_pull && !s_source_done ? 1e9 : 0.0; }

void en_audio_stop(uint32_t fade_ms)
{
    (void)fade_ms;                 /* the fade length is fixed at FADE_MS */
    fade_out_live();
    g_stopping = true;
}

void en_audio_set_paused(bool paused)
{
    if (paused == g_paused) return;
    g_paused = paused;

    if (paused) {
        /* Immediate, and without stopping the voice: the sounding buffer is
           faded to zero in place, ahead of the cursor. The tick then keeps a
           silent block armed so the DAC never goes down. */
        fade_out_live();
        g_state = EN_AUDIO_PAUSED;
    } else {
        g_fade_in = true;          /* come back up, do not step */
        g_state = EN_AUDIO_PLAYING;
    }
}

void en_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
    /* Read at arm time, so it takes hold within one block. */
}

int en_audio_get_volume(void) { return g_volume; }

void en_audio_tick(void)
{
    if (!s_pull) return;

    uint32_t elapsed = hb_time_uptime_ms() - g_started_ms;

    /* Starved: the frame callback stopped being run, which is what happens when
       RetailOS brings its own Music app to the front. Drop the stale timing and
       start again rather than replaying a backlog nobody heard. */
    if (g_playing >= 0 && elapsed > STARVED_MS) {
        g_playing = -1;
        g_fade_in = true;
        elapsed = 0;
    }

    /* Keep the chain alive while paused. Nothing is rendered and the source
       does not move, so the program holds exactly where it was.
       Re-armed BEFORE the previous one runs out: waiting for it to end would
       leave a frame period of nothing between them, which is the very gap this
       is here to prevent. Silence over silence is still silence, so overlapping
       costs nothing. */
    if (g_paused) {
        if (g_playing < 0 || elapsed + (QUIET_MS / 4) >= g_playing_ms)
            arm(BLK_Q);
        return;
    }

    if (g_stopping) {
        if (g_playing < 0 || elapsed >= g_playing_ms) {
            g_playing = -1;
            s_pull = (en_audio_pull_fn)0;
            g_state = EN_AUDIO_IDLE;
        }
        return;
    }

    /* Render the next block as soon as there is a free one: a whole block of
       lead time, so the work never lands on a boundary. This is why there is no
       rendering stage to look at. */
    if (g_pending < 0 && !s_source_done) {
        int idx = (g_playing == BLK_A) ? BLK_B : BLK_A;
        if (prepare(idx)) g_pending = idx;
    }

    /* Hold until the sounding block is actually finished, to the millisecond.
       Anything else and the join is as ragged as the frame rate. */
    if (g_playing >= 0) {
        uint32_t due = g_started_ms + g_playing_ms + JOIN_BIAS_MS;
        int32_t  left = (int32_t)(due - hb_time_uptime_ms());
        if (left > SPIN_MAX_MS) return;         /* plenty of time; come back */

        /* Counted, not just conditioned on the clock. This runs inside the
           frame callback, so a clock that stopped advancing would take the
           whole device down with it rather than merely sounding wrong. The
           bound is generous — a spin only ever has SPIN_MAX_MS to cover — and
           overrunning it costs one ragged join, not a lock-up. */
        for (uint32_t guard = 0; left > 0 && guard < 4000000u; guard++)
            left = (int32_t)(due - hb_time_uptime_ms());
    }

    if (g_pending >= 0) {
        int idx = g_pending;
        g_pending = -1;
        arm(idx);
    } else if (s_source_done) {
        g_playing = -1;
        s_pull = (en_audio_pull_fn)0;
        g_state = EN_AUDIO_IDLE;        /* the program is over */
    } else {
        /* The render did not get done in time — a very busy frame. Fill and arm
           now; one late block is better than a dropout. */
        int idx = (g_playing == BLK_A) ? BLK_B : BLK_A;
        if (prepare(idx)) arm(idx);
    }
}

en_audio_state_t en_audio_state(void) { return g_state; }

double en_audio_elapsed(void)
{
    return (double)g_frames_out / (double)DEVICE_RATE;
}

const char *en_audio_backend_name(void) { return "RetailOS sfx, phase-locked PCM"; }

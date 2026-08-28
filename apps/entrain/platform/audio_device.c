/*
 * audio_device.c — audio.h on the iPod under RetailOS.
 *
 * Generates audio a window at a time, straight into RAM, and never touches the
 * filesystem.
 *
 * Three findings from RetailOS 1.1.2 shape this, all confirmed on device:
 *
 *   1. The OS sound player will play a buffer we allocate and fill ourselves.
 *      voice::setSource (VA 0x0862fd24) stores the DESCRIPTOR pointer at
 *      voice+0x78 rather than copying the audio, and its container-type table
 *      (desc+0x10) has a type 0 whose framesPerPacket = 1 and
 *      bytesPerFrame = (bits/8) * channels — linear PCM and nothing else.
 *      loadFile was only ever a parser, so we skip it. No files, no FAT
 *      writes, and no 1 MiB ceiling (which limited the FILE, not the buffer).
 *
 *   2. Nothing can silence a voice that is already playing, and the voice pool
 *      mixes rather than cutting — so a second play() sums with the first
 *      instead of replacing it. Handing over one long buffer meant every
 *      preset change and every live-tune re-render started a second voice on
 *      top of the first: twice as loud, drifting against itself.
 *
 *   3. play() itself is cheap once no file is involved.
 *
 * So this backend pulls. It keeps two half-second buffers, asks the engine to
 * fill the idle one while the other sounds, and plays them alternately. That
 * gives, in one design:
 *
 *   - no render stage the user can see. A window is a few thousand frames and
 *     renders in a couple of milliseconds, a whole window ahead of when it is
 *     needed, instead of a whole loop up front while a progress ring spun.
 *   - no loop and no seam. Successive windows are successive samples of a
 *     continuous signal, so there is no wrap to get right. The cycle-exact
 *     loop planner still runs, but only to report an honest realised beat.
 *   - everything the user does lands within one window: stop, pause, retune,
 *     a different preset. Never two sounds at once.
 *   - about 50 KB of buffers instead of the best part of a megabyte.
 *
 * Windows carry a short crossfade: each ends with the frames the next opens
 * with, faded down while that fades up, gains summing to one. Identical
 * content cannot comb-filter, so a late tick costs a ripple, not a click.
 *
 * Ownership: buffers come from hb_os_alloc and are never handed to loadFile,
 * so the OS never frees them. The descriptor is reconstructed before every
 * play and its constructor zeroes the buffer pointers without freeing, so
 * there is no path to a double free.
 */

#include "audio.h"
#include "sys.h"

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

/* 22050 Hz: proven by harness T9, and half the render cost and half the
   memory of 44.1k for carriers that never go above a few hundred hertz. */
#define DEVICE_RATE 22050u

/* Overlap-add.
 *
 * Every buffer is twice the advance and carries a triangular window: it fades
 * in across its first half and out across its second, so consecutive buffers
 * overlap by a full advance and every sample of the output is the sum of
 * exactly two ramps. Aligned, those sum to unity everywhere.
 *
 * That length is the whole point. A short crossfade only sums to unity when
 * the two are perfectly aligned; start the next one late by d and the sum sits
 * at 1 - d/overlap for the entire overlap. At 40 ms of overlap against a UI
 * tick of 16 to 30 ms that was a 40 to 75 percent dip twice a second - an
 * audible tremolo. At a one second overlap the same lateness costs about two
 * percent, which is a fifth of a decibel.
 *
 * The advance is also the app's worst-case response time to anything the user
 * does, so it should not grow without reason. */
#define ADVANCE_MS 1000

#define ADVANCE_FRAMES ((DEVICE_RATE * ADVANCE_MS) / 1000u)
#define BUFFER_FRAMES  (ADVANCE_FRAMES * 2u)

/* Longer than this since the last window started and we were not being run at
   all — the OS switched to another app. Restart cleanly rather than trying to
   catch up on audio nobody heard. */
#define STARVED_MS (ADVANCE_MS * 4)

typedef void *(*sfx_ctor_t)(void *self);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *cb, void *cbdata);

typedef struct {
    uint8_t  desc[0x80];
    int16_t *pcm;
    uint32_t frames;        /* window + crossfade tail */
    bool     filled;
} window_t;

static window_t g_win[2];
static int      g_playing = -1;     /* the sounding window, or -1 */
static int      g_filled_next = -1; /* the one already rendered and waiting */
static uint32_t g_started_ms;
static uint64_t g_frames_out;

static en_audio_pull_fn s_pull;
static void            *s_pull_ctx;
static bool             s_source_done;   /* the pull said it had no more */

static bool g_paused;
static bool g_stopping;
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
    /* Two buffers and nothing else: the engine renders straight into the one
       being prepared and the window is applied in place, so there is no third
       scratch copy. */
    uint32_t bytes = BUFFER_FRAMES * 4u;
    if (!reserve(&g_win[0].pcm, bytes)) return false;
    if (!reserve(&g_win[1].pcm, bytes)) return false;
    return true;
}

/* Ask the engine for the next stretch of audio, straight into the buffer, then
   apply the triangular window in place. */
static bool fill_window(int idx)
{
    window_t *w = &g_win[idx];
    if (!s_pull || s_source_done) return false;

    /* Two advances of audio, moving on by one. The second advance is the
       overlap that the next buffer also carries, so the source renders those
       frames twice - once as this buffer's falling half and once as the next
       one's rising half. */
    uint32_t got = s_pull(w->pcm, BUFFER_FRAMES, ADVANCE_FRAMES, s_pull_ctx);
    if (got == 0) { s_source_done = true; return false; }

    /* Apply the triangular window: up across the first half, down across the
       second. Its peak is at the advance point, which is exactly where the
       neighbouring buffer's ramp crosses it, so the pair sums to unity. */
    for (uint32_t i = 0; i < BUFFER_FRAMES; i++) {
        int32_t l = (i < got) ? w->pcm[2 * i + 0] : 0;
        int32_t r = (i < got) ? w->pcm[2 * i + 1] : 0;

        int32_t g = (i < ADVANCE_FRAMES)
                  ? (int32_t)((i * 4096u) / ADVANCE_FRAMES)
                  : 4096 - (int32_t)(((i - ADVANCE_FRAMES) * 4096u) / ADVANCE_FRAMES);

        w->pcm[2 * i + 0] = (int16_t)((l * g) >> 12);
        w->pcm[2 * i + 1] = (int16_t)((r * g) >> 12);
    }

    w->frames = BUFFER_FRAMES;
    w->filled = true;
    return true;
}

static bool play_window(int idx)
{
    window_t *w = &g_win[idx];
    if (!w->filled || !w->pcm) return false;

    uint8_t *d = w->desc;
    ((sfx_ctor_t)SFX_CTOR_ADDR)(d);

    *(volatile uint32_t *)(d + SFX_OFF_BUF_A)    = (uint32_t)(uintptr_t)w->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_B)    = (uint32_t)(uintptr_t)w->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_LEN)  = w->frames * 4u;
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

    w->filled = false;
    g_playing = idx;
    g_started_ms = hb_time_uptime_ms();
    g_frames_out += ADVANCE_FRAMES;
    g_state = EN_AUDIO_PLAYING;
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
    g_stopping = true;
    s_pull = (en_audio_pull_fn)0;
    g_playing = -1;
    g_filled_next = -1;
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

    s_pull = pull;
    s_pull_ctx = ctx;
    s_source_done = false;
    g_frames_out = 0;
    g_paused = false;
    g_stopping = false;
    g_win[0].filled = g_win[1].filled = false;
    g_filled_next = -1;

    /* If a window is still sounding, let it finish: starting another now would
       sum with it rather than replace it. The tick picks the new source up at
       the boundary, at most one window away. */
    if (g_playing >= 0) return true;

    if (!fill_window(0)) { g_state = EN_AUDIO_FAILED; return false; }
    if (!play_window(0)) { g_state = EN_AUDIO_FAILED; return false; }
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
    (void)fade_ms;
    /* Lands at the next window boundary: half a second at worst, rather than
       the whole loop length this used to take. */
    g_stopping = true;
}

void en_audio_set_paused(bool paused)
{
    g_paused = paused;
    g_state = paused ? EN_AUDIO_PAUSED : EN_AUDIO_PLAYING;
    /* Both directions land at the next boundary. Nothing is torn off
       mid-window, so neither is a click. */
}

void en_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
    /* Read at play() time, so it takes hold within one window. */
}

int en_audio_get_volume(void) { return g_volume; }

void en_audio_tick(void)
{
    if (!s_pull) return;

    uint32_t now = hb_time_uptime_ms();

    /* Nothing sounding: either the very start, a resume, or we have just been
       given the CPU back after the OS put another app in front of us. */
    if (g_playing < 0) {
        if (g_paused || g_stopping || s_source_done) return;
        if (g_filled_next >= 0) {
            int idx = g_filled_next;
            g_filled_next = -1;
            play_window(idx);
        } else if (fill_window(0)) {
            play_window(0);
        }
        return;
    }

    uint32_t elapsed = now - g_started_ms;

    /* Starved: the frame callback stopped being run, which is what happens
       when RetailOS brings its own Music app to the front. Whatever was
       sounding finished long ago and in silence. Drop the stale timing and
       start again from the next window rather than replaying a backlog. */
    if (elapsed > STARVED_MS) {
        g_playing = -1;
        g_filled_next = -1;
        g_win[0].filled = g_win[1].filled = false;
        return;                         /* next tick starts a fresh window */
    }

    /* Mid-window: render the next one now, so it is ready well before it is
       needed and the work never lands on a boundary. This is the whole reason
       there is no visible rendering stage any more. */
    if (g_filled_next < 0 && !g_stopping && !g_paused && !s_source_done) {
        int idx = (g_playing == 0) ? 1 : 0;
        if (fill_window(idx)) g_filled_next = idx;
    }

    if (elapsed < ADVANCE_MS) return;

    if (g_stopping) {
        g_playing = -1;
        s_pull = (en_audio_pull_fn)0;
        g_state = EN_AUDIO_IDLE;
        return;
    }
    if (g_paused) {
        g_playing = -1;                 /* resume picks up from here */
        return;
    }

    if (g_filled_next >= 0) {
        int idx = g_filled_next;
        g_filled_next = -1;
        play_window(idx);
    } else if (s_source_done) {
        g_playing = -1;
        s_pull = (en_audio_pull_fn)0;
        g_state = EN_AUDIO_IDLE;        /* the program is over */
    } else {
        /* The render did not get done in time — a very busy frame. Fill and
           play now; one late window is better than a dropout. */
        int idx = (g_playing == 0) ? 1 : 0;
        if (fill_window(idx)) play_window(idx);
    }
}

en_audio_state_t en_audio_state(void) { return g_state; }

double en_audio_elapsed(void)
{
    return (double)g_frames_out / (double)DEVICE_RATE;
}

const char *en_audio_backend_name(void) { return "RetailOS sfx, PCM windows"; }

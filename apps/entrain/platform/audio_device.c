/*
 * audio_device.c — audio.h on the iPod under RetailOS.
 *
 * Plays PCM straight from RAM, in short windows.
 *
 * Two findings shape this file, both from RetailOS 1.1.2 and both confirmed on
 * device:
 *
 *   1. The OS sound player will play a buffer we allocate and fill ourselves.
 *      voice::setSource (VA 0x0862fd24) stores the DESCRIPTOR pointer at
 *      voice+0x78 rather than copying the audio, and dispatches on a container
 *      type at desc+0x10 whose type 0 computes framesPerPacket = 1 and
 *      bytesPerFrame = (bits/8) * channels. That is linear PCM and nothing
 *      else. loadFile was only ever a parser, so we skip it: no files, no FAT
 *      writes, and no 1 MiB ceiling (which limited the FILE, not the buffer).
 *
 *   2. Nothing can silence a voice that is already playing. No stop entry
 *      point has been found, and the voice pool mixes rather than cutting, so
 *      a second play() does not replace the first — it sums with it.
 *
 * (2) is why this does not simply hand over one long buffer. Doing that made
 * every preset change, and every live-tune re-render, start a second voice on
 * top of the first: audibly twice as loud, and drifting against itself.
 *
 * So the rendered audio is kept as a master buffer and played out in half
 * second windows, alternating between two small slots. Anything the user does
 * takes effect at the next window instead of instantly, which is a fiftieth of
 * the wait that a full loop would have been, and — crucially — never overlaps
 * two different sounds.
 *
 * The windows are consecutive samples of the same signal, so the join carries
 * a short crossfade: each window ends with the frames the next one opens with,
 * faded down while that one fades up, gains summing to exactly one. Identical
 * content cannot comb-filter, so a late tick costs a slight ripple instead of
 * a click.
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
#define SFX_OFF_VOICE    0x48   /* setSource writes the voice pointer here */
#define SFX_OFF_PLAYMODE 0x51
#define SFX_OFF_FLAGS    0x52
#define SFX_OFF_NEXTSFX  0x54

#define SFX_TYPE_LPCM 0

/* Window length. Everything the user can do — stop, pause, retune, pick
   another preset — lands at the next window boundary, so this is the app's
   worst-case response time. Short enough to feel immediate, long enough that
   re-arming stays cheap at two play() calls a second. */
#define WINDOW_MS  500

/* Crossfade between consecutive windows. Only has to cover UI-tick jitter. */
#define XFADE_MS   40

typedef void *(*sfx_ctor_t)(void *self);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *cb, void *cbdata);

/* The rendered audio we are playing out of. */
typedef struct {
    int16_t *pcm;
    uint32_t capacity;      /* bytes allocated */
    uint32_t frames;
    uint32_t advance;       /* loop period; frames unless the caller says less */
    uint32_t rate;
    bool     loop;
    bool     ready;
} master_t;

/* One of the two small buffers actually handed to the OS. */
typedef struct {
    uint8_t  desc[0x80];
    int16_t *pcm;
    uint32_t capacity;
    uint32_t frames;        /* window + crossfade tail */
    uint32_t advance;       /* window only */
} window_t;

static master_t g_cur, g_next;
static bool     g_next_ready;

static window_t g_win[2];
static int      g_playing = -1;
static uint32_t g_started_ms;
static uint32_t g_read_pos;        /* frame cursor into the current master */
static uint64_t g_frames_out;      /* for the elapsed readout */

static bool g_paused;
static bool g_stopping;
static int  g_volume = 45;
static en_audio_state_t g_state = EN_AUDIO_IDLE;

/* ---- helpers ------------------------------------------------------------- */

static uint32_t win_frames(uint32_t rate) { return (rate * WINDOW_MS) / 1000u; }
static uint32_t xfade_frames(uint32_t rate) { return (rate * XFADE_MS) / 1000u; }

static uint32_t os_volume(void)
{
    int v = g_volume < 0 ? 0 : (g_volume > 100 ? 100 : g_volume);
    return (0x7fffu * (uint32_t)v) / 100u;
}

static bool reserve(int16_t **pcm, uint32_t *cap, uint32_t bytes)
{
    if (*pcm && *cap >= bytes) return true;

    /* Refuse rather than risk the OS allocator, which panics and reboots the
       device on failure. Picking a preset must never be able to do that. */
    if (hb_os_heap_largest() < bytes + (128u * 1024u)) return false;

    if (*pcm) { hb_os_free(*pcm); *pcm = (int16_t *)0; *cap = 0; }
    *pcm = (int16_t *)hb_os_alloc(bytes);
    if (!*pcm) return false;
    *cap = bytes;
    return true;
}

static bool master_set(master_t *m, const int16_t *pcm, uint32_t frames,
                       uint32_t advance, uint32_t rate, bool loop)
{
    if (!reserve(&m->pcm, &m->capacity, frames * 4u)) return false;

    uint32_t words = frames * 2u;
    for (uint32_t i = 0; i < words; i++) m->pcm[i] = pcm[i];

    m->frames = frames;
    m->advance = (advance && advance <= frames) ? advance : frames;
    m->rate = rate;
    m->loop = loop;
    m->ready = true;
    return true;
}

/* Copy one window out of the master, wrapping if it loops, and apply the
   crossfade shaping: fade the head in and the tail out, so consecutive windows
   sum to exactly the original signal across their overlap. */
static void window_fill(window_t *w, const master_t *m, uint32_t pos)
{
    const uint32_t win = win_frames(m->rate);
    const uint32_t x   = xfade_frames(m->rate);
    const uint32_t len = win + x;

    for (uint32_t i = 0; i < len; i++) {
        uint32_t src = pos + i;
        if (m->loop) {
            src %= m->advance;                 /* the loop period, not the buffer */
        } else if (src >= m->frames) {
            w->pcm[2 * i + 0] = 0;
            w->pcm[2 * i + 1] = 0;
            continue;
        }
        int32_t l = m->pcm[2 * src + 0];
        int32_t r = m->pcm[2 * src + 1];

        /* Linear gains that sum to one with the neighbouring window. Correct
           here because the overlap is the SAME signal in both, not two
           independent ones. */
        if (i < x) {                           /* head: fade in */
            int32_t g = (int32_t)((i * 4096u) / x);
            l = (l * g) >> 12;
            r = (r * g) >> 12;
        } else if (i >= win) {                 /* tail: fade out */
            int32_t g = 4096 - (int32_t)(((i - win) * 4096u) / x);
            l = (l * g) >> 12;
            r = (r * g) >> 12;
        }
        w->pcm[2 * i + 0] = (int16_t)l;
        w->pcm[2 * i + 1] = (int16_t)r;
    }

    w->frames = len;
    w->advance = win;
}

static void window_arm(window_t *w, uint32_t rate)
{
    uint8_t *d = w->desc;

    ((sfx_ctor_t)SFX_CTOR_ADDR)(d);

    *(volatile uint32_t *)(d + SFX_OFF_BUF_A)    = (uint32_t)(uintptr_t)w->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_B)    = (uint32_t)(uintptr_t)w->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_LEN)  = w->frames * 4u;
    d[SFX_OFF_TYPE] = SFX_TYPE_LPCM;
    *(volatile uint32_t *)(d + SFX_OFF_RATE)     = rate;
    *(volatile uint32_t *)(d + SFX_OFF_CHANNELS) = 2;
    *(volatile uint32_t *)(d + SFX_OFF_BITS)     = 16;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_LO)  = 0;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_HI)  = 0;
    *(volatile uint32_t *)(d + SFX_OFF_VOLUME)   = os_volume();
    d[SFX_OFF_PLAYMODE] = 1;
    d[SFX_OFF_FLAGS] = 0;
    *(volatile void **)(d + SFX_OFF_NEXTSFX) = (void *)0;
}

/* Fill the idle window from the master at g_read_pos and play it. */
static bool play_next_window(void)
{
    if (!g_cur.ready || !g_cur.rate) return false;

    int idx = (g_playing == 0) ? 1 : 0;
    window_t *w = &g_win[idx];

    uint32_t len = win_frames(g_cur.rate) + xfade_frames(g_cur.rate);
    if (!reserve(&w->pcm, &w->capacity, len * 4u)) return false;

    window_fill(w, &g_cur, g_read_pos);
    window_arm(w, g_cur.rate);

    void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();
    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, w->desc,
                                              (void *)0, (void *)0);

    g_playing = idx;
    g_started_ms = hb_time_uptime_ms();
    g_state = EN_AUDIO_PLAYING;
    return true;
}

/* Move the read cursor on by one window, following the master's own rules. */
static bool advance_cursor(void)
{
    uint32_t win = win_frames(g_cur.rate);
    g_read_pos += win;
    g_frames_out += win;

    if (g_cur.loop) {
        if (g_read_pos >= g_cur.advance) g_read_pos -= g_cur.advance;
        return true;
    }
    if (g_read_pos < g_cur.frames) return true;

    /* This master is spent. Take the queued one if there is one. */
    if (g_next_ready) {
        master_t tmp = g_cur;
        g_cur = g_next;
        g_next = tmp;
        g_next.ready = false;
        g_next_ready = false;
        g_read_pos = 0;
        return true;
    }
    return false;
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
    g_playing = -1;
    g_cur.ready = g_next.ready = false;
    g_next_ready = false;
    g_state = EN_AUDIO_IDLE;
    /* Buffers are deliberately left allocated: the app is going away, and
       freeing one while a voice may still be reading it would be worse than
       leaking for the few hundred milliseconds until the heap goes too. */
}

bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t advance_frames,
                     uint32_t sample_rate, bool loop)
{
    (void)key;                        /* nothing on disk left to key */
    if (!pcm || !frames) return false;

    g_state = EN_AUDIO_PREPARING;

    if (!master_set(&g_cur, pcm, frames, advance_frames, sample_rate, loop)) {
        g_state = EN_AUDIO_FAILED;
        return false;
    }

    g_next.ready = false;
    g_next_ready = false;
    g_read_pos = 0;
    g_frames_out = 0;
    g_paused = false;
    g_stopping = false;

    /* If a window is already sounding, let it finish: starting another now
       would sum with it rather than replace it. The tick picks the new master
       up at the boundary, at most WINDOW_MS away. */
    if (g_playing >= 0) return true;

    if (!play_next_window()) {
        g_state = EN_AUDIO_FAILED;
        return false;
    }
    return true;
}

bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t advance_frames,
                    uint32_t sample_rate)
{
    (void)key;
    if (!pcm || !frames) return false;
    if (!master_set(&g_next, pcm, frames, advance_frames, sample_rate, false))
        return false;
    g_next_ready = true;
    return true;
}

bool en_audio_can_stream(void) { return false; }

bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx)
{
    (void)sample_rate; (void)pull; (void)ctx;
    return false;
}

bool en_audio_wants_next(void)
{
    return g_cur.ready && !g_cur.loop && !g_next_ready && !g_stopping;
}

double en_audio_remaining(void)
{
    if (!g_cur.ready) return 0.0;
    if (g_cur.loop) return 1e9;
    uint32_t left = (g_read_pos < g_cur.frames) ? g_cur.frames - g_read_pos : 0;
    return (double)left / (double)g_cur.rate;
}

void en_audio_stop(uint32_t fade_ms)
{
    (void)fade_ms;
    /* Takes effect at the next window boundary — half a second at worst,
       rather than the eleven to eighteen seconds a whole loop would have
       been. */
    g_stopping = true;
    g_next_ready = false;
}

void en_audio_set_paused(bool paused)
{
    g_paused = paused;
    g_state = paused ? EN_AUDIO_PAUSED : EN_AUDIO_PLAYING;
    /* Both directions land at the next boundary; nothing is torn off
       mid-window, so neither is a click. */
}

void en_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
    /* The descriptor's volume is read at play() time, so a change takes hold
       within one window rather than instantly. */
}

int en_audio_get_volume(void) { return g_volume; }

void en_audio_tick(void)
{
    if (!g_cur.ready) return;

    /* Nothing playing and not stopped: this is the start, or a resume. */
    if (g_playing < 0) {
        if (!g_paused && !g_stopping) play_next_window();
        return;
    }

    uint32_t dur = (win_frames(g_cur.rate) * 1000u) / g_cur.rate;
    if (hb_time_uptime_ms() - g_started_ms < dur) return;

    if (g_stopping) {
        g_playing = -1;
        g_cur.ready = false;
        g_state = EN_AUDIO_IDLE;
        return;
    }
    if (g_paused) {
        g_playing = -1;              /* hold position; resume refills here */
        return;
    }

    if (!advance_cursor()) {
        g_playing = -1;
        g_cur.ready = false;
        g_state = EN_AUDIO_IDLE;     /* ran dry: the program is over */
        return;
    }
    play_next_window();
}

en_audio_state_t en_audio_state(void) { return g_state; }

double en_audio_elapsed(void)
{
    if (!g_cur.rate) return 0.0;
    return (double)g_frames_out / (double)g_cur.rate;
}

const char *en_audio_backend_name(void) { return "RetailOS sfx, PCM windows"; }

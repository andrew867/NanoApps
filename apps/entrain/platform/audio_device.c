/*
 * audio_device.c — audio.h on the iPod under RetailOS, playing PCM from RAM.
 *
 * This backend used to render WAV files, write them under /Apps/Data and hand
 * the paths to SoundEffectDescriptor::loadFile. That is gone. Confirmed on
 * device by harnesses/audio_spike T9: the OS sound player will play a buffer
 * we allocate and fill ourselves, with no file anywhere.
 *
 * What makes it work, read out of RetailOS 1.1.2 and then verified by ear:
 *
 *   voice::setSource (VA 0x0862fd24) stores the DESCRIPTOR pointer at
 *   voice+0x78 — it does not copy the audio — and dispatches on a container
 *   type at desc+0x10. Type 0 computes framesPerPacket = 1 and
 *   bytesPerFrame = (bits/8) * channels, then frames = desc[0x0C] /
 *   bytesPerFrame. That is linear PCM and nothing else.
 *
 * So loadFile was only ever a parser: it decoded a file into a heap buffer and
 * filled in exactly the fields this file now fills in directly. Doing it
 * ourselves removes, in one go:
 *
 *   - the 1 MiB ceiling, which was a limit on the FILE, not on the buffer;
 *   - every FAT write, with its wear and its latency;
 *   - the whole cache-key class of bug, where two chunks collided on one path;
 *   - re-reading and re-decoding the same loop file every time it wrapped.
 *
 * A join is now one play() call on a descriptor already pointing at ready
 * samples — cheap enough to land exactly on the boundary instead of being
 * started early and overlapped, which is what made loop wraps audible.
 *
 * Ownership: these buffers come from hb_os_alloc and are never handed to
 * loadFile, so the OS never frees them. The descriptor is reconstructed before
 * every play, and its constructor zeroes the buffer pointers without freeing,
 * so there is no path to a double free.
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
#define SFX_OFF_BUF_A    0x04   /* decoded PCM buffer */
#define SFX_OFF_BUF_B    0x08   /* the same pointer; loadFile sets both */
#define SFX_OFF_BUF_LEN  0x0C   /* length in bytes */
#define SFX_OFF_TYPE     0x10   /* container type; 0 = linear PCM */
#define SFX_OFF_RATE     0x14
#define SFX_OFF_CHANNELS 0x18
#define SFX_OFF_BITS     0x1C
#define SFX_OFF_VOLUME   0x24
#define SFX_OFF_TRIM_LO  0x38   /* leading trim, subtracted from the frames */
#define SFX_OFF_TRIM_HI  0x3C   /* trailing trim */
#define SFX_OFF_PLAYMODE 0x51
#define SFX_OFF_FLAGS    0x52
#define SFX_OFF_NEXTSFX  0x54

#define SFX_TYPE_LPCM 0

typedef void *(*sfx_ctor_t)(void *self);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *cb, void *cbdata);

/* Two slots, matching audio.h: what plays now and what plays next. Each owns
   its buffer and descriptor, so preparing one never disturbs the other. */
typedef struct {
    uint8_t  desc[0x80];
    int16_t *pcm;
    uint32_t capacity;      /* bytes allocated */
    uint32_t frames;
    uint32_t rate;
    bool     loop;
    bool     ready;
} slot_t;

static slot_t   g_slot[2];
static int      g_playing = -1;    /* index of the sounding slot, or -1 */
static int      g_pending = -1;    /* index queued to follow it */
static uint32_t g_started_ms;
static uint32_t g_elapsed_ms;
static bool     g_paused;
static bool     g_stopping;
static int      g_volume = 45;
static en_audio_state_t g_state = EN_AUDIO_IDLE;

/* ---- helpers ------------------------------------------------------------- */

static uint32_t slot_ms(const slot_t *s)
{
    if (!s->ready || !s->rate) return 0;
    return (uint32_t)(((uint64_t)s->frames * 1000ull) / s->rate);
}

static uint32_t os_volume(void)
{
    int v = g_volume < 0 ? 0 : (g_volume > 100 ? 100 : g_volume);
    return (0x7fffu * (uint32_t)v) / 100u;
}

/* Grow a slot's buffer if it cannot hold `bytes`. Buffers persist between
   submits: a preset replays one size forever and a program's chunks are all
   the same length, so after the first allocation this never allocates again. */
static bool slot_reserve(slot_t *s, uint32_t bytes)
{
    if (s->pcm && s->capacity >= bytes) return true;

    /* Refuse rather than let the OS allocator panic — it reboots the device on
       failure, and picking a preset should never be able to do that. */
    if (hb_os_heap_largest() < bytes + (128u * 1024u)) return false;

    /* No NULL here: this translation unit is freestanding and does not pull in
       stddef.h. */
    if (s->pcm) { hb_os_free(s->pcm); s->pcm = (int16_t *)0; s->capacity = 0; }
    s->pcm = (int16_t *)hb_os_alloc(bytes);
    if (!s->pcm) return false;
    s->capacity = bytes;
    return true;
}

static void slot_fill(slot_t *s, const int16_t *pcm, uint32_t frames,
                      uint32_t rate, bool loop)
{
    uint32_t words = frames * 2u;
    for (uint32_t i = 0; i < words; i++) s->pcm[i] = pcm[i];

    s->frames = frames;
    s->rate = rate;
    s->loop = loop;
    s->ready = true;
}

/* Point a freshly constructed descriptor at the slot's buffer: everything
   loadFile used to fill in, filled in directly. */
static void slot_arm_descriptor(slot_t *s)
{
    uint8_t *d = s->desc;

    ((sfx_ctor_t)SFX_CTOR_ADDR)(d);

    *(volatile uint32_t *)(d + SFX_OFF_BUF_A)    = (uint32_t)(uintptr_t)s->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_B)    = (uint32_t)(uintptr_t)s->pcm;
    *(volatile uint32_t *)(d + SFX_OFF_BUF_LEN)  = s->frames * 4u;
    d[SFX_OFF_TYPE] = SFX_TYPE_LPCM;
    *(volatile uint32_t *)(d + SFX_OFF_RATE)     = s->rate;
    *(volatile uint32_t *)(d + SFX_OFF_CHANNELS) = 2;
    *(volatile uint32_t *)(d + SFX_OFF_BITS)     = 16;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_LO)  = 0;
    *(volatile uint32_t *)(d + SFX_OFF_TRIM_HI)  = 0;

    *(volatile uint32_t *)(d + SFX_OFF_VOLUME)   = os_volume();
    d[SFX_OFF_PLAYMODE] = 1;
    d[SFX_OFF_FLAGS] = 0;
    *(volatile void **)(d + SFX_OFF_NEXTSFX) = (void *)0;
}

/* Play a slot. No file I/O and no decode, so this is cheap enough to sit on a
   boundary rather than being started early. */
static bool slot_play(int idx)
{
    slot_t *s = &g_slot[idx];
    if (!s->ready || !s->pcm || !s->frames) return false;

    slot_arm_descriptor(s);

    void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();
    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, s->desc,
                                              (void *)0, (void *)0);

    g_playing = idx;
    g_started_ms = hb_time_uptime_ms();
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
    g_playing = -1;
    g_pending = -1;
    g_slot[0].ready = g_slot[1].ready = false;
    g_state = EN_AUDIO_IDLE;
    /* The buffers are deliberately left allocated. The app is going away and
       the OS heap goes with it, and freeing one while a voice may still be
       reading it would be worse than leaking for a few milliseconds. */
}

bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t sample_rate, bool loop)
{
    (void)key;                    /* nothing on disk left to key */
    if (!pcm || !frames) return false;

    g_state = EN_AUDIO_PREPARING;

    if (!slot_reserve(&g_slot[0], frames * 4u)) {
        g_state = EN_AUDIO_FAILED;
        return false;
    }
    slot_fill(&g_slot[0], pcm, frames, sample_rate, loop);

    g_slot[1].ready = false;
    g_pending = -1;
    g_paused = false;
    g_stopping = false;
    g_elapsed_ms = 0;

    if (!slot_play(0)) {
        g_state = EN_AUDIO_FAILED;
        return false;
    }
    return true;
}

bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t sample_rate)
{
    (void)key;
    if (!pcm || !frames) return false;

    int idx = (g_playing == 0) ? 1 : 0;
    if (!slot_reserve(&g_slot[idx], frames * 4u)) return false;
    slot_fill(&g_slot[idx], pcm, frames, sample_rate, false);
    g_pending = idx;
    return true;
}

/* The pull interface is the Linux port's; here the engine still hands over
   finished buffers. */
bool en_audio_can_stream(void) { return false; }

bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx)
{
    (void)sample_rate; (void)pull; (void)ctx;
    return false;
}

bool en_audio_wants_next(void)
{
    return g_playing >= 0 && !g_slot[g_playing].loop
        && g_pending < 0 && !g_stopping;
}

double en_audio_remaining(void)
{
    if (g_playing < 0) return 0.0;
    if (g_slot[g_playing].loop) return 1e9;
    uint32_t dur = slot_ms(&g_slot[g_playing]);
    uint32_t played = hb_time_uptime_ms() - g_started_ms;
    return played >= dur ? 0.0 : (double)(dur - played) / 1000.0;
}

void en_audio_stop(uint32_t fade_ms)
{
    (void)fade_ms;
    /* Still no confirmed way to silence a buffer already in flight, so a stop
       lands at the next boundary. Buffers are far shorter than the old loop
       files, so the wait is correspondingly shorter. */
    g_stopping = true;
    g_pending = -1;
    if (g_playing >= 0) g_slot[g_playing].loop = false;
}

void en_audio_set_paused(bool paused)
{
    g_paused = paused;
    g_state = paused ? EN_AUDIO_PAUSED : EN_AUDIO_PLAYING;
    if (!paused && g_playing >= 0) slot_play(g_playing);
}

void en_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
    /* Read when play() is called, so this takes effect at the next boundary. */
}

int en_audio_get_volume(void) { return g_volume; }

void en_audio_tick(void)
{
    if (g_playing < 0 || g_paused) return;

    uint32_t dur = slot_ms(&g_slot[g_playing]);
    if (!dur) return;

    uint32_t played = hb_time_uptime_ms() - g_started_ms;
    if (played < dur) return;          /* exactly on the boundary, never early */

    g_elapsed_ms += dur;

    if (g_stopping) {
        g_playing = -1;
        g_state = EN_AUDIO_IDLE;
        return;
    }

    if (g_slot[g_playing].loop) {
        slot_play(g_playing);          /* the same samples again */
    } else if (g_pending >= 0) {
        int next = g_pending;
        g_pending = -1;
        g_slot[g_playing].ready = false;
        slot_play(next);
    } else {
        g_playing = -1;
        g_state = EN_AUDIO_IDLE;       /* ran dry: the program is over */
    }
}

en_audio_state_t en_audio_state(void) { return g_state; }

double en_audio_elapsed(void)
{
    if (g_playing < 0) return (double)g_elapsed_ms / 1000.0;
    uint32_t played = hb_time_uptime_ms() - g_started_ms;
    return (double)(g_elapsed_ms + played) / 1000.0;
}

const char *en_audio_backend_name(void) { return "RetailOS sfx, PCM from RAM"; }

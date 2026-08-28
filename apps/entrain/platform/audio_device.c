/*
 * audio_device.c — audio.h on the iPod, over the OS sound-effect player.
 *
 * The device cannot be streamed to. It can only be handed a file, and only a
 * file under 1 MiB (AUDIO_NOTES.md). So this backend:
 *
 *   1. writes the submitted PCM to a WAV under /Apps/Data/Entrain/cache,
 *      keyed by the realised parameters, and skips the write if that key is
 *      already on disk — which is what makes a second play instant;
 *   2. loads it and plays it;
 *   3. re-arms on a timer, because the duration is known exactly (we rendered
 *      it) and no completion signal is confirmed yet.
 *
 * That last point is the fallback the notes committed to — "rung 3" — chosen
 * deliberately so v1 does not depend on the play() callback turning out to
 * work. If harness T3b confirms the callback fires, EN_DEVICE_USE_CALLBACK
 * turns the timer into a signal and the join gets tighter. Nothing else
 * changes.
 *
 * Known limitation, stated plainly: there is no confirmed way to stop a
 * buffer that is already playing. en_audio_stop therefore stops re-arming and
 * lets the current buffer finish, so a stop can take up to one loop length
 * (11-18 s) to fall silent. Finding a stop primitive is the top follow-up in
 * AUDIO_NOTES.md.
 */

#include "audio.h"
#include "sys.h"

#include "hb_sdk.h"
#include "hb_heap.h"

#include "../core/wavout.h"

/* The firmware entry points. The SDK wrappers hardcode the offset/size and
   callback arguments to zero; this backend wants them, so it calls through
   directly, exactly as harnesses/audio_spike does. */
#define SFX_CTOR_ADDR        (0x08417efcu | 1u)
#define SFX_LOADFILE_ADDR    (0x08417f78u | 1u)
#define SFX_PLAYER_INST_ADDR (0x08417eb8u | 1u)
#define SFX_PLAYER_PLAY_ADDR (0x0841828cu | 1u)

#define SFX_OFF_VOLUME   0x24
#define SFX_OFF_PLAYMODE 0x51
#define SFX_OFF_FLAGS    0x52
#define SFX_OFF_NEXTSFX  0x54

#define VOL_MAIN 0

typedef void *(*sfx_ctor_t)(void *self);
typedef int   (*sfx_loadfile_t)(void *self, const char *path, int volume,
                                uint32_t offset, uint32_t size);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *cb, void *cbdata);

/* Load the next buffer this long before the current one ends. Loading is the
   expensive step — it reads and decodes the whole file — so doing it ahead of
   the boundary leaves only the cheap play() call to happen on it.

   Note this is a PRELOAD lead, not a playback lead. An earlier version started
   the next copy 120 ms early on the theory that overlapping the same waveform
   is harmless. It is not: restarting a loop early puts the new copy at phase
   zero while the old one is still 120 ms from its end, so the two are at
   unrelated phases and the sum comb-filters — which is exactly the "stops and
   fades in suddenly, not phase consistent" artefact heard on the device. */
#define PRELOAD_LEAD_MS 900

/* Refuse to load if the OS heap could not take the loader's copy. The loader
   allocates 1.2x the file plus 8 KB through an allocator that panics rather
   than failing, so this check is what stands between a large preset and a
   reboot. */
#define HEAP_SAFETY_BYTES (256u * 1024u)

typedef struct {
    char     path[128];
    uint32_t frames;
    uint32_t rate;
    bool     loop;
    bool     valid;
} clip_t;

static uint8_t  g_desc_a[0x80];
static uint8_t  g_desc_b[0x80];
static uint8_t *g_desc_playing = g_desc_a;

static clip_t   g_cur, g_next;
static bool     g_preloaded;       /* the spare descriptor holds the next clip */
static uint32_t g_started_ms;
static uint32_t g_elapsed_ms;      /* accumulated across re-arms */
static bool     g_paused;
static bool     g_stopping;
static int      g_volume = 45;
static en_audio_state_t g_state = EN_AUDIO_IDLE;

/* ---- small helpers ------------------------------------------------------- */

static void str_cpy(char *d, const char *s, int cap)
{
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void str_cat(char *d, const char *s, int cap)
{
    int n = 0;
    while (d[n] && n < cap - 1) n++;
    while (*s && n < cap - 1) d[n++] = *s++;
    d[n] = 0;
}

static uint32_t clip_ms(const clip_t *c)
{
    if (!c->valid || !c->rate) return 0;
    return (uint32_t)(((uint64_t)c->frames * 1000ull) / c->rate);
}

/* OS volume is 0..0x7fff. */
static uint32_t os_volume(void)
{
    uint32_t v = (uint32_t)g_volume;
    if (v > 100) v = 100;
    return (0x7fffu * v) / 100u;
}

/* ---- writing the cache file ---------------------------------------------- */

static bool write_clip(const char *path, const int16_t *pcm, uint32_t frames,
                       uint32_t rate)
{
    if (hb_fs_size(path) == en_wav_size(frames, 2))
        return true;    /* already cached, byte for byte */

    uint8_t hdr[EN_WAV_HEADER_BYTES];
    en_wav_header(hdr, rate, 2, frames);

    if (!hb_fs_stream_open(path)) return false;
    if (!hb_fs_stream_write(hdr, EN_WAV_HEADER_BYTES)) {
        hb_fs_stream_close();
        return false;
    }
    /* In chunks, so a large preset does not need a second copy of itself in
       RAM while it is written. */
    const uint32_t CHUNK = 32768;
    uint32_t left = frames * 4u;
    const uint8_t *p = (const uint8_t *)pcm;
    while (left) {
        uint32_t n = left < CHUNK ? left : CHUNK;
        if (!hb_fs_stream_write(p, n)) {
            hb_fs_stream_close();
            return false;
        }
        p += n;
        left -= n;
    }
    return hb_fs_stream_close();
}

/* ---- the 4-step play ceremony -------------------------------------------- */

/* sdk/hb_audio.c warns that the OS audio subsystem panics if these calls run
   back to back without display activity between them. An LVGL surface app is
   compositing continuously, which supplies that traffic — apps/files/files.c
   relies on the same thing — so no explicit interleaving is needed here. This
   is the one assumption in this file that wants confirming on hardware before
   the screen-blank feature ships. */
static bool load_clip(uint8_t *desc, const clip_t *c)
{
    if (!c->valid) return false;

    uint32_t bytes = en_wav_size(c->frames, 2);
    if (hb_os_heap_largest() < bytes + bytes / 5u + HEAP_SAFETY_BYTES)
        return false;

    ((sfx_ctor_t)SFX_CTOR_ADDR)(desc);
    if (((sfx_loadfile_t)SFX_LOADFILE_ADDR)(desc, c->path, VOL_MAIN, 0, 0) != 0)
        return false;

    *(volatile uint32_t *)(desc + SFX_OFF_VOLUME) = os_volume();
    desc[SFX_OFF_PLAYMODE] = 1;
    desc[SFX_OFF_FLAGS] = 0;
    *(volatile void **)(desc + SFX_OFF_NEXTSFX) = (void *)0;
    return true;
}

static bool play_desc(uint8_t *desc)
{
    void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();
    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, desc,
                                              (void *)0, (void *)0);
    return true;
}

static uint8_t *spare_desc(void)
{
    return (g_desc_playing == g_desc_a) ? g_desc_b : g_desc_a;
}

/* Load into the descriptor that is NOT playing. Safe to call well before the
   boundary; that is the whole point. */
static bool preload(const clip_t *c)
{
    if (g_preloaded) return true;
    if (!load_clip(spare_desc(), c)) return false;
    g_preloaded = true;
    return true;
}

/* Play whatever is already loaded in the spare descriptor and make it current.
   Nothing here touches the filesystem, so it is cheap enough to sit exactly on
   the boundary. */
static bool play_preloaded(void)
{
    uint8_t *spare = spare_desc();
    if (!g_preloaded) return false;
    if (!play_desc(spare)) return false;
    g_desc_playing = spare;
    g_preloaded = false;
    g_started_ms = hb_time_uptime_ms();
    g_state = EN_AUDIO_PLAYING;
    return true;
}

static bool arm_and_play(const clip_t *c)
{
    g_preloaded = false;
    if (!preload(c)) return false;
    return play_preloaded();
}

/* ---- the interface ------------------------------------------------------- */

bool en_audio_init(void)
{
    hb_fs_mkdir(en_sys_cache_dir());
    g_state = EN_AUDIO_IDLE;
    return true;
}

void en_audio_shutdown(void)
{
    g_stopping = true;
    g_cur.valid = false;
    g_next.valid = false;
    g_state = EN_AUDIO_IDLE;
}

static void clip_path(char *out, int cap, const char *key)
{
    str_cpy(out, en_sys_cache_dir(), cap);
    str_cat(out, "/", cap);
    str_cat(out, key && key[0] ? key : "tmp", cap);
    str_cat(out, ".wav", cap);
}

bool en_audio_submit(const char *key, const int16_t *pcm,
                     uint32_t frames, uint32_t sample_rate, bool loop)
{
    if (!pcm || !frames) return false;

    clip_t c;
    clip_path(c.path, (int)sizeof c.path, key);
    c.frames = frames;
    c.rate = sample_rate;
    c.loop = loop;
    c.valid = true;

    g_state = EN_AUDIO_PREPARING;
    if (!write_clip(c.path, pcm, frames, sample_rate)) {
        g_state = EN_AUDIO_FAILED;
        return false;
    }

    g_cur = c;
    g_next.valid = false;
    g_preloaded = false;
    g_paused = false;
    g_stopping = false;
    g_elapsed_ms = 0;

    if (!arm_and_play(&g_cur)) {
        g_state = EN_AUDIO_FAILED;
        return false;
    }
    return true;
}

bool en_audio_queue(const char *key, const int16_t *pcm,
                    uint32_t frames, uint32_t sample_rate)
{
    if (!pcm || !frames) return false;

    clip_t c;
    clip_path(c.path, (int)sizeof c.path, key);
    c.frames = frames;
    c.rate = sample_rate;
    c.loop = false;
    c.valid = true;

    if (!write_clip(c.path, pcm, frames, sample_rate)) return false;
    g_next = c;
    return true;
}

/* This backend plays buffers, not a continuous feed, so the engine takes the
   submit/queue path. */
bool en_audio_can_stream(void) { return false; }

bool en_audio_start_stream(uint32_t sample_rate, en_audio_pull_fn pull,
                           void *ctx)
{
    (void)sample_rate; (void)pull; (void)ctx;
    return false;
}

bool en_audio_wants_next(void)
{
    return g_cur.valid && !g_cur.loop && !g_next.valid && !g_stopping;
}

double en_audio_remaining(void)
{
    if (!g_cur.valid) return 0.0;
    if (g_cur.loop) return 1e9;
    uint32_t dur = clip_ms(&g_cur);
    uint32_t played = hb_time_uptime_ms() - g_started_ms;
    return played >= dur ? 0.0 : (double)(dur - played) / 1000.0;
}

void en_audio_stop(uint32_t fade_ms)
{
    (void)fade_ms;
    /* No stop primitive is known, so this stops re-arming and lets the
       current buffer run out. See the file header. */
    g_stopping = true;
    g_next.valid = false;
    g_cur.loop = false;
}

void en_audio_set_paused(bool paused)
{
    /* Same limitation: pausing cannot silence what is already playing, so it
       takes effect at the end of the current buffer. Resuming restarts from
       the loop's beginning, which for a steady tone is indistinguishable. */
    g_paused = paused;
    g_state = paused ? EN_AUDIO_PAUSED : EN_AUDIO_PLAYING;
    if (!paused && g_cur.valid) arm_and_play(&g_cur);
}

void en_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
    /* The descriptor's volume is read when play() is called, so a change takes
       effect at the next re-arm rather than immediately. */
}

int en_audio_get_volume(void) { return g_volume; }

void en_audio_tick(void)
{
    if (!g_cur.valid || g_paused) return;

    uint32_t dur = clip_ms(&g_cur);
    if (!dur) return;

    uint32_t played = hb_time_uptime_ms() - g_started_ms;

    /* Well before the boundary: get the next buffer decoded and sitting in the
       spare descriptor, so the boundary itself costs only a play() call. */
    if (!g_stopping && !g_preloaded && played + PRELOAD_LEAD_MS >= dur) {
        if (g_cur.loop)            preload(&g_cur);
        else if (g_next.valid)     preload(&g_next);
    }

    if (played < dur) return;        /* not yet: no early start, no overlap */

    g_elapsed_ms += dur;

    if (g_stopping) {
        g_cur.valid = false;
        g_state = EN_AUDIO_IDLE;
        return;
    }

    if (g_cur.loop) {
        if (!play_preloaded()) arm_and_play(&g_cur);
    } else if (g_next.valid) {
        g_cur = g_next;
        g_next.valid = false;
        if (!play_preloaded()) arm_and_play(&g_cur);
    } else {
        g_cur.valid = false;
        g_state = EN_AUDIO_IDLE;       /* ran dry: the program is over */
    }
}

en_audio_state_t en_audio_state(void) { return g_state; }

double en_audio_elapsed(void)
{
    if (!g_cur.valid) return (double)g_elapsed_ms / 1000.0;
    uint32_t played = hb_time_uptime_ms() - g_started_ms;
    return (double)(g_elapsed_ms + played) / 1000.0;
}

const char *en_audio_backend_name(void) { return "hb_audio (OS sfx player)"; }

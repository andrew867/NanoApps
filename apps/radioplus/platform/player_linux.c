/*
 * player_linux.c — player.h through tinyalsa on hw:0,0.
 *
 * hw:0,0 is IIS0, driving the CS42L81 and the headphone jack. hw:0,1 is IIS2,
 * where the tuner puts its audio. They are separate devices, which is what lets
 * this read one and write the other without either waiting on the other.
 *
 * The thread does one thing per period: get a period of audio from wherever the
 * source currently is, and write it out. Everything else - live, behind live,
 * playing a recording, paused - is a variation on where "wherever" points.
 *
 * Live playback deliberately sits a little behind the newest captured frame.
 * Reading right at the write head means every jitter in either thread is an
 * underrun, and the cost of the delay is a fraction of a second nobody can
 * perceive on a radio.
 */

#include "player.h"
#include "capture.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#include "../core/wav.h"

#define PLAY_CARD   0
#define PLAY_DEVICE 0        /* IIS0, the headphones */

#define RATE     44100u
#define CHANNELS 2u
#define BITS     16u
#define FRAME    (CHANNELS * (BITS / 8u))

#define PERIOD_FRAMES 1024u
#define PERIOD_COUNT  4u

/* How far behind the write head live playback sits. Two periods is about 46 ms:
   enough that ordinary scheduling jitter never reaches the write head, and far
   too little to notice on a radio. */
#define LIVE_LAG_FRAMES (PERIOD_FRAMES * 2u)

static struct pcm *s_pcm;
static pthread_t   s_thread;
static bool        s_running;
static char        s_desc[96];

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static en_play_src_t s_src;
static bool     s_paused;
static uint64_t s_at;            /* live: absolute frame in the capture ring */
static uint32_t s_underruns;

static FILE    *s_file;
static uint32_t s_file_frames;   /* total, from the header */
static uint32_t s_file_pos;      /* frames played */
static char     s_file_name[96];

static uint32_t frames_to_ms(uint64_t f)
{
    return (uint32_t)(f * 1000u / RATE);
}

static uint32_t ms_to_frames(uint32_t ms)
{
    return (uint32_t)((uint64_t)ms * RATE / 1000u);
}

/* ---- the thread ----------------------------------------------------------- */

static void *writer(void *arg)
{
    (void)arg;
    const uint32_t chunk = PERIOD_FRAMES * FRAME;
    uint8_t *buf = malloc(chunk);
    if (!buf) return 0;

    while (s_running) {
        uint32_t got = 0;

        pthread_mutex_lock(&s_lock);
        bool paused = s_paused;

        if (!paused && s_src == EN_SRC_FILE && s_file) {
            size_t n = fread(buf, 1, chunk, s_file);
            got = (uint32_t)(n / FRAME);
            s_file_pos += got;

            /* A recording that has run out stops rather than looping, and hands
               the headphones back to the radio - which is what anyone expects
               when a clip ends. */
            if (got < PERIOD_FRAMES) {
                fclose(s_file);
                s_file = 0;
                s_src = EN_SRC_LIVE;
                s_at = 0;                 /* re-anchor below */
            }
        }
        pthread_mutex_unlock(&s_lock);

        if (!paused && !got) {
            /* Live. Anchor on first use, and re-anchor whenever the cursor has
               fallen outside what the ring still holds - which happens after a
               stall, and is better than playing a gap nobody asked for. */
            uint64_t total = en_cap_total_frames();
            uint64_t oldest = en_cap_oldest_frame();

            pthread_mutex_lock(&s_lock);
            if (s_src != EN_SRC_LIVE) s_src = EN_SRC_LIVE;
            if (!s_at || s_at < oldest || s_at > total)
                s_at = (total > LIVE_LAG_FRAMES) ? total - LIVE_LAG_FRAMES : 0;
            uint64_t at = s_at;
            pthread_mutex_unlock(&s_lock);

            got = at ? en_cap_read_from(at, buf, PERIOD_FRAMES) : 0;

            pthread_mutex_lock(&s_lock);
            s_at = at + got;
            pthread_mutex_unlock(&s_lock);
        }

        /* Silence rather than nothing. The PCM has to keep being fed or it
           underruns and the next real audio starts with a click. */
        if (got < PERIOD_FRAMES) {
            memset(buf + got * FRAME, 0, (PERIOD_FRAMES - got) * FRAME);
            if (!paused && got == 0) {
                pthread_mutex_lock(&s_lock);
                s_underruns++;
                pthread_mutex_unlock(&s_lock);
            }
        }

        if (pcm_writei(s_pcm, buf, PERIOD_FRAMES) != (int)PERIOD_FRAMES) {
            pthread_mutex_lock(&s_lock);
            s_underruns++;
            pthread_mutex_unlock(&s_lock);
        }
    }
    free(buf);
    return 0;
}

/* ---- lifecycle ------------------------------------------------------------ */

en_play_err_t en_play_start(void)
{
    if (s_running) return EN_PLAY_OK;

    struct pcm_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.channels = CHANNELS;
    cfg.rate = RATE;
    cfg.period_size = PERIOD_FRAMES;
    cfg.period_count = PERIOD_COUNT;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.start_threshold = PERIOD_FRAMES;
    cfg.stop_threshold = PERIOD_FRAMES * PERIOD_COUNT;

    s_pcm = pcm_open(PLAY_CARD, PLAY_DEVICE, PCM_OUT, &cfg);
    if (!s_pcm || !pcm_is_ready(s_pcm)) {
        snprintf(s_desc, sizeof s_desc, "hw:%u,%u unavailable: %s",
                 PLAY_CARD, PLAY_DEVICE,
                 s_pcm ? pcm_get_error(s_pcm) : "no device");
        if (s_pcm) { pcm_close(s_pcm); s_pcm = 0; }
        return EN_PLAY_NO_DEVICE;
    }

    s_src = EN_SRC_LIVE;
    s_at = 0;
    s_paused = false;
    s_underruns = 0;

    s_running = true;
    if (pthread_create(&s_thread, 0, writer, 0) != 0) {
        s_running = false;
        pcm_close(s_pcm);
        s_pcm = 0;
        return EN_PLAY_FAILED;
    }

    snprintf(s_desc, sizeof s_desc, "tinyalsa hw:%u,%u  %u Hz %u ch",
             PLAY_CARD, PLAY_DEVICE, RATE, CHANNELS);
    return EN_PLAY_OK;
}

void en_play_stop(void)
{
    if (!s_running) return;
    s_running = false;
    pthread_join(s_thread, 0);

    pthread_mutex_lock(&s_lock);
    if (s_file) { fclose(s_file); s_file = 0; }
    pthread_mutex_unlock(&s_lock);

    if (s_pcm) { pcm_close(s_pcm); s_pcm = 0; }
}

void en_play_state(en_play_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    uint64_t total = en_cap_total_frames();
    uint64_t oldest = en_cap_oldest_frame();

    pthread_mutex_lock(&s_lock);
    out->running = s_running;
    out->paused = s_paused;
    out->source = s_src;
    out->underruns = s_underruns;

    if (s_src == EN_SRC_LIVE) {
        /* The lag is deliberate and constant, so it is subtracted out - showing
           "0:00 behind" while sitting two periods back is the honest reading of
           what the listener experiences. */
        uint64_t at = s_at ? s_at : (total > LIVE_LAG_FRAMES
                                     ? total - LIVE_LAG_FRAMES : 0);
        uint64_t behind = (total > at) ? total - at : 0;
        behind = (behind > LIVE_LAG_FRAMES) ? behind - LIVE_LAG_FRAMES : 0;
        out->behind_ms = frames_to_ms(behind);
        out->behind_max_ms = frames_to_ms(total > oldest ? total - oldest : 0);
    } else {
        out->pos_ms = frames_to_ms(s_file_pos);
        out->len_ms = frames_to_ms(s_file_frames);
        snprintf(out->name, sizeof out->name, "%s", s_file_name);
    }
    pthread_mutex_unlock(&s_lock);
}

const char *en_play_backend(void)
{
    return s_desc[0] ? s_desc : "tinyalsa (not started)";
}

/* ---- live ----------------------------------------------------------------- */

void en_play_seek_live(uint32_t behind_ms)
{
    uint64_t total = en_cap_total_frames();
    uint64_t oldest = en_cap_oldest_frame();

    uint64_t back = ms_to_frames(behind_ms) + LIVE_LAG_FRAMES;
    uint64_t at = (total > back) ? total - back : oldest;
    if (at < oldest) at = oldest;

    pthread_mutex_lock(&s_lock);
    /* Seeking the radio while a recording is playing means the user wants the
       radio, so the file is closed rather than left half-played. */
    if (s_file) { fclose(s_file); s_file = 0; }
    s_src = EN_SRC_LIVE;
    s_at = at;
    s_paused = false;
    pthread_mutex_unlock(&s_lock);
}

void en_play_go_live(void) { en_play_seek_live(0); }

void en_play_nudge(int32_t ms)
{
    en_play_state_t st;
    en_play_state(&st);

    if (st.source == EN_SRC_FILE) {
        int64_t p = (int64_t)st.pos_ms + ms;
        if (p < 0) p = 0;
        if ((uint32_t)p > st.len_ms) p = st.len_ms;
        en_play_seek_file((uint32_t)p);
        return;
    }

    /* Negative ms means going further back, which is a larger "behind". */
    int64_t behind = (int64_t)st.behind_ms - ms;
    if (behind < 0) behind = 0;
    if (behind > (int64_t)st.behind_max_ms) behind = st.behind_max_ms;
    en_play_seek_live((uint32_t)behind);
}

/* ---- recordings ----------------------------------------------------------- */

en_play_err_t en_play_file(const char *path)
{
    if (!path) return EN_PLAY_NO_FILE;

    FILE *f = fopen(path, "rb");
    if (!f) return EN_PLAY_NO_FILE;

    uint8_t hdr[EN_WAV_HDR_BYTES];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f);
        return EN_PLAY_NO_FILE;
    }

    /* Only what this app writes, and said so rather than played as noise: a
       header that is not ours means a sample rate or channel count the output
       is not configured for, and playing it anyway is worse than declining. */
    uint32_t bytes = en_wav_data_len(hdr, sizeof hdr);
    if (!bytes) {
        /* A recording interrupted before its length was patched. The audio is
           still good; the length is simply whatever is there. */
        long here = ftell(f);
        if (fseek(f, 0, SEEK_END) == 0) {
            long end = ftell(f);
            if (end > here) bytes = (uint32_t)(end - here);
            fseek(f, here, SEEK_SET);
        }
    }
    if (!bytes) { fclose(f); return EN_PLAY_NO_FILE; }

    pthread_mutex_lock(&s_lock);
    if (s_file) fclose(s_file);
    s_file = f;
    s_file_frames = bytes / FRAME;
    s_file_pos = 0;
    s_src = EN_SRC_FILE;
    s_paused = false;

    const char *base = path, *p = path;
    for (; *p; p++) if (*p == '/') base = p + 1;
    snprintf(s_file_name, sizeof s_file_name, "%s", base);
    pthread_mutex_unlock(&s_lock);

    return EN_PLAY_OK;
}

void en_play_close_file(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_file) { fclose(s_file); s_file = 0; }
    s_src = EN_SRC_LIVE;
    s_at = 0;                 /* re-anchors to live on the next period */
    s_paused = false;
    pthread_mutex_unlock(&s_lock);
}

void en_play_pause(bool paused)
{
    pthread_mutex_lock(&s_lock);
    s_paused = paused;

    /* Pausing the radio is really "start being behind", so resuming picks up
       where it stopped rather than jumping to live. The cursor is left alone
       and the ring keeps filling underneath it. */
    pthread_mutex_unlock(&s_lock);
}

void en_play_seek_file(uint32_t ms)
{
    pthread_mutex_lock(&s_lock);
    if (s_file) {
        uint32_t frame = ms_to_frames(ms);
        if (frame > s_file_frames) frame = s_file_frames;
        if (fseek(s_file, (long)(EN_WAV_HDR_BYTES + (size_t)frame * FRAME),
                  SEEK_SET) == 0)
            s_file_pos = frame;
    }
    pthread_mutex_unlock(&s_lock);
}

const char *en_play_strerror(en_play_err_t e)
{
    switch (e) {
    case EN_PLAY_OK:        return "ok";
    case EN_PLAY_NO_DEVICE: return "no playback device";
    case EN_PLAY_NO_FILE:   return "cannot play that file";
    default:                return "failed";
    }
}

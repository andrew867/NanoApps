/*
 * capture_linux.c — capture.h on the N31 Linux port, through tinyalsa.
 *
 * hw:0,1 is IIS2, which is where the BCM2078 puts tuner audio; hw:0,0 is IIS0
 * driving the headphone codec. They are separate devices, so capture does not
 * contend with playback and a recording can run while listening.
 *
 * One detail from the n31-fm helper is load-bearing, and easy to half-remember.
 * The SoC side of IIS2 is clocked by the capture PCM, so tuner audio is not
 * merely unrecorded but inaudible until something opens the capture device.
 * That is necessary and not sufficient: the audio then has to be carried from
 * IIS2 to IIS0, which n31-fm does with arecord piped into aplay and this app
 * does in the player. Capture starts with the tuner rather than with the record
 * button because of the first half; the radio is silent without the second.
 *
 * The reader runs on its own thread, feeding a ring in memory and, when one is
 * running, a WAV file. Neither consumer can stall the other: a file write that
 * blocks must not cost live audio, so a failed write stops the recording and
 * leaves the ring alone.
 */

#include "capture.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#include "../core/wav.h"

#define CAP_CARD   0
#define CAP_DEVICE 1        /* IIS2, the tuner's digital audio */

#define CAP_RATE     44100u
#define CAP_CHANNELS 2u
#define CAP_BITS     16u
#define CAP_FRAME    (CAP_CHANNELS * (CAP_BITS / 8u))

#define PERIOD_FRAMES 1024u
#define PERIOD_COUNT  4u

static struct pcm *s_pcm;
static pthread_t   s_thread;
static bool        s_running;
static char        s_desc[96];

/* The ring, and everything the reader thread shares with the callers. */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t  *s_ring;
static uint32_t  s_ring_bytes;
static uint32_t  s_ring_used;      /* bytes currently held, up to s_ring_bytes */
static uint32_t  s_ring_head;      /* where the next byte is written */
static uint32_t  s_overruns;
static uint64_t  s_total;     /* frames captured since the stream began */

static FILE     *s_rec;
static uint32_t  s_rec_bytes;

static uint32_t bytes_to_ms(uint32_t b)
{
    return (uint32_t)((uint64_t)b * 1000u / (CAP_RATE * CAP_FRAME));
}

static uint32_t ms_to_bytes(uint32_t ms)
{
    uint64_t b = (uint64_t)ms * CAP_RATE * CAP_FRAME / 1000u;
    return (uint32_t)(b - (b % CAP_FRAME));      /* always whole frames */
}

/* ---- the ring ------------------------------------------------------------ */

static void ring_write(const uint8_t *p, uint32_t n)
{
    if (!s_ring || !s_ring_bytes) return;

    /* A block larger than the ring can only leave its own tail. */
    if (n >= s_ring_bytes) {
        p += n - s_ring_bytes;
        n = s_ring_bytes;
    }

    uint32_t first = s_ring_bytes - s_ring_head;
    if (first > n) first = n;
    memcpy(s_ring + s_ring_head, p, first);
    if (n > first) memcpy(s_ring, p + first, n - first);

    s_ring_head = (s_ring_head + n) % s_ring_bytes;
    s_ring_used += n;
    if (s_ring_used > s_ring_bytes) s_ring_used = s_ring_bytes;
    s_total += n / CAP_FRAME;
}

/* Copy the most recent `want` bytes out, oldest first. Returns how many. */
static uint32_t ring_tail(uint8_t *out, uint32_t want)
{
    if (!s_ring || !s_ring_used) return 0;
    if (want > s_ring_used) want = s_ring_used;

    uint32_t start = (s_ring_head + s_ring_bytes - want) % s_ring_bytes;
    uint32_t first = s_ring_bytes - start;
    if (first > want) first = want;

    memcpy(out, s_ring + start, first);
    if (want > first) memcpy(out + first, s_ring, want - first);
    return want;
}

/* ---- the reader ---------------------------------------------------------- */

static void *reader(void *arg)
{
    (void)arg;
    const uint32_t chunk = PERIOD_FRAMES * CAP_FRAME;
    uint8_t *buf = malloc(chunk);
    if (!buf) return 0;

    while (s_running) {
        /* pcm_readi counts frames rather than bytes and is what tinyalsa
           wants now; pcm_read is deprecated. It returns frames read, or a
           negative error. */
        int r = pcm_readi(s_pcm, buf, PERIOD_FRAMES);
        if (r != (int)PERIOD_FRAMES) {
            /* A read error is usually an overrun, which means the thread lost
               the race and audio is genuinely gone. Counting them matters: a
               recording with a non-zero count has holes in it and the user
               should be told rather than left to notice. */
            pthread_mutex_lock(&s_lock);
            s_overruns++;
            pthread_mutex_unlock(&s_lock);
            usleep(1000);
            continue;
        }

        pthread_mutex_lock(&s_lock);
        ring_write(buf, chunk);

        if (s_rec) {
            if (fwrite(buf, 1, chunk, s_rec) != chunk) {
                /* Out of space, or the card went away. Close the recording so
                   what was captured stays playable, and leave the live buffer
                   running - losing the radio because a write failed would be
                   the wrong trade. */
                fclose(s_rec);
                s_rec = 0;
            } else {
                s_rec_bytes += chunk;
            }
        }
        pthread_mutex_unlock(&s_lock);
    }
    free(buf);
    return 0;
}

/* ---- the interface ------------------------------------------------------- */

en_cap_err_t en_cap_start(uint32_t live_seconds)
{
    if (s_running) return EN_CAP_OK;
    if (!live_seconds) live_seconds = 30;

    struct pcm_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.channels = CAP_CHANNELS;
    cfg.rate = CAP_RATE;
    cfg.period_size = PERIOD_FRAMES;
    cfg.period_count = PERIOD_COUNT;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.start_threshold = 0;
    cfg.stop_threshold = 0;
    cfg.silence_threshold = 0;

    s_pcm = pcm_open(CAP_CARD, CAP_DEVICE, PCM_IN, &cfg);
    if (!s_pcm || !pcm_is_ready(s_pcm)) {
        snprintf(s_desc, sizeof s_desc, "hw:%u,%u unavailable: %s",
                 CAP_CARD, CAP_DEVICE,
                 s_pcm ? pcm_get_error(s_pcm) : "no device");
        if (s_pcm) { pcm_close(s_pcm); s_pcm = 0; }
        return EN_CAP_NO_DEVICE;
    }

    /* Allocated up front. Failing here, before the tuner is playing, is much
       better than failing later with the radio already on. */
    s_ring_bytes = live_seconds * CAP_RATE * CAP_FRAME;
    s_ring = malloc(s_ring_bytes);
    if (!s_ring) {
        pcm_close(s_pcm);
        s_pcm = 0;
        s_ring_bytes = 0;
        return EN_CAP_NO_MEMORY;
    }
    s_ring_used = s_ring_head = s_overruns = 0;
    s_total = 0;

    s_running = true;
    if (pthread_create(&s_thread, 0, reader, 0) != 0) {
        s_running = false;
        free(s_ring); s_ring = 0; s_ring_bytes = 0;
        pcm_close(s_pcm); s_pcm = 0;
        return EN_CAP_FAILED;
    }

    snprintf(s_desc, sizeof s_desc, "tinyalsa hw:%u,%u  %u Hz %u ch  %us buffer",
             CAP_CARD, CAP_DEVICE, CAP_RATE, CAP_CHANNELS, live_seconds);
    return EN_CAP_OK;
}

void en_cap_stop(void)
{
    if (!s_running) return;
    s_running = false;
    pthread_join(s_thread, 0);

    en_cap_record_stop();

    pthread_mutex_lock(&s_lock);
    free(s_ring);
    s_ring = 0;
    s_ring_bytes = s_ring_used = s_ring_head = 0;
    pthread_mutex_unlock(&s_lock);

    if (s_pcm) { pcm_close(s_pcm); s_pcm = 0; }
}

void en_cap_state(en_cap_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    out->rate = CAP_RATE;
    out->channels = CAP_CHANNELS;
    out->bits = CAP_BITS;
    out->running = s_running;

    pthread_mutex_lock(&s_lock);
    out->live_ms = bytes_to_ms(s_ring_used);
    out->live_cap_ms = bytes_to_ms(s_ring_bytes);
    out->recorded_ms = bytes_to_ms(s_rec_bytes);
    out->recording = s_rec != 0;
    out->overruns = s_overruns;
    pthread_mutex_unlock(&s_lock);
}

uint64_t en_cap_total_frames(void)
{
    pthread_mutex_lock(&s_lock);
    uint64_t t = s_total;
    pthread_mutex_unlock(&s_lock);
    return t;
}

uint64_t en_cap_oldest_frame(void)
{
    pthread_mutex_lock(&s_lock);
    uint64_t oldest = s_total - (s_ring_used / CAP_FRAME);
    pthread_mutex_unlock(&s_lock);
    return oldest;
}

uint32_t en_cap_read_from(uint64_t at, void *buf, uint32_t frames)
{
    if (!buf || !frames) return 0;

    pthread_mutex_lock(&s_lock);

    uint32_t held = s_ring_used / CAP_FRAME;
    uint64_t oldest = s_total - held;

    /* A reader that has fallen behind the window gets the oldest audio still
       held rather than silence or a fault: the alternative is a gap the reader
       cannot see and cannot fix. */
    if (at < oldest) at = oldest;
    if (at >= s_total) { pthread_mutex_unlock(&s_lock); return 0; }

    uint64_t avail = s_total - at;
    if (frames > avail) frames = (uint32_t)avail;

    uint32_t back = (uint32_t)((s_total - at) * CAP_FRAME);
    uint32_t start = (s_ring_head + s_ring_bytes - back) % s_ring_bytes;
    uint32_t want = frames * CAP_FRAME;

    uint32_t first = s_ring_bytes - start;
    if (first > want) first = want;
    memcpy(buf, s_ring + start, first);
    if (want > first) memcpy((uint8_t *)buf + first, s_ring, want - first);

    pthread_mutex_unlock(&s_lock);
    return frames;
}

const char *en_cap_backend(void)
{
    return s_desc[0] ? s_desc : "tinyalsa (not started)";
}

/* Write a header and, optionally, a prefill from the ring. Caller holds the
   lock, because the ring must not move underneath the prefill. */
static bool open_wav_locked(FILE **fp, const char *path, uint32_t prefill_ms,
                            uint32_t *written)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    uint8_t hdr[EN_WAV_HDR_BYTES];
    en_wav_header(hdr, sizeof hdr, CAP_RATE, CAP_CHANNELS, CAP_BITS, 0);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return false; }

    uint32_t n = 0;
    if (prefill_ms) {
        uint32_t want = ms_to_bytes(prefill_ms);
        if (want > s_ring_used) want = s_ring_used;
        if (want) {
            uint8_t *tmp = malloc(want);
            if (tmp) {
                uint32_t got = ring_tail(tmp, want);
                if (fwrite(tmp, 1, got, f) == got) n = got;
                free(tmp);
            }
        }
    }
    *fp = f;
    *written = n;
    return true;
}

en_cap_err_t en_cap_record_start(const char *path, uint32_t prefill_ms)
{
    if (!path) return EN_CAP_FAILED;
    if (!s_running) return EN_CAP_NO_DEVICE;

    pthread_mutex_lock(&s_lock);
    if (s_rec) { pthread_mutex_unlock(&s_lock); return EN_CAP_BUSY; }

    FILE *f = 0;
    uint32_t pre = 0;
    bool ok = open_wav_locked(&f, path, prefill_ms, &pre);
    if (ok) { s_rec = f; s_rec_bytes = pre; }
    pthread_mutex_unlock(&s_lock);

    return ok ? EN_CAP_OK : EN_CAP_IO;
}

en_cap_err_t en_cap_record_stop(void)
{
    pthread_mutex_lock(&s_lock);
    FILE *f = s_rec;
    uint32_t n = s_rec_bytes;
    s_rec = 0;
    s_rec_bytes = 0;
    pthread_mutex_unlock(&s_lock);

    if (!f) return EN_CAP_OK;

    /* Patch the two length fields now that the length is known. Written last
       deliberately: until this happens the file is still playable by anything
       that tolerates a zero-length data chunk, and en_wav_repair-style recovery
       from the file size remains possible if the app never gets here. */
    uint8_t hdr[EN_WAV_HDR_BYTES];
    en_wav_header(hdr, sizeof hdr, CAP_RATE, CAP_CHANNELS, CAP_BITS, n);

    bool ok = fseek(f, 0, SEEK_SET) == 0
           && fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    fclose(f);
    return ok ? EN_CAP_OK : EN_CAP_IO;
}

en_cap_err_t en_cap_save_live(const char *path, uint32_t ms)
{
    if (!path) return EN_CAP_FAILED;
    if (!s_running) return EN_CAP_NO_DEVICE;

    pthread_mutex_lock(&s_lock);
    uint32_t want = ms ? ms_to_bytes(ms) : s_ring_used;
    if (want > s_ring_used) want = s_ring_used;

    uint8_t *tmp = want ? malloc(want) : 0;
    uint32_t got = tmp ? ring_tail(tmp, want) : 0;
    pthread_mutex_unlock(&s_lock);

    if (!tmp) return want ? EN_CAP_NO_MEMORY : EN_CAP_FAILED;

    FILE *f = fopen(path, "wb");
    if (!f) { free(tmp); return EN_CAP_IO; }

    uint8_t hdr[EN_WAV_HDR_BYTES];
    en_wav_header(hdr, sizeof hdr, CAP_RATE, CAP_CHANNELS, CAP_BITS, got);

    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr
           && fwrite(tmp, 1, got, f) == got;
    fclose(f);
    free(tmp);
    return ok ? EN_CAP_OK : EN_CAP_IO;
}

const char *en_cap_strerror(en_cap_err_t e)
{
    switch (e) {
    case EN_CAP_OK:        return "ok";
    case EN_CAP_NO_DEVICE: return "no capture device";
    case EN_CAP_BUSY:      return "already recording";
    case EN_CAP_NO_MEMORY: return "out of memory";
    case EN_CAP_IO:        return "write failed";
    default:               return "failed";
    }
}

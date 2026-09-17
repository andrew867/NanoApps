/*
 * tg_audq.c — see tg_audq.h.
 *
 * The mechanics - the flag at +0x64, the chain at +0x54, the duration trap
 * at +0x34, silence by rewriting a buffer the voice is still reading - are
 * Entrain's (apps/entrain/platform/audio_device.c in NanoApps), which is the
 * one place on this device where streaming PCM has been shown to work. What
 * differs is the shape around them: short blocks, a queue-depth clock, and
 * no thread.
 */

#include "tg_audq.h"

#include <string.h>

#define SLOT_FREE   0
#define SLOT_BUILD  1
#define SLOT_QUEUED 2

/* How far ahead of the estimated play cursor an in-place write has to stay.
   The cursor comes from the wall clock, so it is only good to a tick or so;
   with 60 ms blocks this usually means the sounding block plays out and it
   is the ones behind it that go quiet. */
#define SAFETY_MS   40
#define FADE_MS     40
#define SAFETY_FRAMES ((TG_AUDQ_RATE * SAFETY_MS) / 1000u)
#define FADE_FRAMES   ((TG_AUDQ_RATE * FADE_MS) / 1000u)

#define SLOT_MS       ((TG_AUDQ_SLOT_FRAMES * 1000u) / TG_AUDQ_RATE)
/* No tick for this long and everything queued has played out unheard: the
   OS put something else in front. */
#define STARVED_MS    (SLOT_MS * TG_AUDQ_SLOTS * 3u)

#define MS_FRAMES(ms) ((TG_AUDQ_RATE * (uint32_t)(ms)) / 1000u)

static inline void put32(uint8_t *d, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(d + off) = v;
}

static inline bool desc_playing(const tg_audq_slot *s)
{
    return *(volatile const uint8_t *)(s->desc + TG_SFX_OFF_PLAYING) != 0;
}

uint32_t tg_audq_duration_ms(const tg_audq *q, uint32_t frames)
{
    uint32_t num, ms;

    if (q->duration_bad || !q->out_rate || !frames) return 0;

    num = frames * 1000u;
    if (num % q->out_rate) return 0;
    ms = num / q->out_rate;
    if ((ms * q->out_rate) / 1000u != frames) return 0;
    return ms;
}

/* Everything the voice needs. setSource applies the format and the volume
   itself, so a block reached through the chain is as complete as one passed
   to play(). */
static void desc_build(tg_audq *q, tg_audq_slot *s)
{
    uint8_t *d = s->desc;
    uint32_t ms;

    q->ops->desc_ctor(d);
    put32(d, TG_SFX_OFF_BUF_A,    (uint32_t)(uintptr_t)s->pcm);
    put32(d, TG_SFX_OFF_BUF_B,    (uint32_t)(uintptr_t)s->pcm);
    put32(d, TG_SFX_OFF_BUF_LEN,  s->frames * 4u);
    d[TG_SFX_OFF_TYPE] = 0;
    put32(d, TG_SFX_OFF_RATE,     TG_AUDQ_RATE);
    put32(d, TG_SFX_OFF_CHANNELS, 2);
    put32(d, TG_SFX_OFF_BITS,     16);
    put32(d, TG_SFX_OFF_TRIM_LO,  0);
    put32(d, TG_SFX_OFF_TRIM_HI,  0);
    ms = tg_audq_duration_ms(q, s->frames);
    if (!ms && s != &q->quiet) q->zero_duration_joins++;
    put32(d, TG_SFX_OFF_DURATION, ms);
    put32(d, TG_SFX_OFF_VOLUME,   q->volume);
    d[TG_SFX_OFF_PLAYMODE] = 1;
    d[TG_SFX_OFF_FLAGS] = 0;
    q->ops->set_next(d, (uint8_t *)0);
    d[TG_SFX_OFF_PLAYING] = 0;
}

static void fade_in_head(tg_audq_slot *b)
{
    uint32_t n = FADE_FRAMES < b->frames ? FADE_FRAMES : b->frames;

    for (uint32_t i = 0; i < n; i++) {
        int32_t g = (int32_t)((i * 4096u) / n);

        b->pcm[2 * i + 0] = (int16_t)((b->pcm[2 * i + 0] * g) >> 12);
        b->pcm[2 * i + 1] = (int16_t)((b->pcm[2 * i + 1] * g) >> 12);
    }
}

static void enqueue(tg_audq *q, int idx)
{
    q->slot[idx].state = SLOT_QUEUED;
    q->slot[idx].seen_playing = false;
    q->q[q->qn++] = (uint8_t)idx;
}

static void learn_rate(tg_audq *q, const tg_audq_slot *s)
{
    if (!q->out_rate) q->out_rate = q->ops->voice_rate(s->desc);
}

/* A full block: hand it to the OS. Chained onto the tail when there is
   one, chained from the silence when that is what is sounding, and started
   on a fresh voice only when nothing is. */
static void seal(tg_audq *q, int idx)
{
    tg_audq_slot *b = &q->slot[idx];

    b->frames = b->fill;
    b->fill = 0;

    if (q->fade_in) { fade_in_head(b); q->fade_in = false; }
    desc_build(q, b);

    if (q->qn) {
        q->ops->set_next(q->slot[q->q[q->qn - 1]].desc, b->desc);
        enqueue(q, idx);
        return;
    }

    if (q->quiet_armed) {
        q->ops->set_next(q->quiet.desc, b->desc);
        q->quiet_armed = false;
        enqueue(q, idx);
        q->head_start_ms = q->ops->now_ms();
        q->started = true;
        return;
    }

    if (!q->ops->play(b->desc)) {
        b->state = SLOT_FREE;
        return;
    }

    /* Only now is there a voice to read the mixer's rate from, so this block
       went out with a zero duration; correct its counter directly. Safe
       while it sounds: the voice writes that field only at setSource and
       reads it at exhaustion, 60 ms from now. */
    learn_rate(q, b);
    if (tg_audq_duration_ms(q, b->frames))
        q->ops->voice_set_frames(b->desc, b->frames);

    enqueue(q, idx);
    q->head_start_ms = q->ops->now_ms();
    q->started = true;
}

void tg_audq_init(tg_audq *q, const tg_audq_ops *ops, int16_t *pool,
                  uint32_t volume_0_to_7fff)
{
    memset(q, 0, sizeof *q);
    q->ops = ops;
    q->volume = volume_0_to_7fff > 0x7fffu ? 0x7fffu : volume_0_to_7fff;
    q->build = -1;

    for (int i = 0; i < TG_AUDQ_SLOTS; i++)
        q->slot[i].pcm = pool + (size_t)i * TG_AUDQ_SLOT_FRAMES * 2u;
    q->quiet.pcm = pool + (size_t)TG_AUDQ_SLOTS * TG_AUDQ_SLOT_FRAMES * 2u;
    q->quiet.frames = TG_AUDQ_QUIET_FRAMES;
    memset(q->quiet.pcm, 0, TG_AUDQ_QUIET_FRAMES * 4u);

    q->last_tick_ms = ops->now_ms();
    q->fade_in = true;
}

static uint32_t play_cursor(const tg_audq *q)
{
    return MS_FRAMES(q->ops->now_ms() - q->head_start_ms);
}

/* Break the chain and take the sound out of every block the voice has not
   reached: the ones behind the head zeroed, the head faded from a little
   ahead of where the clock says it is. Nothing can stop a sounding voice;
   this is how it goes quiet anyway. */
static void silence_now(tg_audq *q)
{
    for (uint8_t i = 0; i < q->qn; i++) {
        tg_audq_slot *b = &q->slot[q->q[i]];
        uint32_t from, n;

        q->ops->set_next(b->desc, (uint8_t *)0);

        if (i > 0) {
            memset(b->pcm, 0, b->frames * 4u);
            continue;
        }

        from = play_cursor(q) + SAFETY_FRAMES;
        if (from >= b->frames) continue;   /* ends before a fade could finish */

        n = FADE_FRAMES;
        if (from + n > b->frames) n = b->frames - from;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t j = from + k;
            int32_t g = 4096 - (int32_t)((k * 4096u) / n);

            b->pcm[2 * j + 0] = (int16_t)((b->pcm[2 * j + 0] * g) >> 12);
            b->pcm[2 * j + 1] = (int16_t)((b->pcm[2 * j + 1] * g) >> 12);
        }
        memset(b->pcm + (size_t)(from + n) * 2u, 0, (b->frames - from - n) * 4u);
    }
}

static void drop_queue(tg_audq *q)
{
    for (uint8_t i = 0; i < q->qn; i++) {
        q->slot[q->q[i]].state = SLOT_FREE;
        q->slot[q->q[i]].fill = 0;
    }
    q->qn = 0;
}

static void drop_build(tg_audq *q)
{
    if (q->build >= 0) {
        q->slot[q->build].state = SLOT_FREE;
        q->slot[q->build].fill = 0;
        q->build = -1;
    }
}

/* Chain the silent block to itself, so the voice loops it for nothing and
   coming back is a ramp rather than a cold start. */
static void arm_quiet(tg_audq *q)
{
    if (q->quiet_armed && desc_playing(&q->quiet)) return;

    desc_build(q, &q->quiet);
    q->ops->set_next(q->quiet.desc, q->quiet.desc);

    if (q->qn) {
        q->ops->set_next(q->slot[q->q[q->qn - 1]].desc, q->quiet.desc);
    } else if (!q->ops->play(q->quiet.desc)) {
        return;
    }
    q->quiet_armed = true;
}

void tg_audq_tick(tg_audq *q)
{
    uint32_t now = q->ops->now_ms();
    uint32_t since = now - q->last_tick_ms;

    q->last_tick_ms = now;

    if (q->qn && since > STARVED_MS) {
        drop_queue(q);
        drop_build(q);
        q->fade_in = true;
        q->restarts++;
    }

    /* Reclaim. Strictly FIFO, and only a block whose flag has been seen set
       and then clear has finished; one never seen set is still waiting. */
    while (q->qn) {
        tg_audq_slot *h = &q->slot[q->q[0]];

        if (desc_playing(h)) { h->seen_playing = true; break; }
        if (!h->seen_playing) break;

        /* A block that took half again its own length was padded with
           silence: the duration field is being read differently from how
           this was written. Twice in a row and durations stop being stated,
           which costs a faint click per join and nothing worse. */
        if (!q->duration_bad && tg_audq_duration_ms(q, h->frames)) {
            uint32_t expect = (h->frames * 1000u) / TG_AUDQ_RATE;
            uint32_t took = now - q->head_start_ms;

            if (took > expect + expect / 2u) {
                if (++q->overrun >= 2) q->duration_bad = true;
            } else {
                q->overrun = 0;
            }
        }

        h->state = SLOT_FREE;
        h->fill = 0;
        for (uint8_t i = 1; i < q->qn; i++) q->q[i - 1] = q->q[i];
        q->qn--;
        q->head_start_ms = now;
    }

    if (q->paused) { arm_quiet(q); return; }

    /* Playing, and nothing left in the chain: the emulator fell behind.
       The next sealed block starts a fresh voice, with a fade so the
       restart is not a step. */
    if (q->started && !q->qn && !q->quiet_armed) {
        q->underruns++;
        q->fade_in = true;
        q->started = false;
    }
}

unsigned tg_audq_queued_frames(const tg_audq *q)
{
    unsigned n = 0;

    for (uint8_t i = 0; i < q->qn; i++) {
        const tg_audq_slot *b = &q->slot[q->q[i]];

        if (i == 0) {
            uint32_t played = play_cursor(q);

            n += played < b->frames ? b->frames - played : 0;
        } else {
            n += b->frames;
        }
    }
    if (q->build >= 0) n += q->slot[q->build].fill;
    return n;
}

int tg_audq_frames_wanted(const tg_audq *q)
{
    int target = (int)((TG_AUDQ_TARGET_LEAD + 1) * TG_AUDQ_SLOT_FRAMES);

    if (q->paused) return 0;
    return target - (int)tg_audq_queued_frames(q);
}

void tg_audq_push(tg_audq *q, const int16_t *pcm, unsigned frames)
{
    if (q->paused) return;

    while (frames) {
        tg_audq_slot *b;
        unsigned room, n;

        if (q->build < 0) {
            int i;

            for (i = 0; i < TG_AUDQ_SLOTS; i++)
                if (q->slot[i].state == SLOT_FREE) break;
            if (i == TG_AUDQ_SLOTS) {
                q->dropped_frames += frames;
                return;
            }
            q->build = i;
            q->slot[i].state = SLOT_BUILD;
            q->slot[i].fill = 0;
        }

        b = &q->slot[q->build];
        room = TG_AUDQ_SLOT_FRAMES - b->fill;
        n = frames < room ? frames : room;
        memcpy(b->pcm + (size_t)b->fill * 2u, pcm, (size_t)n * 4u);
        b->fill += n;
        pcm += n * 2u;
        frames -= n;

        if (b->fill == TG_AUDQ_SLOT_FRAMES) {
            int idx = q->build;

            q->build = -1;
            seal(q, idx);
        }
    }
}

void tg_audq_pause(tg_audq *q)
{
    if (q->paused) return;
    silence_now(q);
    drop_queue(q);
    drop_build(q);
    q->paused = true;
    q->started = false;
    arm_quiet(q);
}

void tg_audq_resume(tg_audq *q)
{
    if (!q->paused) return;
    q->paused = false;
    q->fade_in = true;
    q->last_tick_ms = q->ops->now_ms();   /* not starvation, a pause */
}

void tg_audq_stop(tg_audq *q)
{
    q->paused = false;
    silence_now(q);
    drop_queue(q);
    drop_build(q);
    q->started = false;
    q->fade_in = true;
    arm_quiet(q);
}

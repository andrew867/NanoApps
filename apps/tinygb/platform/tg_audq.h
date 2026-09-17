/*
 * tg_audq.h — a queue of short PCM blocks for a sink that plays chains.
 *
 * RetailOS has no streaming audio API. What it has is a SoundEffect voice
 * that reads a descriptor's buffer live, sets the descriptor's +0x64 byte
 * while it does, and - when the buffer ends - follows the descriptor's +0x54
 * pointer to the next one without a gap (Entrain's audio_device.c is where
 * that was established, and its header comment is the reference). So a
 * stream is a chain of blocks, and this is the chain.
 *
 * Small blocks, on purpose. Entrain queues 1.28 s blocks because its tick
 * only has to keep up; a game's sound is a response to a button and has to
 * arrive within a fraction of a second of it. 1323 frames is 60 ms at 22050,
 * and a multiple of 441, which is what lets the descriptor's duration field
 * convert exactly on a 22050 or 44100 mixer (see tg_audq_duration_ms). Six
 * of them, with the emulator told to keep two queued beyond the one
 * sounding, is 120-180 ms of latency and 300 ms of slack.
 *
 * The queue is also the emulator's clock. Nothing on RetailOS blocks, so
 * "how many emulated frames should this tick run" is answered by how far the
 * queue is below its target: none, one, or two. Pitch stays exact because the
 * mixer drains at its own crystal, and the odd repeated or dropped video
 * frame is invisible.
 *
 * Everything the OS actually does is behind tg_audq_ops, so the same code
 * runs against a fake on the host, where the tests play the audio task's
 * part: set the flag, follow the chain, clear the flag.
 */

#ifndef TINYGB_AUDQ_H
#define TINYGB_AUDQ_H

#include <stdbool.h>
#include <stdint.h>

#define TG_AUDQ_RATE          22050u
#define TG_AUDQ_SLOT_FRAMES   1323u   /* 60 ms; 3 x 441 */
#define TG_AUDQ_SLOTS         6
#define TG_AUDQ_TARGET_LEAD   2       /* slots queued beyond the sounding one */
#define TG_AUDQ_QUIET_FRAMES  7056u   /* 320 ms of silence, looped while paused */
#define TG_AUDQ_DESC_BYTES    0x80

/* int16 samples the caller provides for all the buffers: stereo, so twice
   the frames. 63 KB. */
#define TG_AUDQ_POOL_SAMPLES \
    ((TG_AUDQ_SLOTS * TG_AUDQ_SLOT_FRAMES + TG_AUDQ_QUIET_FRAMES) * 2u)

/* The descriptor, as much of it as this file writes or reads. From
   SoundEffectDescriptor::ctor, voice::setSource and the completion handler
   in RetailOS 1.1.2. */
#define TG_SFX_OFF_BUF_A    0x04
#define TG_SFX_OFF_BUF_B    0x08
#define TG_SFX_OFF_BUF_LEN  0x0C   /* bytes */
#define TG_SFX_OFF_TYPE     0x10   /* 0 = linear PCM */
#define TG_SFX_OFF_RATE     0x14
#define TG_SFX_OFF_CHANNELS 0x18
#define TG_SFX_OFF_BITS     0x1C
#define TG_SFX_OFF_VOLUME   0x24   /* 0..0x7fff */
#define TG_SFX_OFF_DURATION 0x34   /* MILLISECONDS -> voice+0x48 sample count */
#define TG_SFX_OFF_TRIM_LO  0x38
#define TG_SFX_OFF_TRIM_HI  0x3C
#define TG_SFX_OFF_VOICE    0x48   /* setSource writes the voice here */
#define TG_SFX_OFF_PLAYMODE 0x51
#define TG_SFX_OFF_FLAGS    0x52
#define TG_SFX_OFF_NEXT     0x54   /* the chain */
#define TG_SFX_OFF_PLAYING  0x64   /* byte: 1 while a voice holds it */

/* What the OS does, and the host fakes. */
typedef struct tg_audq_ops {
    void     (*desc_ctor)(uint8_t *desc);
    /* Point desc's chain at next (NULL to break it). One aligned store on
       the device, so the audio task sees old or new, never torn. */
    void     (*set_next)(uint8_t *desc, uint8_t *next);
    /* Start a voice on desc. False when there is no player. */
    bool     (*play)(uint8_t *desc);
    /* The mixer's OUTPUT rate, read off the voice that took desc up, or 0
       when there is no voice yet or the number is not credible. */
    uint32_t (*voice_rate)(const uint8_t *desc);
    /* Write the voice's remaining-sample counter directly: the first block
       of a stream goes out before any rate is known. */
    void     (*voice_set_frames)(const uint8_t *desc, uint32_t frames);
    uint32_t (*now_ms)(void);
} tg_audq_ops;

typedef struct {
    uint8_t  desc[TG_AUDQ_DESC_BYTES];
    int16_t *pcm;
    uint32_t frames;        /* what the sealed block holds */
    uint32_t fill;          /* how much has been pushed so far */
    uint8_t  state;
    bool     seen_playing;
} tg_audq_slot;

typedef struct {
    const tg_audq_ops *ops;
    tg_audq_slot slot[TG_AUDQ_SLOTS];
    tg_audq_slot quiet;

    uint8_t  q[TG_AUDQ_SLOTS];   /* QUEUED slots, FIFO: q[0] is sounding */
    uint8_t  qn;
    int      build;              /* the slot being pushed into, or -1 */

    uint32_t head_start_ms;
    uint32_t last_tick_ms;
    uint32_t out_rate;           /* the mixer's, once a voice has said */
    uint32_t volume;             /* 0..0x7fff */

    bool     started;            /* a voice has been given real audio */
    bool     paused;
    bool     quiet_armed;
    bool     fade_in;
    bool     duration_bad;
    uint8_t  overrun;

    /* For the overlay and the tests. */
    unsigned underruns;          /* the chain ran dry while playing */
    unsigned restarts;           /* starved: dropped and started over */
    unsigned zero_duration_joins;
    unsigned dropped_frames;     /* pushed with no slot free */
} tg_audq;

/* `pool` is TG_AUDQ_POOL_SAMPLES int16s the caller owns for the life of
   the app (never freed: a voice may still be reading it). */
void tg_audq_init(tg_audq *q, const tg_audq_ops *ops, int16_t *pool,
                  uint32_t volume_0_to_7fff);

/* Once per tick, before anything else: reclaim finished blocks, notice
   starvation, keep the silence looping while paused. */
void tg_audq_tick(tg_audq *q);

/* Frames not yet played: the rest of the sounding block (estimated from the
   clock), everything queued behind it, and what is in the block being
   built. */
unsigned tg_audq_queued_frames(const tg_audq *q);

/* Target minus queued. Positive means "run an emulated frame and push it";
   the caller stops at two per tick. */
int tg_audq_frames_wanted(const tg_audq *q);

/* Append. Fills the current block, seals and hands it over when full, and
   starts on the next free one. Frames with no free block are counted and
   dropped. Never blocks. */
void tg_audq_push(tg_audq *q, const int16_t *pcm, unsigned frames);

/* Pause: silence within a fade, chain broken, a silent block looped so the
   voice and the DAC stay up. Resume: the next block fades in and is chained
   from the silence. Stop: pause without the paused state, for a cartridge
   change - the silence keeps looping until the next push. */
void tg_audq_pause(tg_audq *q);
void tg_audq_resume(tg_audq *q);
void tg_audq_stop(tg_audq *q);

/* What to put in the descriptor's duration field so the voice's counter
   comes out as exactly `frames`, or 0 when no millisecond value would.
   Public for the tests. */
uint32_t tg_audq_duration_ms(const tg_audq *q, uint32_t frames);

#endif /* TINYGB_AUDQ_H */

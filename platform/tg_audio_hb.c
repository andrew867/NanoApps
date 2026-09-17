/*
 * tg_audio_hb.c — see tg_audio_hb.h.
 */

#include "tg_audio_hb.h"

#include "hb_sdk.h"

#define SFX_CTOR_ADDR        (0x08417efcu | 1u)   /* SoundEffectDescriptor::ctor */
#define SFX_PLAYER_INST_ADDR (0x08417eb8u | 1u)   /* sfxPlayer singleton */
#define SFX_PLAYER_PLAY_ADDR (0x0841828cu | 1u)   /* sfxPlayer::play(player, desc, cb, cbdata) */

/* Inside the voice object setSource leaves at desc+0x48: the mixer's output
   rate (compared against desc+0x14 at VA 0x8631610 to decide whether to
   resample - which is what makes it the output rate and not ours), and the
   remaining-sample counter setSource derives from desc+0x34. */
#define VOICE_OFF_RATE    0x0C
#define VOICE_OFF_COUNTER 0x48

typedef void *(*sfx_ctor_t)(void *self);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc, void *cb, void *cbdata);

static uint32_t voice_of(const uint8_t *desc)
{
    uint32_t v = *(volatile const uint32_t *)(desc + TG_SFX_OFF_VOICE);

    /* A pointer into SDRAM, word aligned, or nothing. */
    if (!v || (v & 3u) || v < 0x08000000u || v >= 0x10000000u) return 0;
    return v;
}

static void op_ctor(uint8_t *desc)
{
    ((sfx_ctor_t)SFX_CTOR_ADDR)(desc);
}

static void op_set_next(uint8_t *desc, uint8_t *next)
{
    *(volatile uint32_t *)(desc + TG_SFX_OFF_NEXT) = (uint32_t)(uintptr_t)next;
}

static bool op_play(uint8_t *desc)
{
    void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();

    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, desc, (void *)0, (void *)0);
    return true;
}

static uint32_t op_voice_rate(const uint8_t *desc)
{
    uint32_t v = voice_of(desc), r;

    if (!v) return 0;
    r = *(volatile const uint32_t *)(uintptr_t)(v + VOICE_OFF_RATE);
    return (r >= 8000u && r <= 192000u) ? r : 0;
}

static void op_voice_set_frames(const uint8_t *desc, uint32_t frames)
{
    uint32_t v = voice_of(desc);

    if (v) *(volatile uint32_t *)(uintptr_t)(v + VOICE_OFF_COUNTER) = frames;
}

static uint32_t op_now_ms(void)
{
    return hb_time_uptime_ms();
}

const tg_audq_ops *tg_audio_hb_ops(void)
{
    static const tg_audq_ops ops = {
        op_ctor, op_set_next, op_play, op_voice_rate, op_voice_set_frames,
        op_now_ms,
    };
    return &ops;
}

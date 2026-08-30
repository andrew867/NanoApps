/*
 * model.h — everything the UI draws, in one place.
 *
 * The screens read this and nothing else. That keeps the drawing free of any
 * knowledge of sysfs, HCI or tinyalsa, and it is what lets the whole interface
 * be rendered on a desktop with no tuner: the host preview fills the same
 * struct with a plausible station and every screen believes it.
 */

#ifndef RADIOPLUS_MODEL_H
#define RADIOPLUS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "core/rds.h"
#include "core/region.h"
#include "core/store.h"

typedef struct {
    /* Tuner */
    uint32_t khz;
    uint8_t  rssi;           /* 0..255 as the chip reports it */
    int8_t   snr;
    bool     stereo;
    bool     powered;
    bool     tuner_ok;
    const char *tuner_note;  /* why not, when tuner_ok is false */

    /* Region and decoded RDS */
    const en_region_t *region;
    en_rds_t rds;
    bool     rds_on;

    /* Capture, the live buffer and recording */
    bool     capture_ok;
    bool     recording;
    uint32_t rec_ms;
    uint32_t live_ms;        /* how much is buffered */
    uint32_t live_cap_ms;
    uint32_t live_pos_ms;    /* how far behind live we are; 0 is live */
    uint32_t overruns;

    /* Playback. The radio and a recording share one player, so "what is
       coming out of the headphones" is one question with one answer. */
    bool     play_file;          /* a recording rather than the radio */
    char     play_name[96];
    uint32_t play_pos_ms;
    uint32_t play_len_ms;
    bool     play_paused;
    uint32_t behind_ms;          /* how far behind live; 0 is live */
    uint32_t behind_max_ms;      /* how far back the buffer allows */
    bool     play_ok;

    bool     ta_record;          /* auto-record traffic announcements */
    bool     ta_recording;       /* one is being recorded right now */

    /* Which optional screens are in the swipe order. Here rather than private
       to the UI because they persist, and persistence is the platform's job -
       the same reason ta_record lives here and not beside the checkbox. */
    bool     simple_screen;      /* big preset buttons and nothing else */
    bool     wide_screen;        /* the landscape readout */

    /* Presets and recordings */
    en_presets_t presets;
    uint8_t  library_count;
    char     library[12][96];   /* a dated, named recording is ~34 chars */

    bool     can_raw;        /* show the register explorer at all */
    const char *backend;
    const char *capture_backend;
} rp_model_t;

extern rp_model_t rp_model;

/* Pull the current state in from whatever platform is underneath. Called from
   the frame callback a few times a second, not per frame - reading RSSI is a
   round trip through a driver and there is nothing to see at 60 Hz. */
void rp_model_refresh(void);

/*
 * Called as each stage of bring-up begins, and again with a reason when one
 * fails. Set before the first rp_model_refresh, which is what does the work.
 *
 * A hook rather than a direct call into the UI because core and platform know
 * nothing about screens - the host preview has no bring-up to report and the
 * device build should not gain a UI dependency to say what it is doing.
 */
typedef struct {
    void (*step)(const char *what);
    void (*failed)(const char *why);
} rp_progress_t;

void rp_model_set_progress(const rp_progress_t *p);

/* Actions the UI invokes. Implemented per platform. */
void rp_act_tune(uint32_t khz);
void rp_act_step(bool up);
void rp_act_seek(bool up);
void rp_act_power(bool on);
void rp_act_record_toggle(void);
void rp_act_save_live(uint32_t ms);
void rp_act_preset_toggle(void);
void rp_act_set_region(const en_region_t *rg);

/*
 * Raw register access from the explorer.
 *
 * A write is also remembered: the tuner forgets everything when it powers down,
 * so an override that is not replayed at start-up lasts only as long as the
 * chip stays on, and the explorer would be a toy rather than a settings screen.
 * rp_act_reg_revert drops the override and puts the register back to whatever
 * the chip powers up with.
 */
bool rp_act_reg_read(uint8_t addr, uint8_t *buf, uint8_t len);
void rp_act_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len);
void rp_act_reg_revert(uint8_t addr);
bool rp_act_reg_overridden(uint8_t addr);

/* Playback and the live buffer. */
void rp_act_play_file(const char *name);
void rp_act_play_live(void);        /* jump to the front of the buffer */
void rp_act_nudge(int32_t ms);      /* negative goes back */
void rp_act_pause_toggle(void);
void rp_act_ta_record(bool on);

/* Turn the optional screens on or off, and remember it. */
void rp_act_show_simple(bool on);
void rp_act_show_wide(bool on);

/* Flag or unflag the tuned station for the simple screen, saving the presets.
   Returns false when the grid is full, which the UI says out loud. */
bool rp_act_simple_toggle(uint32_t khz);

#endif /* RADIOPLUS_MODEL_H */

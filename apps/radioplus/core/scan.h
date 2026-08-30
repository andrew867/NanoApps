/*
 * scan.h — sweep the band and turn what is on it into presets.
 *
 * Filling the preset list by hand means tuning to a station, deciding it is
 * worth keeping, saving it, and doing that twenty times. The tuner can do it:
 * step the whole band, note where the signal is, and come back for the names.
 *
 * Two passes, deliberately.
 *
 *   The SWEEP is fast and only asks "is anything here". A hundred milliseconds
 *   per channel is enough for the RSSI reading to settle, and a European band
 *   at 100 kHz spacing is 205 channels - twenty seconds for the lot.
 *
 *   The NAMING pass goes back to the channels that answered yes and sits on
 *   each one long enough for RDS to arrive. That is seconds, not milliseconds
 *   - the station name comes in group 0A, two characters at a time, and a
 *   weak signal can take several seconds to spell eight letters. Doing that on
 *   every channel would take eight minutes; doing it on the twenty that have
 *   something on them takes one.
 *
 * The whole thing is a state machine over an externally supplied clock, with
 * no knowledge of any tuner. It says which channel it wants to be on and is
 * told what is there. That is what lets it run identically on the device and
 * in the host preview, and be tested with neither.
 */

#ifndef RADIOPLUS_SCAN_H
#define RADIOPLUS_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "rds.h"
#include "region.h"
#include "store.h"

/* Room for a full band of strong stations. Past this the sweep keeps going but
   stops recording, and en_scan_overflowed() says so - a scan that quietly kept
   the first forty and dropped the rest would look like a complete scan. */
#define EN_SCAN_MAX_HITS 40

typedef struct {
    uint32_t khz;
    uint8_t  rssi;                        /* the best seen while dwelling */
    uint16_t pi;
    uint8_t  pty;
    bool     rbds;
    bool     named;                       /* RDS gave us a station name */
    char     name[EN_PRESET_NAME_LEN + 1];
} en_scan_hit_t;

typedef enum {
    EN_SCAN_IDLE = 0,
    EN_SCAN_SWEEP,      /* stepping the band, RSSI only */
    EN_SCAN_NAMING,     /* sitting on each hit, waiting for RDS */
    EN_SCAN_DONE
} en_scan_phase_t;

typedef struct {
    en_scan_phase_t phase;
    const en_region_t *region;

    uint8_t  threshold;      /* RSSI at or above which a channel counts */
    uint32_t sweep_ms;       /* dwell while sweeping */
    uint32_t name_ms;        /* dwell while waiting for a name */

    uint32_t khz;            /* the channel it wants to be on */
    uint32_t waited_ms;
    uint8_t  peak;           /* best RSSI seen on this channel so far */

    int      done;           /* channels swept, or hits named */
    int      total;          /* channels in the band, or hits to name */
    uint8_t  n_hits;
    bool     overflowed;
    en_scan_hit_t hits[EN_SCAN_MAX_HITS];

    /* Where the tuner was before the scan, so cancelling puts it back. A scan
       that leaves you on a different station than you started on has taken
       something away from you as well as given something. */
    uint32_t resume_khz;
} en_scan_t;

/* Begin. `threshold` is an RSSI as the chip reports it, 0..255. Returns false
   if the region is missing or has no channels, in which case the scan stays
   idle rather than running over an empty band. */
bool en_scan_start(en_scan_t *s, const en_region_t *rg, uint32_t from_khz,
                   uint8_t threshold);

/* Give up. Leaves `resume_khz` for the caller to tune back to. */
void en_scan_stop(en_scan_t *s);

/*
 * Advance by `dt_ms`, having observed `rssi` and `rds` on the channel the
 * scan last asked for.
 *
 * Returns true when it wants a different channel, writing it to *tune_khz.
 * The caller tunes and keeps ticking. Returns false while it is still
 * gathering, and when the scan is over - check the phase for that.
 */
bool en_scan_tick(en_scan_t *s, uint32_t dt_ms, uint8_t rssi,
                  const en_rds_t *rds, uint32_t *tune_khz);

/* 0..100, across both passes, for a progress bar that does not jump back to
   zero when the naming pass starts. */
uint8_t en_scan_percent(const en_scan_t *s);

/* Write the results into a preset list. Existing presets at the same
   frequency are updated rather than duplicated - a rescan should improve the
   list, not double it. Returns how many were added or updated. */
uint8_t en_scan_commit(const en_scan_t *s, en_presets_t *p);

#endif /* RADIOPLUS_SCAN_H */

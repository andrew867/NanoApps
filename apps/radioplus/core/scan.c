/*
 * scan.c — see scan.h.
 *
 * No allocation, no libc beyond memcpy-by-hand, no knowledge of any tuner.
 */

#include "scan.h"

static void copy_name(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Trailing spaces are normal in an RDS station name - the field is eight
   characters and short names are padded - and a preset called "BBC R4  " sorts
   and compares differently from one called "BBC R4". */
static void trim(char *s)
{
    int n = 0;
    while (s[n]) n++;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static int band_channels(const en_region_t *rg)
{
    if (!rg || !rg->step_khz || rg->high_khz <= rg->low_khz) return 0;
    return (int)((rg->high_khz - rg->low_khz) / rg->step_khz) + 1;
}

bool en_scan_start(en_scan_t *s, const en_region_t *rg, uint32_t from_khz,
                   uint8_t threshold)
{
    int n = band_channels(rg);
    if (!s || n <= 0) return false;

    /* Zeroed by hand: core/ has no string.h, and the fields are few enough
       that naming them is also the list of what a scan consists of. */
    s->phase = EN_SCAN_SWEEP;
    s->region = rg;
    s->threshold = threshold;
    s->sweep_ms = 100;
    s->name_ms = 2500;
    s->khz = rg->low_khz;
    s->waited_ms = 0;
    s->peak = 0;
    s->done = 0;
    s->total = n;
    s->n_hits = 0;
    s->overflowed = false;
    s->resume_khz = from_khz;
    return true;
}

void en_scan_stop(en_scan_t *s)
{
    if (!s) return;
    s->phase = EN_SCAN_IDLE;
}

/* Take the strongest reading seen while sitting here, not the last one. RSSI
   on a just-tuned channel climbs for a moment; sampling once at the end would
   be fine, but a station that fades in and out during the dwell should be
   judged on its best moment rather than on whichever instant we looked. */
static void observe(en_scan_t *s, uint8_t rssi)
{
    if (rssi > s->peak) s->peak = rssi;
}

static void next_sweep_channel(en_scan_t *s, uint32_t *tune_khz)
{
    if (s->peak >= s->threshold) {
        if (s->n_hits < EN_SCAN_MAX_HITS) {
            en_scan_hit_t *h = &s->hits[s->n_hits++];
            h->khz = s->khz;
            h->rssi = s->peak;
            h->pi = 0;
            h->pty = 0;
            h->rbds = s->region ? s->region->rbds : false;
            h->named = false;
            h->name[0] = 0;
        } else {
            s->overflowed = true;
        }
    }

    s->done++;
    s->peak = 0;
    s->waited_ms = 0;

    if (s->done >= s->total) {
        /* On to the names, or straight to the end if nothing answered. */
        if (s->n_hits == 0) {
            s->phase = EN_SCAN_DONE;
            *tune_khz = s->resume_khz;
            return;
        }
        s->phase = EN_SCAN_NAMING;
        s->done = 0;
        s->total = s->n_hits;
        s->khz = s->hits[0].khz;
        *tune_khz = s->khz;
        return;
    }

    s->khz = en_region_step(s->region, s->khz, true);
    *tune_khz = s->khz;
}

static void next_named_channel(en_scan_t *s, uint32_t *tune_khz)
{
    s->done++;
    s->waited_ms = 0;
    s->peak = 0;

    if (s->done >= s->total) {
        s->phase = EN_SCAN_DONE;
        *tune_khz = s->resume_khz;
        return;
    }
    s->khz = s->hits[s->done].khz;
    *tune_khz = s->khz;
}

bool en_scan_tick(en_scan_t *s, uint32_t dt_ms, uint8_t rssi,
                  const en_rds_t *rds, uint32_t *tune_khz)
{
    uint32_t dummy;
    if (!tune_khz) tune_khz = &dummy;
    if (!s || s->phase == EN_SCAN_IDLE || s->phase == EN_SCAN_DONE)
        return false;

    s->waited_ms += dt_ms;
    observe(s, rssi);

    if (s->phase == EN_SCAN_SWEEP) {
        if (s->waited_ms < s->sweep_ms) return false;
        next_sweep_channel(s, tune_khz);
        return true;
    }

    /* Naming. Take the name the moment it is complete rather than sitting out
       the full dwell: on a strong station it arrives in well under a second,
       and waiting anyway would triple the length of the pass for nothing. */
    en_scan_hit_t *h = &s->hits[s->done];
    if (rds && rds->ps_valid) {
        copy_name(h->name, (int)sizeof h->name, rds->ps);
        trim(h->name);
        h->named = h->name[0] != 0;
        h->pi = rds->pi;
        h->pty = rds->pty;
        h->rbds = rds->rbds;
    }
    if (h->rssi < s->peak) h->rssi = s->peak;

    if (h->named || s->waited_ms >= s->name_ms) {
        next_named_channel(s, tune_khz);
        return true;
    }
    return false;
}

uint8_t en_scan_percent(const en_scan_t *s)
{
    if (!s) return 0;
    if (s->phase == EN_SCAN_DONE) return 100;
    if (s->phase == EN_SCAN_IDLE || s->total <= 0) return 0;

    /* The sweep is most of the time and most of the bar. Splitting it 70/30
       rather than by channel count keeps the bar moving at roughly one speed:
       a naming step is twenty-five times longer than a sweep step, so equal
       weighting would crawl through the first three quarters and then stall. */
    int within = (s->done * 100) / s->total;
    if (s->phase == EN_SCAN_SWEEP) return (uint8_t)((within * 70) / 100);
    return (uint8_t)(70 + (within * 30) / 100);
}

uint8_t en_scan_commit(const en_scan_t *s, en_presets_t *p)
{
    if (!s || !p) return 0;

    uint8_t n = 0;
    for (uint8_t i = 0; i < s->n_hits; i++) {
        const en_scan_hit_t *h = &s->hits[i];

        int at = en_preset_find(p, h->khz);
        if (at >= 0) {
            /* Already known. Fill in what the scan learned and leave the rest
               alone - in particular leave `simple` alone, because a rescan
               must not rearrange the screen the user chose. */
            en_preset_t *e = &p->list[at];
            if (h->named) copy_name(e->name, (int)sizeof e->name, h->name);
            if (h->pi) e->pi = h->pi;
            e->pty = h->pty;
            e->rbds = h->rbds;
            n++;
            continue;
        }

        en_preset_t e;
        e.khz = h->khz;
        e.pi = h->pi;
        e.pty = h->pty;
        e.rbds = h->rbds;
        e.simple = false;
        copy_name(e.name, (int)sizeof e.name, h->name);
        if (en_preset_add(p, &e)) n++;
    }

    en_preset_sort(p);
    return n;
}

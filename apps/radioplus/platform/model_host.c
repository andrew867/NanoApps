/*
 * model_host.c — a plausible station, for looking at the interface.
 *
 * The screens read rp_model and nothing else, so filling it in here renders the
 * whole application on a desktop with no tuner, no iPod and no Bluetooth. That
 * is not a toy: laying out a 240 x 432 screen by imagining it does not work, and
 * the fastest way to find a label that overflows or a row that collides is to
 * render every screen and look at all of them at once.
 *
 * The data is chosen to be awkward on purpose. The station name is the full
 * eight characters, the radio text is longer than the box, the programme type
 * is one whose RBDS and RDS names differ, and there are enough presets to
 * scroll. A layout that survives this survives a real station.
 */

#include "../model.h"

#include <string.h>

rp_model_t rp_model;

static bool s_recording;
static uint32_t s_rec_ms;

/* Build the RDS state the way it would actually arrive: through the decoder,
   group by group, rather than by writing the fields directly. That way the
   preview exercises the same code the device does, and a decoder bug shows up
   here rather than only on hardware. */
static void seed_rds(void)
{
    en_rds_init(&rp_model.rds, true);

    const char *ps = "CBC RAD";
    for (uint8_t seg = 0; seg < 4; seg++) {
        uint16_t b = (uint16_t)((0u << 12) | (1u << 10) | (22u << 5) | seg);
        char c0 = seg * 2 < 7 ? ps[seg * 2] : ' ';
        char c1 = seg * 2 + 1 < 7 ? ps[seg * 2 + 1] : ' ';
        uint16_t blk[4] = { 0xC2B5, b,
                            (uint16_t)((226u << 8) | 108u),
                            (uint16_t)(((uint8_t)c0 << 8) | (uint8_t)c1) };
        en_rds_group(&rp_model.rds, blk, EN_RDS_ALL);
    }

    const char *rt =
        "Q with Tom Power - Neil Young on Harvest Moon and thirty years of it  ";
    for (uint8_t seg = 0; seg < 16; seg++) {
        uint16_t b = (uint16_t)((2u << 12) | (1u << 10) | (22u << 5) | seg);
        uint16_t c = (uint16_t)(((uint8_t)rt[seg * 4] << 8)
                                | (uint8_t)rt[seg * 4 + 1]);
        uint16_t d = (uint16_t)(((uint8_t)rt[seg * 4 + 2] << 8)
                                | (uint8_t)rt[seg * 4 + 3]);
        uint16_t blk[4] = { 0xC2B5, b, c, d };
        en_rds_group(&rp_model.rds, blk, EN_RDS_ALL);
    }
}

static void seed_presets(void)
{
    static const struct { uint32_t khz; const char *name; uint8_t pty; }
    seed[] = {
        {  88500, "CBC RAD", 22 },
        {  93500, "OZ FM",    5 },
        {  97500, "K-ROCK",   6 },
        {  99100, "HITS FM",  9 },
        { 102900, "VOCM FM", 11 },
        { 106900, "CHMR",    23 },
    };

    en_presets_init(&rp_model.presets, "Americas");
    for (unsigned i = 0; i < sizeof seed / sizeof seed[0]; i++) {
        en_preset_t e;
        memset(&e, 0, sizeof e);
        e.khz = seed[i].khz;
        e.pty = seed[i].pty;
        e.rbds = true;
        e.pi = (uint16_t)(0xC000 + i);
        strncpy(e.name, seed[i].name, sizeof e.name - 1);
        en_preset_add(&rp_model.presets, &e);
    }
}

void rp_model_refresh(void)
{
    static bool seeded;
    if (!seeded) {
        seeded = true;

        rp_model.region = en_region_find("Americas");
        rp_model.khz = 98500;
        rp_model.rssi = 62;
        rp_model.snr = 24;
        rp_model.stereo = true;
        rp_model.powered = true;
        rp_model.tuner_ok = true;
        rp_model.rds_on = true;
        rp_model.capture_ok = true;
        rp_model.live_ms = 21000;
        rp_model.live_cap_ms = 30000;
        rp_model.can_raw = true;
        rp_model.backend = "bcm2078-bt at /sys/devices/platform/soc/bcm2078";
        rp_model.capture_backend = "tinyalsa hw:0,1  44100 Hz 2 ch  30s buffer";

        rp_model.library_count = 3;
        strcpy(rp_model.library[0], "2026-08-28 09-14 CBC RAD 98.5.wav");
        strcpy(rp_model.library[1], "2026-08-27 18-02 K-ROCK 97.5.wav");
        strcpy(rp_model.library[2], "2026-08-26 21-47 traffic 98.5.wav");

        seed_rds();
        seed_presets();
    }

    rp_model.recording = s_recording;
    rp_model.rec_ms = s_rec_ms;
    if (s_recording) s_rec_ms += 100;
}

/* ---- actions ------------------------------------------------------------- */

void rp_act_tune(uint32_t khz) { rp_model.khz = khz; }

void rp_act_step(bool up)
{
    rp_model.khz = en_region_step(rp_model.region, rp_model.khz, up);
}

void rp_act_seek(bool up) { rp_act_step(up); }
void rp_act_power(bool on) { rp_model.powered = on; }

void rp_act_record_toggle(void)
{
    s_recording = !s_recording;
    if (s_recording) s_rec_ms = 0;
}

void rp_act_save_live(uint32_t ms) { (void)ms; }

void rp_act_preset_toggle(void)
{
    en_preset_t e;
    memset(&e, 0, sizeof e);
    e.khz = rp_model.khz;
    e.pty = rp_model.rds.pty;
    e.rbds = rp_model.rds.rbds;
    e.pi = rp_model.rds.pi;
    if (rp_model.rds.ps_valid) strncpy(e.name, rp_model.rds.ps, sizeof e.name - 1);

    if (en_preset_find(&rp_model.presets, e.khz) >= 0)
        en_preset_remove(&rp_model.presets, e.khz);
    else
        en_preset_add(&rp_model.presets, &e);
    en_preset_sort(&rp_model.presets);
}

void rp_act_set_region(const en_region_t *rg)
{
    if (!rg) return;
    rp_model.region = rg;
    rp_model.rds.rbds = rg->rbds;
    if (!en_region_on_grid(rg, rp_model.khz))
        rp_model.khz = en_region_step(rg, rp_model.khz, true);
}

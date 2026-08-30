/*
 * model_device.c — the model on RetailOS.
 *
 * What works here is persistence and the interface. What does not, yet, is the
 * tuner and the audio, and the reason is worth stating plainly because it is a
 * protocol constraint rather than missing work.
 *
 * The tuner is reached by HCI vendor command 0xFC15, and on this device the OS
 * Bluetooth stack owns that transport. HCI is flow-controlled: the controller
 * grants command credits in Command Complete events, and a host is expected to
 * account for them. Injecting a command underneath a running stack breaks that
 * accounting in both directions - our completion can be consumed as the OS's,
 * or the OS can see a credit returned for a command it never sent - and the
 * failure mode is its command queue stalling, which takes Bluetooth and FM with
 * it. That is why this backend does not send commands, and why it is not simply
 * a matter of being careful with reads.
 *
 * The supported route is the OS's own tuner object, ISL::TRFTuner, whose vtable
 * is located (0x087f0050, with the concrete RTXC implementation at 0x087ed41c)
 * and whose slots are mapped by shape but not by meaning. Calling a slot whose
 * name is a guess would be worse than not calling it, so this reports the tuner
 * unavailable and says why. See RADIO_NOTES.md.
 *
 * Everything above the platform layer is unaffected: the screens read rp_model,
 * settings and presets persist, and the parts that cannot work are absent
 * rather than broken.
 */

#include "../model.h"

#include "hb_sdk.h"

#include <string.h>

/* The device build is freestanding and has no formatted printing. A bounded
   copy is all this file needed it for. */
static void copy_str(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!cap) return;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

rp_model_t rp_model;

#define RP_DIR       "/RadioPlus"
#define RP_SETTINGS  RP_DIR "/settings.json"
#define RP_PRESETS   RP_DIR "/presets.json"
#define RP_RECORDS   RP_DIR "/Recordings"

static en_settings_t s_settings;

/* ---- files ---------------------------------------------------------------- */

static void paths_init(void)
{
    if (!hb_fs_exists(RP_DIR)) hb_fs_mkdir(RP_DIR);
    if (!hb_fs_exists(RP_RECORDS)) hb_fs_mkdir(RP_RECORDS);
}

static void settings_load(void)
{
    en_settings_default(&s_settings);

    static char buf[4096];
    uint32_t n = hb_fs_read(RP_SETTINGS, buf, sizeof buf - 1);
    if (!n) return;
    buf[n] = 0;
    en_settings_load(&s_settings, buf, n);
}

static void settings_save(void)
{
    if (rp_model.region)
        copy_str(s_settings.region, sizeof s_settings.region, rp_model.region->name);
    s_settings.khz = rp_model.khz;
    s_settings.rds_on = rp_model.rds_on;
    s_settings.ta_record = rp_model.ta_record;

    static char buf[4096];
    uint32_t n = en_settings_save(&s_settings, buf, sizeof buf);

    /* Zero means it would have been truncated, and a short settings file looks
       valid with fields missing. Leaving the previous one is the better half of
       a bad choice. */
    if (n) hb_fs_write(RP_SETTINGS, buf, n);
}

static void presets_load(void)
{
    static char buf[16384];
    uint32_t n = hb_fs_read(RP_PRESETS, buf, sizeof buf - 1);
    if (!n) {
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
        return;
    }
    buf[n] = 0;
    if (!en_presets_load(&rp_model.presets, buf, n))
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
}

static void presets_save(void)
{
    static char buf[16384];
    uint32_t n = en_presets_save(&rp_model.presets, buf, sizeof buf);
    if (n) hb_fs_write(RP_PRESETS, buf, n);
}

static void library_scan(void)
{
    rp_model.library_count = 0;

    hb_dir_t d;
    if (!hb_fs_dir_open(&d, RP_RECORDS, false)) return;

    char name[128];
    bool is_dir = false;
    while (hb_fs_dir_next(&d, name, sizeof name, &is_dir)
           && rp_model.library_count < 12) {
        if (is_dir) continue;

        uint32_t len = 0;
        while (name[len]) len++;
        if (len < 5 || name[len - 4] != '.' || name[len - 3] != 'w'
            || name[len - 2] != 'a' || name[len - 1] != 'v') continue;

        /* Whole or not at all: a truncated name would give a row that cannot
           be opened, which is worse than a row that is not there. */
        if (len >= sizeof rp_model.library[0]) continue;
        memcpy(rp_model.library[rp_model.library_count], name, len + 1);
        rp_model.library_count++;
    }
    hb_fs_dir_close(&d);
}

/* ---- the model ------------------------------------------------------------ */

static void bring_up(void)
{
    paths_init();
    settings_load();

    rp_model.region = en_region_find(s_settings.region);
    if (!rp_model.region) rp_model.region = en_region_find("Americas");

    rp_model.rds_on = s_settings.rds_on;
    rp_model.ta_record = s_settings.ta_record;
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);

    presets_load();
    library_scan();

    /* Stated rather than left blank. A radio that shows nothing and explains
       nothing is indistinguishable from a broken one. */
    rp_model.tuner_ok = false;
    rp_model.backend = "not wired: the OS owns the HCI transport";
    rp_model.tuner_note =
        "The tuner is reached by an HCI vendor command, and the system "
        "Bluetooth stack owns that transport. HCI is flow controlled, so "
        "injecting commands underneath it can stall its command queue. The "
        "supported route is the OS tuner object, whose vtable is located but "
        "whose methods are not yet identified.";

    rp_model.capture_ok = false;
    rp_model.capture_backend = "not wired: FM capture buffer not located";
    rp_model.play_ok = false;

    /* Not offered at all rather than offered and failing. */
    rp_model.can_raw = false;

    rp_model.khz = s_settings.khz;
    if (!rp_model.khz && rp_model.region) rp_model.khz = rp_model.region->low_khz;
}

void rp_model_refresh(void)
{
    static bool up;
    if (!up) { up = true; bring_up(); }

    /* Nothing to poll. The screens redraw from what is already here, which on
       this platform is the settings, the presets and the recordings. */
}

/* ---- actions -------------------------------------------------------------- */

/*
 * The tuner actions are accepted and remembered rather than refused. A user
 * setting a region or saving a preset on this build is making a choice that
 * will be correct the moment the tuner is wired, and throwing it away because
 * the radio is not working yet would lose real intent.
 */

void rp_act_tune(uint32_t khz)
{
    rp_model.khz = khz;
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);
    settings_save();
}

void rp_act_step(bool up)
{
    rp_act_tune(en_region_step(rp_model.region, rp_model.khz, up));
}

void rp_act_seek(bool up) { rp_act_step(up); }
void rp_act_power(bool on) { rp_model.powered = on; }

void rp_act_set_region(const en_region_t *rg)
{
    if (!rg) return;
    rp_model.region = rg;
    rp_model.rds.rbds = rg->rbds;
    copy_str(rp_model.presets.region, sizeof rp_model.presets.region, rg->name);

    if (!en_region_on_grid(rg, rp_model.khz))
        rp_act_tune(en_region_step(rg, rp_model.khz, true));

    presets_save();
    settings_save();
}

void rp_act_preset_toggle(void)
{
    en_preset_t e;
    memset(&e, 0, sizeof e);
    e.khz = rp_model.khz;
    e.pty = rp_model.rds.pty;
    e.rbds = rp_model.rds.rbds;
    e.pi = rp_model.rds.pi;
    if (rp_model.rds.ps_valid)
        copy_str(e.name, sizeof e.name, rp_model.rds.ps);

    if (en_preset_find(&rp_model.presets, e.khz) >= 0)
        en_preset_remove(&rp_model.presets, e.khz);
    else
        en_preset_add(&rp_model.presets, &e);

    en_preset_sort(&rp_model.presets);
    presets_save();
}

void rp_act_ta_record(bool on)
{
    rp_model.ta_record = on;
    settings_save();
}

void rp_act_show_simple(bool on)
{
    rp_model.simple_screen = on;
    settings_save();
}

void rp_act_show_wide(bool on)
{
    rp_model.wide_screen = on;
    settings_save();
}

bool rp_act_simple_toggle(uint32_t khz)
{
    int at = en_preset_find(&rp_model.presets, khz);
    if (at < 0) return false;
    if (!en_preset_set_simple(&rp_model.presets, khz,
                              !rp_model.presets.list[at].simple))
        return false;
    presets_save();
    return true;
}

/* Audio. Nothing to do without a capture path, and doing nothing quietly is
   correct here: the buttons that would drive these are not shown, because the
   model reports capture and playback unavailable. */
void rp_act_record_toggle(void) { }
void rp_act_save_live(uint32_t ms) { (void)ms; }
void rp_act_play_file(const char *name) { (void)name; }
void rp_act_play_live(void) { }
void rp_act_nudge(int32_t ms) { (void)ms; }
void rp_act_pause_toggle(void) { }

/* Raw register access is not offered on this platform - can_raw is false, so
   the explorer shows why instead of a list of controls that would fail. */
bool rp_act_reg_read(uint8_t addr, uint8_t *buf, uint8_t len)
{
    (void)addr; (void)buf; (void)len;
    return false;
}

void rp_act_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    (void)addr; (void)buf; (void)len;
}

void rp_act_reg_revert(uint8_t addr) { (void)addr; }
bool rp_act_reg_overridden(uint8_t addr) { (void)addr; return false; }

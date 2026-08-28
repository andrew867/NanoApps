/*
 * model_linux.c — fill rp_model from the real tuner and the real capture.
 *
 * The screens read rp_model and nothing else, so this is the only file that
 * knows both sides. Everything expensive is rate-limited rather than done per
 * frame: reading RSSI is a round trip through sysfs into a driver that talks
 * HCI, and doing that sixty times a second would keep the transport busy to
 * show a number that changes twice.
 *
 * Recording names carry the time, the station and the frequency, because a
 * directory of recordings called rec001 is useless a week later. The RDS
 * sidecar is written beside the audio under the same stem.
 */

#include "../model.h"
#include "tuner.h"
#include "capture.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

rp_model_t rp_model;

static char s_home[256];
static char s_rec_dir[320];
static char s_presets_path[320];

/* The recording in progress, and its sidecar. */
static FILE    *s_sidecar;
static en_sidecar_t s_sc;
static uint32_t s_rec_started_ms;
static char     s_rec_stem[512];   /* directory plus a dated, named stem */

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000));
}

/* ---- where things live ---------------------------------------------------- */

static void paths_init(void)
{
    const char *h = getenv("RADIOPLUS_HOME");
    if (!h || !*h) h = "/tmp/radioplus";
    snprintf(s_home, sizeof s_home, "%s", h);
    mkdir(s_home, 0755);

    snprintf(s_rec_dir, sizeof s_rec_dir, "%s/recordings", s_home);
    mkdir(s_rec_dir, 0755);

    snprintf(s_presets_path, sizeof s_presets_path, "%s/presets.json", s_home);
}

static void presets_load(void)
{
    FILE *f = fopen(s_presets_path, "rb");
    if (!f) {
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
        return;
    }
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    if (!en_presets_load(&rp_model.presets, buf, (uint32_t)n))
        en_presets_init(&rp_model.presets,
                        rp_model.region ? rp_model.region->name : "");
}

static void presets_save(void)
{
    static char buf[16384];
    uint32_t n = en_presets_save(&rp_model.presets, buf, sizeof buf);

    /* en_presets_save reports 0 rather than truncating. Writing a short file
       would look like a valid preset list with entries missing, which is worse
       than leaving the previous one alone. */
    if (!n) return;

    FILE *f = fopen(s_presets_path, "wb");
    if (!f) return;
    fwrite(buf, 1, n, f);
    fclose(f);
}

/* Newest first, which is the order anyone wants a recordings list in. */
static void library_scan(void)
{
    rp_model.library_count = 0;

    DIR *d = opendir(s_rec_dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) && rp_model.library_count < 12) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".wav")) continue;

        /* Copied whole or not at all. A truncated name is worse than a missing
           row, because the row would be there and would not open - and names
           this app writes are about thirty characters, so anything that does
           not fit did not come from here. */
        if (len >= sizeof rp_model.library[0]) continue;
        memcpy(rp_model.library[rp_model.library_count], e->d_name, len + 1);
        rp_model.library_count++;
    }
    closedir(d);

    /* Names begin with the timestamp, so a reverse sort is newest first. */
    for (uint8_t i = 1; i < rp_model.library_count; i++) {
        char k[96];
        snprintf(k, sizeof k, "%s", rp_model.library[i]);
        int j = (int)i - 1;
        while (j >= 0 && strcmp(rp_model.library[j], k) < 0) {
            snprintf(rp_model.library[j + 1], sizeof rp_model.library[0],
                     "%s", rp_model.library[j]);
            j--;
        }
        snprintf(rp_model.library[j + 1], sizeof rp_model.library[0], "%s", k);
    }
}

/* ---- startup -------------------------------------------------------------- */

static void bring_up(void)
{
    paths_init();

    rp_model.region = en_region_find("Americas");
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);

    en_tuner_err_t te = en_tuner_init();
    rp_model.backend = en_tuner_backend();
    rp_model.tuner_ok = (te == EN_TUNER_OK);
    rp_model.tuner_note = en_tuner_strerror(te);
    rp_model.can_raw = rp_model.tuner_ok && en_tuner_can_raw();

    if (rp_model.tuner_ok) {
        en_tuner_power(true);
        en_tuner_set_region(rp_model.region);
        en_tuner_rds_enable(true);
        rp_model.rds_on = true;
    }

    /* Capture is started with the tuner rather than with the record button:
       the SoC side of IIS2 is clocked by the capture PCM, so nothing is
       audible until this is open. It is how the radio makes sound, not just
       how it is recorded. */
    en_cap_err_t ce = en_cap_start(30);
    rp_model.capture_ok = (ce == EN_CAP_OK);
    rp_model.capture_backend = en_cap_backend();

    presets_load();
    library_scan();

    /* Start on the first preset if there is one - resuming where you were is
       better than landing on the bottom of the band. */
    if (rp_model.presets.count) {
        rp_model.khz = rp_model.presets.list[0].khz;
        if (rp_model.tuner_ok) en_tuner_tune(rp_model.khz);
    } else if (rp_model.region) {
        rp_model.khz = rp_model.region->low_khz;
    }
}

/* ---- the sidecar ---------------------------------------------------------- */

static void sidecar_open(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s.rds.json", s_rec_stem);

    s_sidecar = fopen(path, "wb");
    if (!s_sidecar) return;

    char buf[2048];
    uint32_t n = en_sidecar_begin(&s_sc, buf, sizeof buf, rp_model.khz,
                                  rp_model.region ? rp_model.region->name : "",
                                  rp_model.rds.rbds, &rp_model.rds);
    if (n) fwrite(buf, 1, n, s_sidecar);
}

static void sidecar_close(uint32_t duration_ms)
{
    if (!s_sidecar) return;

    char buf[2048];
    uint32_t n = en_sidecar_end(&s_sc, buf, sizeof buf, duration_ms,
                                &rp_model.rds);
    if (n) fwrite(buf, 1, n, s_sidecar);
    fclose(s_sidecar);
    s_sidecar = 0;
}

/* ---- the refresh ---------------------------------------------------------- */

void rp_model_refresh(void)
{
    static bool up;
    static uint32_t last_slow, last_rds;

    if (!up) { up = true; bring_up(); }

    uint32_t t = now_ms();

    /* Signal and frequency, a few times a second. Each of these is a driver
       round trip and nothing on screen changes faster. */
    if (rp_model.tuner_ok && t - last_slow >= 400u) {
        last_slow = t;

        en_tuner_state_t st;
        if (en_tuner_state(&st) == EN_TUNER_OK) {
            if (st.khz) rp_model.khz = st.khz;
            rp_model.rssi = st.rssi;
            rp_model.snr = st.snr;
            rp_model.stereo = st.stereo;
            rp_model.powered = st.powered;
        }
    }

    /* RDS more often, because groups arrive continuously and the FIFO is
       finite - polling too slowly loses them rather than merely delaying
       them. */
    if (rp_model.tuner_ok && rp_model.rds_on && t - last_rds >= 200u) {
        last_rds = t;

        uint16_t groups[8][4];
        uint8_t valid[8];
        int n = en_tuner_rds_poll(&rp_model.rds, groups, valid, 8);

        /* Every group also goes to the sidecar while recording, with its
           offset into the audio, so the display can be rebuilt against the
           recording afterwards. */
        if (s_sidecar && n > 0) {
            uint32_t at = t - s_rec_started_ms;
            char buf[512];
            for (int i = 0; i < n; i++) {
                uint32_t w = en_sidecar_group(&s_sc, buf, sizeof buf, at,
                                              groups[i], valid[i]);
                if (w) fwrite(buf, 1, w, s_sidecar);
            }
        }
    }

    en_cap_state_t cs;
    en_cap_state(&cs);
    rp_model.capture_ok = cs.running;
    rp_model.recording = cs.recording;
    rp_model.rec_ms = cs.recorded_ms;
    rp_model.live_ms = cs.live_ms;
    rp_model.live_cap_ms = cs.live_cap_ms;
    rp_model.overruns = cs.overruns;

    /* A recording that stopped itself - a full disk, most likely - has to be
       noticed here, because the capture thread cannot close the sidecar. */
    if (s_sidecar && !cs.recording) sidecar_close(cs.recorded_ms);
}

/* ---- actions -------------------------------------------------------------- */

void rp_act_tune(uint32_t khz)
{
    rp_model.khz = khz;
    if (rp_model.tuner_ok) en_tuner_tune(khz);

    /* A new station is a new set of RDS state; carrying the old name over a
       retune is worse than showing nothing for a second. */
    en_rds_init(&rp_model.rds, rp_model.region ? rp_model.region->rbds : true);
}

void rp_act_step(bool up)
{
    rp_act_tune(en_region_step(rp_model.region, rp_model.khz, up));
}

void rp_act_seek(bool up)
{
    if (rp_model.tuner_ok) {
        en_tuner_seek(up);
        en_rds_init(&rp_model.rds,
                    rp_model.region ? rp_model.region->rbds : true);
    } else {
        rp_act_step(up);
    }
}

void rp_act_power(bool on)
{
    rp_model.powered = on;
    if (rp_model.tuner_ok) en_tuner_power(on);
}

void rp_act_record_toggle(void)
{
    if (!rp_model.capture_ok) return;

    if (rp_model.recording) {
        en_cap_record_stop();
        sidecar_close(rp_model.rec_ms);
        library_scan();
        return;
    }

    /* Name it so it is identifiable a week later: when, what, and where on the
       dial. A directory of rec001.wav is not a library. */
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);

    char station[24];
    snprintf(station, sizeof station, "%s",
             rp_model.rds.ps_valid ? rp_model.rds.ps : "FM");
    for (char *p = station; *p; p++)
        if (*p == '/' || *p == ' ') *p = '-';

    snprintf(s_rec_stem, sizeof s_rec_stem,
             "%s/%04d-%02d-%02d %02d-%02d %s %u.%u",
             s_rec_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, station,
             rp_model.khz / 1000u, (rp_model.khz % 1000u) / 100u);

    char wav[600];
    snprintf(wav, sizeof wav, "%s.wav", s_rec_stem);

    /* Ten seconds of prefill, so pressing record keeps what was just said as
       well as what comes next - which is the only useful moment to press it. */
    if (en_cap_record_start(wav, 10000) == EN_CAP_OK) {
        s_rec_started_ms = now_ms();
        sidecar_open();
    }
}

void rp_act_save_live(uint32_t ms)
{
    if (!rp_model.capture_ok) return;

    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);

    char path[600];
    snprintf(path, sizeof path, "%s/%04d-%02d-%02d %02d-%02d live %u.%u.wav",
             s_rec_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min,
             rp_model.khz / 1000u, (rp_model.khz % 1000u) / 100u);

    if (en_cap_save_live(path, ms) == EN_CAP_OK) library_scan();
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
        snprintf(e.name, sizeof e.name, "%s", rp_model.rds.ps);

    if (en_preset_find(&rp_model.presets, e.khz) >= 0)
        en_preset_remove(&rp_model.presets, e.khz);
    else
        en_preset_add(&rp_model.presets, &e);

    en_preset_sort(&rp_model.presets);
    presets_save();
}

void rp_act_set_region(const en_region_t *rg)
{
    if (!rg) return;

    rp_model.region = rg;
    rp_model.rds.rbds = rg->rbds;
    snprintf(rp_model.presets.region, sizeof rp_model.presets.region,
             "%s", rg->name);

    if (rp_model.tuner_ok) en_tuner_set_region(rg);

    /* A frequency that was on the old grid may not be on the new one. */
    if (!en_region_on_grid(rg, rp_model.khz))
        rp_act_tune(en_region_step(rg, rp_model.khz, true));

    presets_save();
}

/*
 * audio_spike — throwaway probe for the OS SFX audio path.
 *
 * hb_audio.c gives us "load a WAV file, play it once". Everything an
 * entrainment player needs to know beyond that is undocumented, so this
 * harness asks the device directly. Each test prints to the screen AND
 * to the DRAM trace ring (`./start trace`), because some of these probes
 * can reboot the device and the trace is what survives.
 *
 * Setup:
 *     python3 gen_spike_wavs.py
 *     # copy ./wavs/ to the iPod main volume as /WAV/spike/
 *     ./start run audio_spike
 *
 * Tests, and the question each one answers:
 *     T0  L/R separation     does the mixer preserve stereo? (binaural or not)
 *     T1  format matrix      which sample rates / depths / containers load
 *     T2  blocking + rate    does play_now return immediately; real output rate
 *     T3  descriptor watch   which descriptor byte marks "finished"
 *     T3b completion cb      does the play() callback argument ever fire
 *     T4  overlap            second play: cuts off, mixes, or queues
 *     T5  loop / chain       PLAYMODE + FLAGS sweep, and the NEXTSFX pointer
 *     T6  latency            per-step cost of ctor/load/fields/play
 *     T7  size + heap        does loadFile pull the whole file into RAM
 *     T8  subrange           can loadFile play a window of a larger file
 *
 * The 4-step ceremony (ctor / loadfile / setfields / play) with a scale-3
 * hb_draw_str between each step is load-bearing — see the FIXME in
 * sdk/hb_audio.c. Every audio call here keeps that shape.
 */

#include "hb_sdk.h"
#include "hb_heap.h"

/* ---- raw OS entry points -------------------------------------------------
 * The SDK wrappers hardcode offset=0/size=0 and a NULL callback. The whole
 * point of T3b and T8 is to pass those, so the spike calls the firmware
 * directly rather than widening the SDK before we know what the args do. */

#define SFX_CTOR_ADDR        (0x08417efcu | 1u)
#define SFX_LOADFILE_ADDR    (0x08417f78u | 1u)
#define SFX_PLAYER_INST_ADDR (0x08417eb8u | 1u)
#define SFX_PLAYER_PLAY_ADDR (0x0841828cu | 1u)
#define PTHREAD_CREATE_ADDR  (0x080226f8u | 1u)

#define SFX_OFF_VOLUME   0x24
#define SFX_OFF_PLAYMODE 0x51
#define SFX_OFF_FLAGS    0x52
#define SFX_OFF_NEXTSFX  0x54
#define SFX_DESC_LEN     0x78

#define VOL_MAIN 0   /* main filesystem */
#define VOL_RES  4   /* OS resource bundle */

typedef void *(*sfx_ctor_t)(void *self);
typedef int   (*sfx_loadfile_t)(void *self, const char *path, int volume,
                                uint32_t offset, uint32_t size);
typedef void *(*sfx_player_inst_t)(void);
typedef void  (*sfx_player_play_t)(void *player, void *desc,
                                   void *cb, void *cbdata);
typedef int   (*pthread_create_t)(uint32_t *thread, void *attr,
                                  void *(*start)(void *), void *arg);

static uint8_t g_desc_a[0x80];
static uint8_t g_desc_b[0x80];

#define SPIKE_DIR "/WAV/spike/"

/* ---- tiny formatting (no libc) ------------------------------------------- */

static char *app_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

static char *app_u32(char *p, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { *p++ = '0'; *p = 0; return p; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *app_i32(char *p, int32_t v)
{
    if (v < 0) { *p++ = '-'; return app_u32(p, (uint32_t)(-v)); }
    return app_u32(p, (uint32_t)v);
}

static const char HEXD[] = "0123456789abcdef";

static char *app_hex8(char *p, uint8_t v)
{
    *p++ = HEXD[(v >> 4) & 0xF];
    *p++ = HEXD[v & 0xF];
    *p = 0;
    return p;
}

/* ---- screen log ---------------------------------------------------------- */

#define LOG_TOP     26
#define LOG_LINE_H  10
#define LOG_MAX     38

static int g_log_y;

static void log_clear(const char *title)
{
    hb_fill_screen(HB_BLACK);
    hb_draw_str(4, 2, title, 2, HB_CYAN, HB_BLACK);
    g_log_y = 0;
}

static void lg(const char *s)
{
    if (g_log_y >= LOG_MAX) return;
    hb_draw_str(4, (int16_t)(LOG_TOP + g_log_y * LOG_LINE_H), s, 1,
                HB_WHITE, HB_BLACK);
    g_log_y++;
}

static void lg_warn(const char *s)
{
    if (g_log_y >= LOG_MAX) return;
    hb_draw_str(4, (int16_t)(LOG_TOP + g_log_y * LOG_LINE_H), s, 1,
                HB_YELLOW, HB_BLACK);
    g_log_y++;
}

/* ---- MIPI keepalive ------------------------------------------------------
 * A scale-3 string draw is what empirically keeps the OS audio subsystem
 * from panicking between SFX calls. Cheap insurance: never call two audio
 * entry points back to back without one of these. */

static void chk(const char *msg)
{
    hb_fill_rect(0, HB_SCREEN_H - 30, HB_SCREEN_W, 26, HB_BLACK);
    hb_draw_str(4, HB_SCREEN_H - 30, msg, 3, HB_YELLOW, HB_BLACK);
}

/* Wait `ms`, keeping MIPI busy so the audio task stays scheduled.
 * Returns early (true) if the user taps, so a runaway loop is escapable. */
static bool wait_ms(uint32_t ms, const char *label)
{
    uint32_t t0 = hb_time_uptime_ms();
    uint32_t last = 0;
    for (;;) {
        uint32_t dt = hb_time_uptime_ms() - t0;
        if (dt >= ms) return false;
        if (dt - last >= 100) {
            last = dt;
            char buf[32], *p = buf;
            p = app_str(p, label);
            p = app_str(p, " ");
            p = app_u32(p, dt);
            chk(buf);
        }
        int16_t tx, ty;
        if (hb_ui_poll(&tx, &ty) == HB_UI_TAP) return true;
        hb_ui_pace();
    }
}

/* ---- audio primitives, always via the 4-step ceremony --------------------- */

static void sfx_ctor(void *d)
{
    ((sfx_ctor_t)SFX_CTOR_ADDR)(d);
}

static int sfx_load(void *d, const char *path, int vol,
                    uint32_t off, uint32_t len)
{
    return ((sfx_loadfile_t)SFX_LOADFILE_ADDR)(d, path, vol, off, len);
}

static void sfx_fields(void *d, uint32_t volume, uint8_t playmode,
                       uint8_t flags, void *next)
{
    *(volatile uint32_t *)((uint8_t *)d + SFX_OFF_VOLUME) = volume;
    ((uint8_t *)d)[SFX_OFF_PLAYMODE] = playmode;
    ((uint8_t *)d)[SFX_OFF_FLAGS] = flags;
    *(volatile void **)((uint8_t *)d + SFX_OFF_NEXTSFX) = next;
}

static bool sfx_play(void *d, void *cb, void *cbdata)
{
    void *player = ((sfx_player_inst_t)SFX_PLAYER_INST_ADDR)();
    if (!player) return false;
    ((sfx_player_play_t)SFX_PLAYER_PLAY_ADDR)(player, d, cb, cbdata);
    return true;
}

/* Full canonical play of a /WAV/spike file. Returns loadFile rc, or -1 if
 * the player singleton was missing. Fills timings if non-NULL. */
typedef struct {
    uint32_t ctor_ms, load_ms, fields_ms, play_ms;
} step_ms_t;

static int play_spike(void *d, const char *name, uint8_t playmode,
                      uint8_t flags, void *next, void *cb, step_ms_t *out)
{
    char path[96], *p = path;
    p = app_str(p, SPIKE_DIR);
    app_str(p, name);

    uint32_t t0, t1;
    step_ms_t s = {0, 0, 0, 0};

    chk("step1 ctor");
    t0 = hb_time_uptime_ms();
    sfx_ctor(d);
    t1 = hb_time_uptime_ms(); s.ctor_ms = t1 - t0;

    chk("step2 load");
    t0 = hb_time_uptime_ms();
    int rc = sfx_load(d, path, VOL_MAIN, 0, 0);
    t1 = hb_time_uptime_ms(); s.load_ms = t1 - t0;
    if (out) *out = s;
    if (rc != 0) return rc;

    chk("step3 fields");
    t0 = hb_time_uptime_ms();
    sfx_fields(d, 0x5000, playmode, flags, next);
    t1 = hb_time_uptime_ms(); s.fields_ms = t1 - t0;

    chk("step4 play");
    t0 = hb_time_uptime_ms();
    bool ok = sfx_play(d, cb, (void *)0);
    t1 = hb_time_uptime_ms(); s.play_ms = t1 - t0;

    if (out) *out = s;
    return ok ? 0 : -1;
}

/* ---- T0: does the mixer keep left and right apart? ----------------------- */

static void test_lr(void)
{
    log_clear("T0 L/R SEPARATION");
    lg("lr.wav: 440Hz LEFT 2s, gap,");
    lg("        880Hz RIGHT 2s.");
    lg("");
    lg_warn("PUT HEADPHONES ON. Listen:");
    lg(" both tones in BOTH cups");
    lg("   -> mixer downmixes to mono");
    lg("   -> BINAURAL IS IMPOSSIBLE,");
    lg("      isochronic only.");
    lg(" one cup then the other");
    lg("   -> stereo preserved, all");
    lg("      three modes are viable.");
    lg("");

    hb_trace_log("SPK0", 0, 0);
    step_ms_t s;
    int rc = play_spike(g_desc_a, "lr.wav", 1, 0, (void *)0, (void *)0, &s);
    hb_trace_log("SPK0", (uint32_t)rc, s.load_ms);

    char buf[40], *p = buf;
    p = app_str(p, "load rc="); p = app_i32(p, rc);
    p = app_str(p, "  load="); p = app_u32(p, s.load_ms); app_str(p, "ms");
    lg(buf);
    wait_ms(6000, "playing");
    lg("");
    lg_warn("tap to return to menu");
}

/* ---- T1: which containers does loadFile accept? -------------------------- */

typedef struct {
    const char *file;
    const char *desc;
} fmt_case_t;

static const fmt_case_t FMT_CASES[] = {
    { "s44s16.wav",  "44100 16 st" },
    { "s48s16.wav",  "48000 16 st" },
    { "s32s16.wav",  "32000 16 st" },
    { "s22s16.wav",  "22050 16 st" },
    { "s16s16.wav",  "16000 16 st" },
    { "s11s16.wav",  "11025 16 st" },
    { "s08s16.wav",  " 8000 16 st" },
    { "s44m16.wav",  "44100 16 mono" },
    { "s44s08.wav",  "44100  8 st" },
    { "s44s24.wav",  "44100 24 st" },
    { "s44s32f.wav", "44100 f32 st" },
    { "s44sxt.wav",  "44100 16 EXTENS" },
    { "s44slst.wav", "44100 16 +LIST" },
};
#define N_FMT_CASES ((int)(sizeof FMT_CASES / sizeof FMT_CASES[0]))

static void test_formats(void)
{
    log_clear("T1 FORMAT MATRIX");
    lg("loadFile only (no playback).");
    lg("rc 0 = accepted.");
    lg("");

    for (int i = 0; i < N_FMT_CASES; i++) {
        char path[96], *p = path;
        p = app_str(p, SPIKE_DIR);
        app_str(p, FMT_CASES[i].file);

        uint32_t sz = hb_fs_size(path);

        chk("fmt ctor");
        sfx_ctor(g_desc_a);
        chk("fmt load");
        uint32_t t0 = hb_time_uptime_ms();
        int rc = sfx_load(g_desc_a, path, VOL_MAIN, 0, 0);
        uint32_t dt = hb_time_uptime_ms() - t0;
        chk("fmt done");

        hb_trace_log("SPK1", (uint32_t)i, (uint32_t)rc);

        char buf[40], *q = buf;
        q = app_str(q, FMT_CASES[i].desc);
        while ((int)(q - buf) < 16) *q++ = ' ';
        *q = 0;
        q = app_str(q, sz ? "rc=" : "MISSING rc=");
        q = app_i32(q, rc);
        q = app_str(q, " ");
        q = app_u32(q, dt);
        app_str(q, "ms");
        if (rc == 0) lg(buf); else lg_warn(buf);
    }
    lg("");
    lg("NB: accepted != played at the");
    lg("stated rate. T2 measures the");
    lg("real output rate.");
    lg_warn("tap to return to menu");
}

/* ---- T2: blocking? and what is the true output rate? --------------------- */

static void timed_play(const char *file, uint32_t nominal_ms)
{
    step_ms_t s;
    uint32_t t_call = hb_time_uptime_ms();
    int rc = play_spike(g_desc_a, file, 1, 0, (void *)0, (void *)0, &s);
    uint32_t t_ret = hb_time_uptime_ms();

    char buf[40], *p = buf;
    p = app_str(p, file);
    p = app_str(p, " rc="); p = app_i32(p, rc);
    lg(buf);

    p = buf;
    p = app_str(p, "  call->return ");
    p = app_u32(p, t_ret - t_call);
    p = app_str(p, "ms (nom ");
    p = app_u32(p, nominal_ms);
    app_str(p, ")");
    lg(buf);

    if (rc != 0) return;

    lg_warn("  TAP THE MOMENT IT STOPS");
    uint32_t t0 = hb_time_uptime_ms();
    bool tapped = wait_ms(nominal_ms + 8000, "listen");
    uint32_t heard = hb_time_uptime_ms() - t0;

    p = buf;
    if (!tapped) {
        app_str(p, "  no tap - timed out");
    } else {
        p = app_str(p, "  heard ");
        p = app_u32(p, heard);
        p = app_str(p, "ms -> rate x");
        /* ratio in hundredths: nominal/heard */
        uint32_t ratio = heard ? (nominal_ms * 100u) / heard : 0;
        p = app_u32(p, ratio / 100);
        *p++ = '.';
        uint32_t frac = ratio % 100;
        *p++ = (char)('0' + frac / 10);
        *p++ = (char)('0' + frac % 10);
        *p = 0;
    }
    lg(buf);
    hb_trace_log("SPK2", heard, nominal_ms);
}

static void test_blocking(void)
{
    log_clear("T2 BLOCKING + RATE");
    lg("If call->return is much less");
    lg("than the file duration, play");
    lg("is async. The tap timing then");
    lg("gives the real output rate:");
    lg("x1.00 = rate honoured,");
    lg("x1.09 on a 48k file = forced");
    lg("to 44.1k, and so on.");
    lg("");
    timed_play("t5s.wav", 5000);
    lg("");
    timed_play("s48s16.wav", 5000);
    lg("");
    lg_warn("tap to return to menu");
}

/* ---- T3: watch the descriptor for a completion marker -------------------- */

static uint8_t g_snap0[SFX_DESC_LEN];
static uint8_t g_snapN[SFX_DESC_LEN];
static uint32_t g_first_ms[SFX_DESC_LEN];
static uint32_t g_last_ms[SFX_DESC_LEN];

static void test_desc_watch(void)
{
    log_clear("T3 DESCRIPTOR WATCH");
    lg("t5s.wav, polling all 0x78");
    lg("descriptor bytes for 9s.");
    lg("A byte that flips at ~5000ms");
    lg("is the completion marker we");
    lg("need for gapless chaining.");
    lg("");

    hb_trace_log("SPK3", 0, 0);
    step_ms_t s;
    uint32_t t_play = hb_time_uptime_ms();
    int rc = play_spike(g_desc_a, "t5s.wav", 1, 0, (void *)0, (void *)0, &s);
    if (rc != 0) {
        lg_warn("load failed - is /WAV/spike/ there?");
        lg_warn("tap to return to menu");
        return;
    }

    for (int i = 0; i < SFX_DESC_LEN; i++) {
        g_snap0[i] = g_desc_a[i];
        g_snapN[i] = g_desc_a[i];
        g_first_ms[i] = 0;
        g_last_ms[i] = 0;
    }

    uint32_t deadline = t_play + 9000;
    uint32_t paint = 0;
    while (hb_time_uptime_ms() < deadline) {
        uint32_t now = hb_time_uptime_ms() - t_play;
        for (int i = 0; i < SFX_DESC_LEN; i++) {
            uint8_t v = ((volatile uint8_t *)g_desc_a)[i];
            if (v != g_snapN[i]) {
                g_snapN[i] = v;
                if (!g_first_ms[i]) g_first_ms[i] = now ? now : 1;
                g_last_ms[i] = now;
            }
        }
        if (now - paint >= 200) {
            paint = now;
            char buf[32], *p = buf;
            p = app_str(p, "watching ");
            app_u32(p, now);
            chk(buf);
        }
        hb_ui_pace();
    }

    int shown = 0;
    for (int i = 0; i < SFX_DESC_LEN && shown < 20; i++) {
        if (!g_first_ms[i]) continue;
        char buf[40], *p = buf;
        p = app_str(p, "off 0x"); p = app_hex8(p, (uint8_t)i);
        p = app_str(p, " "); p = app_hex8(p, g_snap0[i]);
        p = app_str(p, ">"); p = app_hex8(p, g_snapN[i]);
        p = app_str(p, " t1="); p = app_u32(p, g_first_ms[i]);
        p = app_str(p, " tN="); app_u32(p, g_last_ms[i]);
        lg(buf);
        hb_trace_log("SPK3", (uint32_t)i, g_last_ms[i]);
        shown++;
    }
    if (!shown) {
        lg_warn("no descriptor byte changed.");
        lg("Playback state lives in the");
        lg("player, not the descriptor:");
        lg("completion must be timed.");
    }
    lg_warn("tap to return to menu");
}

/* ---- T3b: does the play() callback argument ever fire? ------------------- */

static volatile uint32_t g_cb_hits;
static volatile uint32_t g_cb_ms;
static volatile uint32_t g_cb_a0, g_cb_a1;

static void completion_cb(void *a0, void *a1)
{
    g_cb_hits++;
    g_cb_ms = hb_time_uptime_ms();
    g_cb_a0 = (uint32_t)(uintptr_t)a0;
    g_cb_a1 = (uint32_t)(uintptr_t)a1;
}

static void test_callback(void)
{
    log_clear("T3b COMPLETION CALLBACK");
    lg("sfxPlayer::play() takes a cb +");
    lg("cbdata the SDK always passes");
    lg("as NULL. Pass a real one and");
    lg("see whether it fires, and when.");
    lg("");
    lg_warn("if this reboots, the arg is");
    lg_warn("not a plain C callback.");
    lg("(./start trace shows how far");
    lg(" we got)");
    lg("");

    g_cb_hits = 0; g_cb_ms = 0; g_cb_a0 = 0; g_cb_a1 = 0;
    hb_trace_log("SPKC", 0xA1, (uint32_t)(uintptr_t)completion_cb);

    uint32_t t0 = hb_time_uptime_ms();
    step_ms_t s;
    int rc = play_spike(g_desc_a, "t5s.wav", 1, 0, (void *)0,
                        (void *)completion_cb, &s);
    hb_trace_log("SPKC", 0xA2, (uint32_t)rc);

    char buf[40], *p = buf;
    p = app_str(p, "survived play(), rc=");
    app_i32(p, rc);
    lg(buf);

    wait_ms(9000, "waiting cb");

    p = buf;
    p = app_str(p, "cb hits=");
    p = app_u32(p, g_cb_hits);
    if (g_cb_hits) {
        p = app_str(p, " at +");
        p = app_u32(p, g_cb_ms - t0);
        app_str(p, "ms");
    }
    lg(buf);
    if (g_cb_hits) {
        p = buf;
        p = app_str(p, "  a0="); p = app_u32(p, g_cb_a0);
        p = app_str(p, " a1="); app_u32(p, g_cb_a1);
        lg(buf);
        lg("-> real completion signal.");
        lg("   Gapless chaining is on.");
    } else {
        lg("-> never fired. Completion");
        lg("   must be timed by us.");
    }
    hb_trace_log("SPKC", 0xA3, g_cb_hits);
    lg_warn("tap to return to menu");
}

/* ---- T4: what does a second play() do to the first? ---------------------- */

static void test_overlap(void)
{
    log_clear("T4 OVERLAP");
    lg("A = 440Hz LEFT 4s.");
    lg("B = 880Hz RIGHT 4s, started");
    lg("1.5s later on a 2nd descriptor.");
    lg("");
    lg_warn("Listen for:");
    lg(" A stops when B starts -> CUT");
    lg(" both together        -> MIX");
    lg(" B waits for A        -> QUEUE");
    lg("");

    step_ms_t s;
    int rca = play_spike(g_desc_a, "ovl_a.wav", 1, 0, (void *)0, (void *)0, &s);
    hb_trace_log("SPK4", 0xA, (uint32_t)rca);
    wait_ms(1500, "A playing");
    int rcb = play_spike(g_desc_b, "ovl_b.wav", 1, 0, (void *)0, (void *)0, &s);
    hb_trace_log("SPK4", 0xB, (uint32_t)rcb);

    char buf[40], *p = buf;
    p = app_str(p, "rcA="); p = app_i32(p, rca);
    p = app_str(p, "  rcB="); app_i32(p, rcb);
    lg(buf);

    wait_ms(7000, "listening");
    lg("");
    lg("Second pass: same descriptor");
    lg("reused while still playing.");
    int rc1 = play_spike(g_desc_a, "ovl_a.wav", 1, 0, (void *)0, (void *)0, &s);
    wait_ms(1500, "A playing");
    int rc2 = play_spike(g_desc_a, "ovl_b.wav", 1, 0, (void *)0, (void *)0, &s);
    p = buf;
    p = app_str(p, "rc1="); p = app_i32(p, rc1);
    p = app_str(p, "  rc2="); app_i32(p, rc2);
    lg(buf);
    wait_ms(7000, "listening");
    lg_warn("tap to return to menu");
}

/* ---- T5: loop flag / chain hunt ----------------------------------------- *
 * The risky screen. Each probe is a separate deliberate tap, and every one
 * is traced before it runs, so a reboot still tells us which value did it. */

#define T5_BTN_H  36
#define T5_BTN_W  110
#define T5_COL_A  4
#define T5_COL_B  (T5_COL_A + T5_BTN_W + 6)

static uint8_t g_t5_mode = 1;
static uint8_t g_t5_flags = 0;

static void t5_draw(void)
{
    hb_fill_screen(HB_BLACK);
    hb_draw_str(4, 2, "T5 LOOP / CHAIN", 2, HB_CYAN, HB_BLACK);

    char buf[40], *p = buf;
    p = app_str(p, "PLAYMODE(0x51)=0x"); p = app_hex8(p, g_t5_mode);
    p = app_str(p, "  FLAGS(0x52)=0x"); app_hex8(p, g_t5_flags);
    hb_draw_str(4, 26, buf, 1, HB_WHITE, HB_BLACK);
    hb_draw_str(4, 38, "seam1s.wav (200/210Hz, 1.000s)", 1,
                HB_RGB(0x80, 0x80, 0x80), HB_BLACK);
    hb_draw_str(4, 50, "loops cleanly if it repeats", 1,
                HB_RGB(0x80, 0x80, 0x80), HB_BLACK);

    int16_t y = 70;
    hb_ui_button_draw(T5_COL_A, y, T5_BTN_W, T5_BTN_H, "MODE-",
                      HB_RGB(0x30, 0x30, 0x50), HB_WHITE);
    hb_ui_button_draw(T5_COL_B, y, T5_BTN_W, T5_BTN_H, "MODE+",
                      HB_RGB(0x30, 0x30, 0x50), HB_WHITE);
    y += T5_BTN_H + 6;
    hb_ui_button_draw(T5_COL_A, y, T5_BTN_W, T5_BTN_H, "FLAG<<",
                      HB_RGB(0x30, 0x30, 0x50), HB_WHITE);
    hb_ui_button_draw(T5_COL_B, y, T5_BTN_W, T5_BTN_H, "FLAG=0",
                      HB_RGB(0x30, 0x30, 0x50), HB_WHITE);
    y += T5_BTN_H + 6;
    hb_ui_button_draw(T5_COL_A, y, T5_BTN_W * 2 + 6, T5_BTN_H, "PLAY WITH THESE",
                      HB_RGB(0x00, 0x60, 0x00), HB_WHITE);
    y += T5_BTN_H + 6;
    hb_ui_button_draw(T5_COL_A, y, T5_BTN_W * 2 + 6, T5_BTN_H, "CHAIN A->B",
                      HB_RGB(0x00, 0x40, 0x60), HB_WHITE);
    y += T5_BTN_H + 6;
    hb_ui_button_draw(T5_COL_A, y, T5_BTN_W * 2 + 6, T5_BTN_H, "SELF CHAIN A->A",
                      HB_RGB(0x60, 0x30, 0x00), HB_WHITE);
    y += T5_BTN_H + 6;
    hb_ui_button_draw(T5_COL_A, y, T5_BTN_W * 2 + 6, T5_BTN_H, "SILENCE / CUT OFF",
                      HB_RGB(0x60, 0x00, 0x00), HB_WHITE);
    y += T5_BTN_H + 6;
    hb_ui_button_draw(T5_COL_A, y, T5_BTN_W * 2 + 6, T5_BTN_H, "BACK",
                      HB_RGB(0x30, 0x30, 0x30), HB_WHITE);

    hb_draw_str(4, HB_SCREEN_H - 46,
                "self-chain may loop forever;", 1, HB_YELLOW, HB_BLACK);
    hb_draw_str(4, HB_SCREEN_H - 36,
                "SILENCE is the escape hatch.", 1, HB_YELLOW, HB_BLACK);
}

static void test_loop_chain(void)
{
    t5_draw();
    for (;;) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) return;
        if (e != HB_UI_TAP) { hb_ui_pace(); continue; }

        int16_t y = 70;
        step_ms_t s;
        if (hb_ui_button_hit(tx, ty, T5_COL_A, y, T5_BTN_W, T5_BTN_H)) {
            g_t5_mode--; t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_B, y, T5_BTN_W, T5_BTN_H)) {
            g_t5_mode++; t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_A, y + 42, T5_BTN_W, T5_BTN_H)) {
            g_t5_flags = g_t5_flags ? (uint8_t)(g_t5_flags << 1) : 1;
            t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_B, y + 42, T5_BTN_W, T5_BTN_H)) {
            g_t5_flags = 0; t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_A, y + 84, T5_BTN_W * 2 + 6, T5_BTN_H)) {
            hb_trace_log("SPK5", g_t5_mode, g_t5_flags);
            play_spike(g_desc_a, "seam1s.wav", g_t5_mode, g_t5_flags,
                       (void *)0, (void *)0, &s);
            wait_ms(6000, "loop?");
            t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_A, y + 126, T5_BTN_W * 2 + 6, T5_BTN_H)) {
            /* Load B first, then point A's NEXTSFX at it and play A. */
            hb_trace_log("SPK5", 0xC4A1, 0);
            chk("chain ctorB");
            sfx_ctor(g_desc_b);
            chk("chain loadB");
            sfx_load(g_desc_b, SPIKE_DIR "seam1s.wav", VOL_MAIN, 0, 0);
            chk("chain fldB");
            sfx_fields(g_desc_b, 0x5000, 1, 0, (void *)0);
            chk("chain A");
            play_spike(g_desc_a, "seam1s.wav", 1, 0, g_desc_b, (void *)0, &s);
            hb_trace_log("SPK5", 0xC4A2, 0);
            wait_ms(6000, "chained?");
            t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_A, y + 168, T5_BTN_W * 2 + 6, T5_BTN_H)) {
            hb_trace_log("SPK5", 0x5E1F, 0);
            play_spike(g_desc_a, "seam1s.wav", 1, 0, g_desc_a, (void *)0, &s);
            hb_trace_log("SPK5", 0x5E20, 0);
            wait_ms(10000, "self-loop?");
            t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_A, y + 210, T5_BTN_W * 2 + 6, T5_BTN_H)) {
            play_spike(g_desc_b, "t1s.wav", 1, 0, (void *)0, (void *)0, &s);
            t5_draw();
        } else if (hb_ui_button_hit(tx, ty, T5_COL_A, y + 252, T5_BTN_W * 2 + 6, T5_BTN_H)) {
            return;
        }
        hb_ui_pace();
    }
}

/* ---- T6: per-step latency ------------------------------------------------ */

static void latency_row(const char *file)
{
    step_ms_t s;
    int rc = play_spike(g_desc_a, file, 1, 0, (void *)0, (void *)0, &s);
    char buf[40], *p = buf;
    p = app_str(p, file);
    while ((int)(p - buf) < 13) *p++ = ' ';
    *p = 0;
    p = app_str(p, "rc="); p = app_i32(p, rc);
    lg(buf);
    p = buf;
    p = app_str(p, "  ctor="); p = app_u32(p, s.ctor_ms);
    p = app_str(p, " load="); p = app_u32(p, s.load_ms);
    p = app_str(p, " fld="); p = app_u32(p, s.fields_ms);
    p = app_str(p, " play="); app_u32(p, s.play_ms);
    lg(buf);
    hb_trace_log("SPK6", s.load_ms, s.play_ms);
    wait_ms(2500, "settle");
}

static void test_latency(void)
{
    log_clear("T6 LATENCY");
    lg("Per-step cost, small vs large.");
    lg("If load scales with file size,");
    lg("the loader reads the whole file");
    lg("(confirmed against heap in T7).");
    lg("");
    latency_row("t1s.wav");
    latency_row("t5s.wav");
    latency_row("sz04m.wav");
    lg("");
    lg("call->first sample is NOT");
    lg("directly observable; use the");
    lg("T3 first-change time as the");
    lg("best proxy.");
    lg_warn("tap to return to menu");
}

/* ---- T7: does loadFile buffer the whole file? ---------------------------- */

/* Static RE of SoundEffectDescriptor::loadFile (VA 0x08417f78) shows a hard
 * reject of any load over 1 MiB. These bracket that boundary: sz1023k should
 * load, sz1025k should come back rc=1. If it does, the constant is confirmed
 * and the whole preset budget follows from it. */
static const char *SIZE_CASES[] = {
    "t1s.wav", "szhalf.wav", "sz0900k.wav", "sz1023k.wav", "sz1025k.wav",
    "sz04m.wav", "loop783.wav"
};
#define N_SIZE_CASES ((int)(sizeof SIZE_CASES / sizeof SIZE_CASES[0]))

static void test_size(void)
{
    log_clear("T7 SIZE + HEAP");
    lg("heap delta ~ file size means");
    lg("the whole WAV lands in RAM;");
    lg("a small fixed delta means it");
    lg("streams off disk.");
    lg("");

    char buf[48];
    char *p = buf;
    p = app_str(p, "heap free "); p = app_u32(p, hb_os_heap_free() >> 10);
    p = app_str(p, "K largest "); p = app_u32(p, hb_os_heap_largest() >> 10);
    app_str(p, "K");
    lg(buf);
    lg("");

    for (int i = 0; i < N_SIZE_CASES; i++) {
        char path[96], *q = path;
        q = app_str(q, SPIKE_DIR);
        app_str(q, SIZE_CASES[i]);
        uint32_t fsz = hb_fs_size(path);
        if (!fsz) {
            p = buf;
            p = app_str(p, SIZE_CASES[i]);
            app_str(p, "  (not on device)");
            lg_warn(buf);
            continue;
        }

        uint32_t before = hb_os_heap_free();
        chk("size ctor");
        sfx_ctor(g_desc_a);
        chk("size load");
        uint32_t t0 = hb_time_uptime_ms();
        int rc = sfx_load(g_desc_a, path, VOL_MAIN, 0, 0);
        uint32_t dt = hb_time_uptime_ms() - t0;
        chk("size done");
        uint32_t after = hb_os_heap_free();

        hb_trace_log("SPK7", fsz >> 10, (uint32_t)rc);

        p = buf;
        p = app_str(p, SIZE_CASES[i]);
        while ((int)(p - buf) < 13) *p++ = ' ';
        *p = 0;
        p = app_u32(p, fsz >> 10);
        p = app_str(p, "K rc="); p = app_i32(p, rc);
        lg(buf);

        p = buf;
        p = app_str(p, "  load="); p = app_u32(p, dt);
        p = app_str(p, "ms heapd=");
        int32_t delta = (int32_t)(before - after);
        p = app_i32(p, delta / 1024);
        app_str(p, "K");
        lg(buf);
    }
    lg("");
    lg_warn("tap to return to menu");
}

/* ---- T8: can loadFile play a window of a bigger file? ------------------- */

static void test_subrange(void)
{
    log_clear("T8 SUBRANGE");
    lg("loadFile takes (offset,size) —");
    lg("the SDK always passes 0,0. If a");
    lg("window plays, one big rendered");
    lg("file can hold every segment and");
    lg("we seek instead of re-render.");
    lg("");
    lg("sz0900k.wav, 44-byte header,");
    lg("44100*4 B/s. Window = 2s in,");
    lg("2s long.");
    lg("");

    const uint32_t hdr = 44;
    const uint32_t bps = 44100u * 4u;
    uint32_t off = hdr + bps * 2u;
    uint32_t len = bps * 2u;

    chk("sub ctor");
    sfx_ctor(g_desc_a);
    chk("sub load");
    int rc = sfx_load(g_desc_a, SPIKE_DIR "sz0900k.wav", VOL_MAIN, off, len);
    chk("sub fields");
    hb_trace_log("SPK8", off, (uint32_t)rc);

    char buf[40], *p = buf;
    p = app_str(p, "rc="); p = app_i32(p, rc);
    p = app_str(p, " off="); p = app_u32(p, off);
    lg(buf);

    if (rc == 0) {
        sfx_fields(g_desc_a, 0x5000, 1, 0, (void *)0);
        chk("sub play");
        sfx_play(g_desc_a, (void *)0, (void *)0);
        lg("playing... 5s of tone means");
        lg("the window worked. Silence or");
        lg("noise means offset/size are");
        lg("not a PCM byte range.");
        wait_ms(8000, "subrange");
    } else {
        lg("rejected: offset/size are not");
        lg("a usable seek. Segments must");
        lg("be separate files.");
    }
    lg_warn("tap to return to menu");
}

/* ---- T9: PCM straight from RAM, no file ---------------------------------- *
 *
 * The descriptor is not just a handle to a file. loadFile fills in a decoded
 * PCM buffer pointer and a format, and sfxPlayer::play hands the descriptor to
 * a voice which reads those fields - the voice keeps the DESCRIPTOR pointer
 * (voice+0x78), not a copy of the audio.
 *
 * Read out of RetailOS 1.1.2: voice::setSource at VA 0x0862fd24, and the
 * container-type jump table it dispatches on (desc+0x10):
 *
 *   desc+0x04 / +0x08   decoded PCM buffer
 *   desc+0x0C           its length in bytes
 *   desc+0x10           container type. Type 0 computes framesPerPacket = 1
 *                       and bytesPerFrame = (bits/8) * channels, which is
 *                       linear PCM and nothing else
 *   desc+0x14/18/1C     sample rate, channels, bits
 *   desc+0x38 / +0x3C   leading/trailing trim, subtracted from the frame count
 *
 * If that reading is right, filling those in by hand plays arbitrary PCM with
 * no file, no FAT write, and no 1 MiB loader ceiling - and if the voice reads
 * the buffer as it goes rather than copying it, double-buffering underneath a
 * playing voice gives real streaming.
 *
 * This is the test that decides whether Entrain can generate audio live on
 * RetailOS instead of rendering files first. */

#define T9_RATE     22050u
#define T9_SECONDS  3u
#define T9_FRAMES   (T9_RATE * T9_SECONDS)

static int16_t *g_t9_pcm;

/* A 220 / 227 Hz pair, one per ear, so a working result is unmistakable: two
   clear tones beating seven times a second.
 *
 * The first version of this used a hand-rolled "triangle" that was discontinuous
 * at every zero crossing - it jumped 32767 units each cycle, so it came out as a
 * harsh buzz and made a working audio path sound broken. Use a real triangle:
 * fold the top bit of the phase, which is continuous by construction. */
static void t9_fill(int16_t *dst, uint32_t frames, uint32_t rate,
                    uint32_t phase_l_start, uint32_t phase_r_start)
{
    uint32_t pl = phase_l_start, pr = phase_r_start;
    uint32_t step_l = (uint32_t)(220.0 * 4294967296.0 / (double)rate);
    uint32_t step_r = (uint32_t)(227.0 * 4294967296.0 / (double)rate);

    for (uint32_t i = 0; i < frames; i++) {
        /* phase -> 0..65535 -> triangle -32768..32767, no discontinuity */
        uint32_t a = (pl >> 15) & 0x1FFFF;      /* 0..131071, two ramps */
        int32_t  l = (a < 65536u) ? (int32_t)a - 32768 : 98303 - (int32_t)a;
        uint32_t b = (pr >> 15) & 0x1FFFF;
        int32_t  r = (b < 65536u) ? (int32_t)b - 32768 : 98303 - (int32_t)b;

        dst[2 * i + 0] = (int16_t)(l / 2);      /* half scale, leave headroom */
        dst[2 * i + 1] = (int16_t)(r / 2);
        pl += step_l;
        pr += step_r;
    }
}

static void test_raw_pcm(void)
{
    log_clear("T9 PCM FROM RAM");
    lg("No file at all: allocate a buffer,");
    lg("fill it, point the descriptor at it,");
    lg("and play.");
    lg("");
    lg("If this works the render-to-WAV path");
    lg("goes away and streaming is possible.");
    lg("");

    uint32_t bytes = T9_FRAMES * 4u;

    char buf[48], *p = buf;
    p = app_str(p, "need "); p = app_u32(p, bytes >> 10);
    p = app_str(p, "K, largest free ");
    p = app_u32(p, hb_os_heap_largest() >> 10);
    app_str(p, "K");
    lg(buf);

    if (!g_t9_pcm) g_t9_pcm = (int16_t *)hb_os_alloc(bytes);
    if (!g_t9_pcm) {
        lg_warn("alloc failed - not enough heap");
        lg_warn("tap to return to menu");
        return;
    }
    t9_fill(g_t9_pcm, T9_FRAMES, T9_RATE, 0, 0);
    hb_trace_log("SPK9", (uint32_t)(uintptr_t)g_t9_pcm, bytes);

    chk("t9 ctor");
    sfx_ctor(g_desc_a);

    /* Everything loadFile would have filled in, filled in by hand. */
    chk("t9 fields");
    *(volatile uint32_t *)(g_desc_a + 0x04) = (uint32_t)(uintptr_t)g_t9_pcm;
    *(volatile uint32_t *)(g_desc_a + 0x08) = (uint32_t)(uintptr_t)g_t9_pcm;
    *(volatile uint32_t *)(g_desc_a + 0x0C) = bytes;
    g_desc_a[0x10] = 0;                                   /* linear PCM */
    *(volatile uint32_t *)(g_desc_a + 0x14) = T9_RATE;
    *(volatile uint32_t *)(g_desc_a + 0x18) = 2;          /* channels */
    *(volatile uint32_t *)(g_desc_a + 0x1C) = 16;         /* bits */
    *(volatile uint32_t *)(g_desc_a + 0x38) = 0;          /* no leading trim */
    *(volatile uint32_t *)(g_desc_a + 0x3C) = 0;          /* no trailing trim */
    sfx_fields(g_desc_a, 0x5000, 1, 0, (void *)0);

    chk("t9 play");
    bool ok = sfx_play(g_desc_a, (void *)0, (void *)0);
    hb_trace_log("SPK9", 0x9AA9, ok ? 1u : 0u);

    p = buf;
    p = app_str(p, "play() returned ");
    app_str(p, ok ? "true" : "FALSE");
    lg(buf);
    lg("");
    lg_warn("LISTEN: two tones, one per ear,");
    lg_warn("beating 7 times a second, 3s.");
    lg("");
    lg("silence -> the voice does not read");
    lg("  desc+0x04, or type 0 is not LPCM");
    lg("noise   -> right path, wrong format");
    lg("  fields (try bits / channels)");

    wait_ms(6000, "t9 playing");

    /* The streaming question: refill the SAME buffer with a different phase and
       play again without touching the pointer. If the voice reads the buffer
       live rather than copying it at play time, swapping underneath a playing
       voice will work too, which is what real streaming needs. */
    lg("");
    lg("refilled same buffer, playing again");
    t9_fill(g_t9_pcm, T9_FRAMES, T9_RATE, 0x40000000u, 0);
    chk("t9 replay");
    sfx_play(g_desc_a, (void *)0, (void *)0);
    wait_ms(5000, "t9 replay");

    lg_warn("tap to return to menu");
}

/* ---- T10: watch the voice while it plays ---------------------------------- *
 *
 * Everything about how Entrain joins buffers is currently guesswork, and it
 * sounds like it. Three things would settle it, and all three are readable
 * from the voice object:
 *
 *   voice::setSource (VA 0x0862fd24) writes the voice pointer into desc+0x48,
 *   so after play() we have the object. It also sets voice+0x50 to the total
 *   playable frames, and voice::start (VA 0x08630154) sets voice+0x5C to 1.
 *
 *   1. Is there a read cursor? A field that counts up in step with playback
 *      would let the next buffer be started at exactly the right sample
 *      instead of whenever the UI tick happens to land - which is what the
 *      crossfades are currently papering over.
 *   2. Is a finished voice released? We start one a second and each plays for
 *      two, so if they are never freed the pool of eight runs out after eight
 *      seconds and the allocator starts stealing one that is still sounding.
 *      That would click exactly the way it does.
 *   3. Are voice+0x48 / +0x4C loop points? setSource fills them from
 *      desc+0x34 and desc+0x30/+0x5C, converted from milliseconds to samples.
 *      If they are loop bounds, a steady preset needs no joins at all.
 *
 * This plays four seconds from RAM and samples the voice's first 0x100 bytes
 * ten times a second, then reports every word that changed, how it changed,
 * and whether it moved monotonically. */

#define T10_RATE    22050u
#define T10_SECONDS 4u
#define T10_FRAMES  (T10_RATE * T10_SECONDS)
#define T10_WORDS   0x40            /* 0x100 bytes of the voice struct */
#define T10_SAMPLES 40

static int16_t *g_t10_pcm;
static uint32_t g_t10_first[T10_WORDS];
static uint32_t g_t10_last[T10_WORDS];
static uint32_t g_t10_changes[T10_WORDS];
static uint8_t  g_t10_monotonic[T10_WORDS];

static void test_voice_watch(void)
{
    log_clear("T10 VOICE WATCH");
    lg("4s from RAM, sampling the voice");
    lg("object 10x a second.");
    lg("");

    uint32_t bytes = T10_FRAMES * 4u;
    if (!g_t10_pcm) g_t10_pcm = (int16_t *)hb_os_alloc(bytes);
    if (!g_t10_pcm) {
        lg_warn("alloc failed - not enough heap");
        lg_warn("tap to return to menu");
        return;
    }
    t9_fill(g_t10_pcm, T10_FRAMES, T10_RATE, 0, 0);

    chk("t10 ctor");
    sfx_ctor(g_desc_a);
    chk("t10 fields");
    *(volatile uint32_t *)(g_desc_a + 0x04) = (uint32_t)(uintptr_t)g_t10_pcm;
    *(volatile uint32_t *)(g_desc_a + 0x08) = (uint32_t)(uintptr_t)g_t10_pcm;
    *(volatile uint32_t *)(g_desc_a + 0x0C) = bytes;
    g_desc_a[0x10] = 0;
    *(volatile uint32_t *)(g_desc_a + 0x14) = T10_RATE;
    *(volatile uint32_t *)(g_desc_a + 0x18) = 2;
    *(volatile uint32_t *)(g_desc_a + 0x1C) = 16;
    *(volatile uint32_t *)(g_desc_a + 0x38) = 0;
    *(volatile uint32_t *)(g_desc_a + 0x3C) = 0;
    sfx_fields(g_desc_a, 0x5000, 1, 0, (void *)0);

    chk("t10 play");
    if (!sfx_play(g_desc_a, (void *)0, (void *)0)) {
        lg_warn("play() failed");
        lg_warn("tap to return to menu");
        return;
    }

    /* setSource stores the voice here. If it is still zero, the descriptor
       never reached a voice and nothing else below means anything. */
    uint32_t voice = *(volatile uint32_t *)(g_desc_a + 0x48);
    char buf[48], *p = buf;
    p = app_str(p, "voice = 0x");
    for (int sh = 28; sh >= 0; sh -= 4)
        *p++ = HEXD[(voice >> sh) & 0xF];
    *p = 0;
    lg(buf);
    hb_trace_log("SPKA", voice, T10_FRAMES);

    if (voice < 0x08000000u || voice >= 0x0A000000u) {
        lg_warn("not a sane pointer - stopping");
        lg_warn("tap to return to menu");
        return;
    }

    volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)voice;
    for (int i = 0; i < T10_WORDS; i++) {
        g_t10_first[i] = v[i];
        g_t10_last[i] = v[i];
        g_t10_changes[i] = 0;
        g_t10_monotonic[i] = 1;
    }

    for (int s = 0; s < T10_SAMPLES; s++) {
        wait_ms(100, "watching");
        for (int i = 0; i < T10_WORDS; i++) {
            uint32_t now = v[i];
            if (now != g_t10_last[i]) {
                g_t10_changes[i]++;
                if (now < g_t10_last[i]) g_t10_monotonic[i] = 0;
                g_t10_last[i] = now;
            }
        }
    }

    lg("");
    lg("off  first -> last   n  mono");
    int shown = 0;
    for (int i = 0; i < T10_WORDS && shown < 16; i++) {
        if (!g_t10_changes[i]) continue;
        p = buf;
        *p++ = '+';
        p = app_hex8(p, (uint8_t)(i * 4));
        p = app_str(p, " ");
        p = app_u32(p, g_t10_first[i]);
        p = app_str(p, ">");
        p = app_u32(p, g_t10_last[i]);
        p = app_str(p, " ");
        p = app_u32(p, g_t10_changes[i]);
        p = app_str(p, g_t10_monotonic[i] ? " UP" : " --");
        lg(buf);
        hb_trace_log("SPKA", (uint32_t)(i * 4), g_t10_last[i]);
        shown++;
    }
    if (!shown) lg_warn("nothing in the voice moved at all");

    lg("");
    lg("a field rising to about");
    p = buf; p = app_u32(p, T10_FRAMES); app_str(p, " is the read cursor.");
    lg(buf);

    /* After the buffer has finished: is the voice released, or still marked
       busy? If it stays busy, the pool of eight runs out and the allocator
       starts stealing voices that are still sounding. */
    wait_ms(2000, "after end");
    p = buf;
    p = app_str(p, "after end +5C = ");
    p = app_u32(p, v[0x5C / 4]);
    p = app_str(p, "  +50 = ");
    app_u32(p, v[0x50 / 4]);
    lg(buf);
    lg("+5C back to 0 means released.");
    hb_trace_log("SPKA", 0xEEEE, v[0x5C / 4]);

    lg_warn("tap to return to menu");
}

/* ---- menu ---------------------------------------------------------------- */

typedef void (*test_fn_t)(void);

typedef struct {
    const char *label;
    test_fn_t   fn;
} menu_item_t;

static const menu_item_t MENU[] = {
    { "T0  L/R SEPARATION", test_lr },
    { "T1  FORMAT MATRIX",  test_formats },
    { "T2  BLOCKING+RATE",  test_blocking },
    { "T3  DESC WATCH",     test_desc_watch },
    { "T3b COMPLETION CB",  test_callback },
    { "T4  OVERLAP",        test_overlap },
    { "T5  LOOP / CHAIN",   test_loop_chain },
    { "T6  LATENCY",        test_latency },
    { "T7  SIZE + HEAP",    test_size },
    { "T8  SUBRANGE",       test_subrange },
    { "T9  PCM FROM RAM",   test_raw_pcm },
    { "T10 VOICE WATCH",    test_voice_watch },
};
#define N_MENU ((int)(sizeof MENU / sizeof MENU[0]))

#define MENU_TOP 30
#define MENU_H   34
#define MENU_GAP 2
#define MENU_X   4
#define MENU_W   (HB_SCREEN_W - 8)

static void menu_draw(void)
{
    hb_fill_screen(HB_BLACK);
    hb_draw_str(4, 2, "AUDIO SPIKE", 2, HB_CYAN, HB_BLACK);
    hb_draw_str(150, 8, "/WAV/spike", 1, HB_RGB(0x70, 0x70, 0x70), HB_BLACK);
    for (int i = 0; i < N_MENU; i++) {
        int16_t y = (int16_t)(MENU_TOP + i * (MENU_H + MENU_GAP));
        hb_ui_button_draw(MENU_X, y, MENU_W, MENU_H, MENU[i].label,
                          HB_RGB(0x18, 0x18, 0x22), HB_WHITE);
    }
    hb_draw_str(4, HB_SCREEN_H - 14, "headphones on. VOL/HOME = exit", 1,
                HB_RGB(0x80, 0x80, 0x80), HB_BLACK);
}

static void wait_for_tap(void)
{
    for (;;) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_TAP || e == HB_UI_EXIT) return;
        hb_ui_pace();
    }
}

static void *app_main(void *arg)
{
    (void)arg;
    hb_trace_init();
    hb_trace_log("SPKB", 0, 0);
    hb_ui_init();
    menu_draw();

    for (;;) {
        int16_t tx, ty;
        hb_ui_event_t e = hb_ui_poll(&tx, &ty);
        if (e == HB_UI_EXIT) break;
        if (e == HB_UI_TAP) {
            for (int i = 0; i < N_MENU; i++) {
                int16_t y = (int16_t)(MENU_TOP + i * (MENU_H + MENU_GAP));
                if (hb_ui_button_hit(tx, ty, MENU_X, y, MENU_W, MENU_H)) {
                    hb_trace_log("SPKR", (uint32_t)i, 0);
                    MENU[i].fn();
                    if (MENU[i].fn != test_loop_chain) wait_for_tap();
                    menu_draw();
                    break;
                }
            }
        }
        hb_ui_pace();
    }

    hb_ui_done();
    hb_trace_log("SPKX", 0, 0);
    return (void *)0;
}

/* loadFile needs ~21 KB of stack, so the whole harness runs in its own
 * 64 KB pthread — same reason hb_audio.c spawns one. */
HB_APP_ENTRY(payload_entry)
{
    static uint32_t attr[16];
    for (int i = 0; i < 16; i++) attr[i] = 0;
    attr[0] = 0x50544841u;   /* 'PTHA' */
    attr[2] = 2;
    attr[4] = 0x10000;       /* 64 KB stack */
    attr[6] = 1;
    attr[7] = 1;
    attr[8] = 1;
    uint32_t tid = 0;
    ((pthread_create_t)PTHREAD_CREATE_ADDR)(&tid, attr, app_main, (void *)0);
}

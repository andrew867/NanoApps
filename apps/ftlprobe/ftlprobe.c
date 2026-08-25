/*
 * ftlprobe — RAW surface probe for N31 FTL bring-up.
 *
 * Shows a status screen and writes /Apps/Data/Ftlprobe/report.txt with
 * a string-probe of RetailOS ("sftl: lba mismatch") plus a few DRAM peeks
 * around the classic 1.0.2 L2V sites so we can see if 1.1.2 still lines up.
 *
 * Tap anywhere to re-run the probe. Home to exit.
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FG      0xe8eef8
#define BG      0x0a0e1a
#define ACCENT  0x0da5f5
#define OKCOL   0x33c059
#define BADCOL  0xe65d53
#define DIM     0x69809d

#define DATA_DIR "/Apps/Data/Ftlprobe"
#define REPORT   DATA_DIR "/report.txt"

/* Classic 1.0.2 VAs (Image1 @ 0x08000000). On 1.1.2 these may be junk. */
#define VA_L2V_SEARCH  0x08428694u
#define VA_S_READ      0x0856AB08u
#define VA_CURSOR      0x08D0F864u

static int s_dirty = 1;
static int s_ran;
static int s_sftl_hit;
static uint32_t s_w0, s_w1, s_w2;
static int s_prev_down;

static void u32_hex(char *out, uint32_t v)
{
    static const char *H = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = H[(v >> (28 - 4 * i)) & 0xf];
    out[10] = 0;
}

/* Blit 8x8 glyphs into the raw framebuffer via hb_draw_* is MIPI-direct;
 * for surface apps we stamp a tiny monochrome readout with filled rects
 * representing status, plus hb_draw_str which still paints something useful
 * on many builds. Prefer file report as source of truth. */

static int probe_sftl_string(void)
{
    const uint8_t *p = (const uint8_t *)0x087A0000u;
    const uint8_t *end = (const uint8_t *)0x087C0000u;
    const char *needle = "sftl: lba mismatch";
    const uint32_t nlen = 18;
    for (; p + nlen < end; p++) {
        uint32_t i;
        for (i = 0; i < nlen; i++) {
            if (p[i] != (uint8_t)needle[i])
                break;
        }
        if (i == nlen)
            return 1;
    }
    return 0;
}

static void run_probe(void)
{
    s_sftl_hit = probe_sftl_string();
    s_w0 = *(volatile uint32_t *)VA_L2V_SEARCH;
    s_w1 = *(volatile uint32_t *)VA_S_READ;
    s_w2 = *(volatile uint32_t *)VA_CURSOR;

    hb_fs_mkdir(DATA_DIR);

    char buf[512];
    char h0[12], h1[12], h2[12];
    u32_hex(h0, s_w0);
    u32_hex(h1, s_w1);
    u32_hex(h2, s_w2);

    /* Keep it ASCII and short — easy to open from Windows. */
    int n = 0;
    const char *hdr = "ftlprobe report (RetailOS 1.1.2 untethered)\n";
    while (hdr[n]) { buf[n] = hdr[n]; n++; }
    const char *a = "sftl_string=";
    for (int i = 0; a[i]; i++) buf[n++] = a[i];
    buf[n++] = s_sftl_hit ? '1' : '0';
    buf[n++] = '\n';

    const char *b = "peek_L2V_Search@08428694=";
    for (int i = 0; b[i]; i++) buf[n++] = b[i];
    for (int i = 0; h0[i]; i++) buf[n++] = h0[i];
    buf[n++] = '\n';

    const char *c = "peek_s_read@0856AB08=";
    for (int i = 0; c[i]; i++) buf[n++] = c[i];
    for (int i = 0; h1[i]; i++) buf[n++] = h1[i];
    buf[n++] = '\n';

    const char *d = "peek_cursor@08D0F864=";
    for (int i = 0; d[i]; i++) buf[n++] = d[i];
    for (int i = 0; h2[i]; i++) buf[n++] = h2[i];
    buf[n++] = '\n';
    buf[n] = 0;

    hb_fs_write(REPORT, buf, (uint32_t)n);
    s_ran = 1;
    s_dirty = 1;
}

static void draw_ui(void)
{
    int w = hb_raw_w();
    hb_raw_fill(BG);
    hb_raw_fill_rect(0, 0, w, 48, ACCENT);

    /* Title bar accent only — readable text via report file + on-screen blocks. */
    hb_raw_fill_rect(16, 14, 80, 8, 0xffffff);   /* "FTL" bar */
    hb_raw_fill_rect(104, 14, 48, 8, 0xffffff);

    hb_raw_fill_rect(16, 72, w - 32, 56, 0x1a1f2e);
    hb_raw_fill_rect(24, 88, 12, 24, s_ran ? (s_sftl_hit ? OKCOL : BADCOL) : DIM);
    hb_raw_fill_rect(44, 92, 140, 8, FG);
    hb_raw_fill_rect(44, 108, 100, 6, DIM);

    hb_raw_fill_rect(16, 144, w - 32, 100, 0x1a1f2e);
    /* three peek rows as color chips */
    hb_raw_fill_rect(24, 160, 20, 20, ACCENT);
    hb_raw_fill_rect(52, 164, (int)(s_w0 & 0xff), 12, FG);

    hb_raw_fill_rect(24, 192, 20, 20, ACCENT);
    hb_raw_fill_rect(52, 196, (int)(s_w1 & 0xff), 12, FG);

    hb_raw_fill_rect(24, 224, 20, 20, ACCENT);
    hb_raw_fill_rect(52, 228, (int)(s_w2 & 0xff), 12, FG);

    hb_raw_fill_rect(16, 270, w - 32, 40, 0x1a1f2e);
    hb_raw_fill_rect(24, 284, 180, 8, DIM);

    hb_raw_fill_rect(16, 380, w - 32, 36, s_ran ? OKCOL : ACCENT);
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_prev_down = 0;
    run_probe();
    draw_ui();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    int down = touch && touch->down;
    if (down && !s_prev_down)
        run_probe();
    s_prev_down = down;

    if (s_dirty) {
        draw_ui();
        s_dirty = 0;
    }
}

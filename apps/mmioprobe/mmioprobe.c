/*
 * mmioprobe — dump audio / DMA / clock MMIO to FAT while USB is unplugged.
 *
 * RetailOS will not play music with USB data connected, so host SCSI peeks
 * cannot capture a live "playing" IIS0/PL080 snapshot. Run this app with the
 * cable out, music (or FM) active if possible, tap to dump, then reconnect
 * and pull /Apps/Data/Mmioprobe/.
 *
 * Tap = re-dump. Home = exit.
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FG     0xe8eef8
#define BG     0x0a0e1a
#define ACCENT 0xf59e0b
#define OKCOL  0x33c059
#define DIM    0x69809d

#define DATA_DIR "/Apps/Data/Mmioprobe"
#define MANIFEST DATA_DIR "/MANIFEST.txt"

typedef struct {
	const char *name;
	uint32_t base;
	uint32_t size; /* bytes, multiple of 4, ≤ 0x200 */
} win_t;

/* Canonical N31 RE / DTS bases (39A / 1.1.2-family CFW). */
static const win_t WINDOWS[] = {
	{ "CLKCON",    0x3C500000u, 0x100 },
	{ "IIS0_ASP",  0x3CA00000u, 0x100 },
	{ "IIS1_XSP",  0x3CD00000u, 0x100 },
	{ "IIS2_FM",   0x3D400000u, 0x100 },
	{ "SPI0_CS42", 0x3C300000u, 0x080 },
	{ "SPI2_NIMBUS", 0x3D200000u, 0x040 },
	{ "PL080_0",   0x38200000u, 0x100 },
	{ "PL080_1",   0x38700000u, 0x100 },
	{ "UART1_BCM", 0x3DB00000u, 0x080 },
	{ "IIC1_PMIC", 0x3C900000u, 0x040 },
};

static int s_dirty = 1;
static int s_ran;
static int s_ok;
static int s_nonzero_iis0;
static int s_prev_down;
static uint32_t s_iis0_w0;

static void u32_hex(char *out, uint32_t v)
{
	static const char *H = "0123456789abcdef";
	out[0] = '0';
	out[1] = 'x';
	for (int i = 0; i < 8; i++)
		out[2 + i] = H[(v >> (28 - 4 * i)) & 0xf];
	out[10] = 0;
}

static void append(char *buf, int *n, int cap, const char *s)
{
	while (*s && *n < cap - 1)
		buf[(*n)++] = *s++;
	buf[*n] = 0;
}

static void dump_window(const win_t *w, uint8_t *scratch)
{
	volatile uint32_t *p = (volatile uint32_t *)w->base;
	uint32_t words = w->size / 4;
	uint32_t i;
	for (i = 0; i < words; i++) {
		uint32_t v = p[i];
		scratch[i * 4 + 0] = (uint8_t)(v & 0xff);
		scratch[i * 4 + 1] = (uint8_t)((v >> 8) & 0xff);
		scratch[i * 4 + 2] = (uint8_t)((v >> 16) & 0xff);
		scratch[i * 4 + 3] = (uint8_t)((v >> 24) & 0xff);
	}

	char path[96];
	int n = 0;
	append(path, &n, (int)sizeof(path), DATA_DIR "/");
	append(path, &n, (int)sizeof(path), w->name);
	append(path, &n, (int)sizeof(path), ".bin");
	hb_fs_write(path, scratch, w->size);
}

static void run_probe(void)
{
	uint8_t scratch[0x200];
	char man[768];
	int n = 0;
	int i;
	int all_ok = 1;

	hb_fs_mkdir(DATA_DIR);

	s_iis0_w0 = *(volatile uint32_t *)0x3CA00000u;
	s_nonzero_iis0 = 0;

	for (i = 0; i < (int)(sizeof(WINDOWS) / sizeof(WINDOWS[0])); i++) {
		dump_window(&WINDOWS[i], scratch);
		if (WINDOWS[i].base == 0x3CA00000u) {
			uint32_t words = WINDOWS[i].size / 4;
			uint32_t j;
			for (j = 0; j < words; j++) {
				uint32_t v = scratch[j * 4] |
					     ((uint32_t)scratch[j * 4 + 1] << 8) |
					     ((uint32_t)scratch[j * 4 + 2] << 16) |
					     ((uint32_t)scratch[j * 4 + 3] << 24);
				if (v)
					s_nonzero_iis0 = 1;
			}
		}
	}

	append(man, &n, (int)sizeof(man),
	       "mmioprobe N31 RetailOS (USB-unplugged oracle)\n");
	append(man, &n, (int)sizeof(man),
	       "note=play music or FM before tap if possible; open app may pause audio\n");

	{
		char h[12];
		u32_hex(h, s_iis0_w0);
		append(man, &n, (int)sizeof(man), "IIS0_ASP+0=");
		append(man, &n, (int)sizeof(man), h);
		append(man, &n, (int)sizeof(man), "\n");
	}
	append(man, &n, (int)sizeof(man), "IIS0_ASP_nonzero=");
	man[n++] = s_nonzero_iis0 ? '1' : '0';
	man[n++] = '\n';

	for (i = 0; i < (int)(sizeof(WINDOWS) / sizeof(WINDOWS[0])); i++) {
		char h[12];
		append(man, &n, (int)sizeof(man), WINDOWS[i].name);
		append(man, &n, (int)sizeof(man), " ");
		u32_hex(h, WINDOWS[i].base);
		append(man, &n, (int)sizeof(man), h);
		append(man, &n, (int)sizeof(man), " ");
		u32_hex(h, WINDOWS[i].size);
		append(man, &n, (int)sizeof(man), h);
		append(man, &n, (int)sizeof(man), "\n");
	}

	if (!hb_fs_write(MANIFEST, man, (uint32_t)n))
		all_ok = 0;

	s_ok = all_ok;
	s_ran = 1;
	s_dirty = 1;
}

static void draw_ui(void)
{
	int w = hb_raw_w();
	hb_raw_fill(BG);
	hb_raw_fill_rect(0, 0, w, 48, ACCENT);
	hb_raw_fill_rect(16, 14, 120, 8, 0xffffff);
	hb_raw_fill_rect(144, 14, 48, 8, 0xffffff);

	hb_raw_fill_rect(16, 72, w - 32, 56, 0x1a1f2e);
	hb_raw_fill_rect(24, 88, 12, 24, s_ran ? (s_ok ? OKCOL : 0xe65d53) : DIM);
	hb_raw_fill_rect(44, 92, 160, 8, FG);
	hb_raw_fill_rect(44, 108, 120, 6, DIM);

	/* IIS0 activity chip — green if any non-zero word */
	hb_raw_fill_rect(16, 144, w - 32, 80, 0x1a1f2e);
	hb_raw_fill_rect(24, 160, 28, 28, s_nonzero_iis0 ? OKCOL : DIM);
	hb_raw_fill_rect(60, 168, (int)(s_iis0_w0 & 0xff) + 8, 12, FG);

	hb_raw_fill_rect(16, 240, w - 32, 48, 0x1a1f2e);
	hb_raw_fill_rect(24, 256, 200, 8, DIM);

	hb_raw_fill_rect(16, 380, w - 32, 36, s_ran ? OKCOL : ACCENT);
}

void hb_raw_init(int w, int h)
{
	(void)w;
	(void)h;
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

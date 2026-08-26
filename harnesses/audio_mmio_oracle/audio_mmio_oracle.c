/*
 * audio_mmio_oracle — force RetailOS audio path while USB/SCSI is live,
 * then snapshot IIS/PL080/CLKCON into DRAM for host peeks.
 *
 * Connected UI blocks the Music app, but it does not necessarily gate the
 * OS audio stack. This harness:
 *   1) resumes an existing media session (hb_media) if any
 *   2) plays Resources/Sounds/volumebeep.wav through hb_audio (SFX path)
 *   3) copies key MMIO windows to DRAM @ 0x09390000
 *
 * Host (after ./start run audio_mmio_oracle, while beep/track may still run):
 *   sudo python3 tools/linux-n31/scsi-mem.py dump 0x09390000 0x800 /tmp/aor.bin
 *   sudo python3 tools/linux-n31/scsi-mem.py probe audio
 *
 * Prefer: ./start eject first so Home is up; SCSI still works.
 */
#include "hb_sdk.h"

#define ORACLE_DRAM   0x09390000u
#define ORACLE_MAGIC  0x524F4141u  /* 'AAOR' LE */
#define ORACLE_VER    1u

#define WIN_SIZE      0x100u
#define N_WINS        6u

/* LE words in report after header */
#define HDR_WORDS     16u

typedef struct {
	uint32_t magic;
	uint32_t ver;
	uint32_t media_state;   /* hb_media_state() after resume attempt */
	uint32_t media_had;     /* 1 if session before resume */
	uint32_t sfx_ok;        /* 1 if play_now returned true */
	uint32_t iis0_nonzero;  /* 1 if any IIS0 word non-zero at snap */
	uint32_t flags;
	uint32_t reserved[9];
	/* then N_WINS * WIN_SIZE bytes of LE register dumps */
} aor_hdr_t;

static const uint32_t WIN_BASE[N_WINS] = {
	0x3C500000u, /* CLKCON */
	0x3CA00000u, /* IIS0 ASP */
	0x3CD00000u, /* IIS1 XSP */
	0x3D400000u, /* IIS2 FM */
	0x3C300000u, /* SPI0 CS42 */
	0x38200000u, /* PL080_0 */
};

static uint8_t g_sfx_desc[0x78];

static void copy_mmio(uint8_t *dst, uint32_t base, uint32_t nbytes)
{
	volatile uint32_t *p = (volatile uint32_t *)base;
	uint32_t words = nbytes / 4;
	uint32_t i;
	for (i = 0; i < words; i++) {
		uint32_t v = p[i];
		dst[i * 4 + 0] = (uint8_t)(v & 0xff);
		dst[i * 4 + 1] = (uint8_t)((v >> 8) & 0xff);
		dst[i * 4 + 2] = (uint8_t)((v >> 16) & 0xff);
		dst[i * 4 + 3] = (uint8_t)((v >> 24) & 0xff);
	}
}

static void store_report(const aor_hdr_t *hdr, const uint8_t *body, uint32_t body_n)
{
	volatile uint8_t *dst = (volatile uint8_t *)ORACLE_DRAM;
	const uint8_t *s = (const uint8_t *)hdr;
	uint32_t i;
	for (i = 0; i < sizeof(*hdr); i++)
		dst[i] = s[i];
	for (i = 0; i < body_n; i++)
		dst[sizeof(*hdr) + i] = body[i];
}

static void step_draw(const char *s)
{
	/* Required between hb_audio_* steps — scale-3 text wakes audio task. */
	hb_draw_str(8, 200, s, 3, HB_WHITE, HB_BLACK);
}

HB_APP_ENTRY(payload_entry)
{
	aor_hdr_t hdr;
	uint8_t body[N_WINS * WIN_SIZE];
	uint32_t i;
	int had;
	int st;
	int sfx_ok = 0;
	int iis0_nz = 0;

	hb_fill_screen(HB_BLACK);
	hb_draw_str(8, 8, "AUDIO MMIO ORACLE", 2, HB_YELLOW, HB_BLACK);
	hb_draw_str(8, 40, "DRAM @9390000", 1, HB_CYAN, HB_BLACK);
	hb_draw_str(8, 56, "resume+beep+snap", 1, HB_GREEN, HB_BLACK);

	/* Clear header */
	for (i = 0; i < sizeof(hdr) / 4; i++)
		((uint32_t *)&hdr)[i] = 0;
	hdr.magic = ORACLE_MAGIC;
	hdr.ver = ORACLE_VER;

	had = hb_media_has_session() ? 1 : 0;
	hdr.media_had = (uint32_t)had;
	if (had)
		hb_media_set_paused(false); /* resume / unpause */
	st = hb_media_state();
	hdr.media_state = (uint32_t)(st < 0 ? 0xffffffffu : (uint32_t)st);

	hb_draw_str(8, 80, had ? "media: resume" : "media: none", 1,
		    HB_WHITE, HB_BLACK);

	/* Force SFX path even under Connected — bypasses Music UI. */
	hb_audio_ctor(g_sfx_desc);
	step_draw("SFX1----");
	(void)hb_audio_loadfile(g_sfx_desc, "Resources/Sounds/volumebeep.wav", 4);
	step_draw("SFX2----");
	hb_audio_setfields(g_sfx_desc, 0x7fff);
	step_draw("SFX3----");
	sfx_ok = hb_audio_play_now(g_sfx_desc) ? 1 : 0;
	step_draw(sfx_ok ? "PLAY-OK-" : "PLAY-BAD");
	hdr.sfx_ok = (uint32_t)sfx_ok;

	/* Give DMA a moment while keeping the scheduler busy with draws. */
	for (i = 0; i < 12; i++) {
		hb_draw_str(8, 240, "SPIN----", 3, HB_CYAN, HB_BLACK);
		hb_draw_str(8, 280, "SPIN----", 3, HB_MAGENTA, HB_BLACK);
	}

	for (i = 0; i < N_WINS; i++)
		copy_mmio(body + i * WIN_SIZE, WIN_BASE[i], WIN_SIZE);

	{
		uint32_t *w = (uint32_t *)(body + 1 * WIN_SIZE); /* IIS0 */
		for (i = 0; i < WIN_SIZE / 4; i++) {
			if (w[i]) {
				iis0_nz = 1;
				break;
			}
		}
	}
	hdr.iis0_nonzero = (uint32_t)iis0_nz;
	hdr.flags = (had ? 1u : 0u) | (sfx_ok ? 2u : 0u) | (iis0_nz ? 4u : 0u);

	store_report(&hdr, body, sizeof(body));

	hb_draw_str(8, 320, iis0_nz ? "IIS0 LIVE" : "IIS0 ZERO", 2,
		    iis0_nz ? HB_GREEN : HB_RED, HB_BLACK);
	hb_draw_str(8, 360, "peek 0x09390000", 1, HB_WHITE, HB_BLACK);
}

/*
 * ftl_oracle — one-shot SCSI harness for NanoApps / ipod_sun.
 *
 * Asks *running RetailOS* for the live L2V mapping + SFTL-read DATA of
 * oracle LBAs, then copies a packed report to a fixed DRAM window so the
 * host can pull it with zero FAT writes:
 *
 *   ./start run ftl_oracle
 *   ./start peek 0x09380000 0x6200 > /tmp/ftl_oracle.hex
 *   # or: python tools/linux-n31/pull_ftl_oracle_peek.py
 *
 * NO NAND writes. Does not touch the volume.
 *
 * Address table below is for RetailOS / OSOS **1.0.2** (our IDA corpus).
 * NanoApps SDK docs mention 1.1.2 — if your device is 1.1.2, either
 * retarget the VAs or set FTL_RESOLVE_STRINGS and verify the probe.
 */

#include "hb_sdk.h"

/* ---- Fixed DRAM dump window (above LINK_VA 0x09280000) ------------------- */
#define ORACLE_DRAM   0x09380000u
#define ORACLE_MAGIC  0x4F4C5446u  /* 'FTLO' LE */
#define ORACLE_VER    1u
#define LBA_BYTES     4096u
#define META_BYTES    16u
#define MAX_RECORDS   6u

/* ---- OSOS 1.0.2 VAs (Image1 @ 0x08000000) -------------------------------- */
#ifndef FTL_OSOS_102
#define FTL_OSOS_102 1
#endif

#if FTL_OSOS_102
#define VA_L2V_SEARCH  0x00428694u
#define VA_S_READ      0x0056AB08u
#define VA_L2V_CURSOR  0x8D0F864u
#else
#error "Supply 1.1.2 VAs or enable a resolver before building"
#endif

typedef void (*l2v_search_fn)(uint32_t *cursor);
typedef int  (*s_read_fn)(uint32_t lba, uint32_t count, void *buf);

struct oracle_record {
	uint32_t lba;
	uint32_t l2v_status; /* 0=ok */
	uint32_t vba;
	uint32_t span;
	uint32_t read_status;
	uint32_t fnv;
	uint32_t head;       /* first 4 DATA bytes BE for eyeballing */
	uint8_t  meta[META_BYTES];
	uint8_t  data[LBA_BYTES];
};

struct oracle_blob {
	uint32_t magic;
	uint32_t ver;
	uint32_t count;
	uint32_t flags;      /* bit0: string probe saw sftl lba mismatch */
	uint32_t va_l2v;
	uint32_t va_sread;
	uint32_t va_cursor;
	uint32_t reserved;
	struct oracle_record rec[MAX_RECORDS];
};

static const uint32_t k_lbas[MAX_RECORDS] = {
	0u, 32u, 121u, 122u, 123u, 1916u
};

static uint8_t g_sec[LBA_BYTES] __attribute__((aligned(64)));
static struct oracle_blob g_blob;

static uint32_t fnv32(const uint8_t *p, uint32_t n)
{
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static void mem_zero(void *d, uint32_t n)
{
	uint8_t *p = d;
	for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

static void mem_copy(void *d, const void *s, uint32_t n)
{
	uint8_t *dd = d;
	const uint8_t *ss = s;
	for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}

/* Best-effort: confirm we're on a build that contains the known panic string. */
static uint32_t probe_sftl_string(void)
{
	/* Scan a slice of OSOS .rodata commonly mapped in N31 images. */
	const uint8_t *p = (const uint8_t *)0x087A0000u;
	const uint8_t *end = (const uint8_t *)0x087C0000u;
	const char *needle = "sftl: lba mismatch";
	uint32_t nlen = 18;
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

static void dram_store_blob(const struct oracle_blob *src)
{
	volatile uint8_t *dst = (volatile uint8_t *)ORACLE_DRAM;
	const uint8_t *s = (const uint8_t *)src;
	uint32_t n = (uint32_t)sizeof(*src);
	for (uint32_t i = 0; i < n; i++)
		dst[i] = s[i];
}

static void fill_record(struct oracle_record *r, uint32_t lba)
{
	volatile uint32_t *cur = (volatile uint32_t *)VA_L2V_CURSOR;
	l2v_search_fn l2v = (l2v_search_fn)VA_L2V_SEARCH;
	s_read_fn sread = (s_read_fn)VA_S_READ;

	mem_zero(r, sizeof(*r));
	r->lba = lba;
	r->l2v_status = 0xffffffffu;
	r->read_status = 0xffffffffu;

	hb_trace_log("LBA", lba, 0);

	/* L2V_Search: cursor[0]=lba → cursor[2]=vba, cursor[3]=span */
	cur[0] = lba;
	l2v((uint32_t *)(uintptr_t)cur);
	r->vba = cur[2];
	r->span = cur[3];
	r->l2v_status = 0;

	mem_zero(g_sec, LBA_BYTES);
	r->read_status = (uint32_t)sread(lba, 1, g_sec);
	mem_copy(r->data, g_sec, LBA_BYTES);
	r->fnv = fnv32(g_sec, LBA_BYTES);
	r->head = ((uint32_t)g_sec[0] << 24) | ((uint32_t)g_sec[1] << 16) |
		  ((uint32_t)g_sec[2] << 8) | (uint32_t)g_sec[3];
	/* META not always returned by this s_read signature — leave zero
	 * unless a future VA with meta out-arg is wired. */
}

HB_APP_ENTRY(payload_entry)
{
	uint32_t i;

	hb_trace_init();
	hb_trace_reset();
	hb_trace_log("FTLO", ORACLE_DRAM, (uint32_t)sizeof(g_blob));

	hb_fill_screen(HB_BLACK);
	hb_draw_str(8, 8, "FTL ORACLE", 2, HB_YELLOW, HB_BLACK);
	hb_draw_str(8, 40, "DRAM @9380000", 1, HB_CYAN, HB_BLACK);
	hb_draw_str(8, 56, "NO FAT WRITE", 1, HB_GREEN, HB_BLACK);

	mem_zero(&g_blob, sizeof(g_blob));
	g_blob.magic = ORACLE_MAGIC;
	g_blob.ver = ORACLE_VER;
	g_blob.count = MAX_RECORDS;
	g_blob.va_l2v = VA_L2V_SEARCH;
	g_blob.va_sread = VA_S_READ;
	g_blob.va_cursor = VA_L2V_CURSOR;
	g_blob.flags = probe_sftl_string();

	hb_draw_uint(8, 80, g_blob.flags, 1, HB_WHITE, HB_BLACK);
	hb_draw_str(24, 80, "=strprobe", 1, HB_RGB(0x80, 0x80, 0x80), HB_BLACK);

	for (i = 0; i < MAX_RECORDS; i++) {
		fill_record(&g_blob.rec[i], k_lbas[i]);
		hb_draw_uint(8, 100 + (int16_t)i * 18, k_lbas[i], 5, HB_WHITE, HB_BLACK);
		hb_draw_hex32(90, 100 + (int16_t)i * 18, g_blob.rec[i].vba, HB_CYAN, HB_BLACK);
	}

	dram_store_blob(&g_blob);
	hb_trace_log("STORED", ORACLE_DRAM, g_blob.rec[2].fnv); /* LBA121 fnv */

	hb_draw_str(8, HB_SCREEN_H - 36, "peek 0x09380000", 1, HB_YELLOW, HB_BLACK);
	hb_draw_str(8, HB_SCREEN_H - 20, "BOTH VOL = EXIT", 1,
		    HB_RGB(0x80, 0x80, 0x80), HB_BLACK);

	for (uint32_t frame = 0; frame < 20000000; frame++) {
		if (hb_button_pressed(HB_BTN_VOL_UP) &&
		    hb_button_pressed(HB_BTN_VOL_DOWN))
			break;
		hb_ui_pace();
	}
}

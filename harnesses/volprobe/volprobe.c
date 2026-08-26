/*
 * volprobe — list RetailOS FS volume ids 0..7: entry count at "/", whether
 * GrapeFirmware / iPod_Control exist. Draw results on screen; Vol± to exit.
 *
 * Deploy after `./start eject` so the FS is mounted:
 *   ./start --local run volprobe
 */
#include "hb_sdk.h"

#define BG   HB_BLACK
#define FG   HB_WHITE
#define OK   HB_GREEN
#define DIM  HB_RGB(0x80, 0x80, 0x80)

static int count_at(int vol, const char *path)
{
	hb_dir_t d;
	char name[64];
	bool is_dir = false;
	int n = 0;

	if (!hb_fs_dir_open_at(&d, path, false, vol))
		return -1;
	while (n < 64 && hb_fs_dir_next(&d, name, sizeof name, &is_dir)) {
		if (name[0] == '.')
			continue;
		n++;
	}
	hb_fs_dir_close(&d);
	return n;
}

static void draw_row(int y, int vol, int nroot, int nic, bool grape)
{
	char line[40];
	int i = 0;
	line[i++] = 'V';
	line[i++] = (char)('0' + vol);
	line[i++] = ':';
	line[i++] = ' ';
	if (nroot < 0) {
		line[i++] = '-';
	} else {
		if (nroot >= 10) line[i++] = (char)('0' + (nroot / 10) % 10);
		line[i++] = (char)('0' + nroot % 10);
	}
	line[i++] = ' ';
	line[i++] = 'e';
	line[i++] = 'n';
	line[i++] = 't';
	if (nic > 0) {
		line[i++] = ' ';
		line[i++] = 'i';
		line[i++] = 'C';
	}
	if (grape) {
		line[i++] = ' ';
		line[i++] = 'G';
		line[i++] = 'R';
		line[i++] = 'A';
		line[i++] = 'P';
		line[i++] = 'E';
	}
	line[i] = 0;
	hb_draw_str(4, (int16_t)y, line, 1, grape ? OK : FG, BG);
}

HB_APP_ENTRY(payload_entry)
{
	int v;

	hb_fill_screen(BG);
	hb_draw_str(4, 4, "VOLPROBE 0..7", 2, HB_YELLOW, BG);
	hb_draw_str(4, 28, "ent=/  iC=iPod_Control  GRAPE=fw", 1, DIM, BG);

	for (v = 0; v < 8; v++) {
		int nroot = count_at(v, "/");
		int nic = count_at(v, "/iPod_Control");
		bool grape =
			hb_fs_exists_at("/iPod_Control/Device/GrapeFirmware.bin", v) ||
			hb_fs_exists_at("iPod_Control/Device/GrapeFirmware.bin", v);
		draw_row(48 + v * 16, v, nroot, nic, grape);
	}

	hb_draw_str(4, HB_SCREEN_H - 28, "hold both VOL to exit", 1, DIM, BG);
	for (;;) {
		if (hb_button_pressed(HB_BTN_VOL_UP) &&
		    hb_button_pressed(HB_BTN_VOL_DOWN))
			break;
	}
}

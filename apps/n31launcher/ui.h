/*
 * ui.h - the launcher screens.
 *
 * Four of them, and which one you are on decides what the buttons mean.
 *
 *   HOME     three fixed tiles, one per side button. No scrolling.
 *   EXTRAS   what is on the volume, scrolled with the volume keys.
 *   MOUNTING the volume coming up, which takes a while.
 *   STARTING an app being handed the screen.
 *
 * PLAY always means go and HOME always means back, on every screen. That is the
 * whole navigation model, and it is why the extras list opens with PLAY rather
 * than HOME even though HOME is the more obvious "select" on a device with a
 * home button.
 */

#ifndef N31_LAUNCHER_UI_H
#define N31_LAUNCHER_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "status.h"

#define N31_SCREEN_W 240
#define N31_SCREEN_H 432

void n31_ui_init(void);

/*
 * A mount happening in the background, shown on the Extra apps tile rather
 * than by taking the screen. `pct` of -1 means indeterminate; `text` is the
 * current phase, or a short reason once it has stopped.
 */
void n31_ui_mount_progress(bool running, int pct, const char *text);

/* The three fixed tiles. `hot` lights one while its app starts. */
void n31_ui_home(void);
void n31_ui_home_hot(int tile, bool on);

/* Tile indices, in the order they are drawn - which is the order of the
   buttons down the side of the device, not any order of importance. */
#define N31_TILE_RADIO  0
#define N31_TILE_EXTRAS 1
#define N31_TILE_MUSIC  2

/*
 * The discovered apps - only those, never the two that already have a button on
 * the home screen. `selected` is an absolute index into n31_apps.
 *
 * With none to show it puts up a modal instead: what the user can do about it
 * differs, because with no volume mounted there is something to try and with a
 * mounted volume and nothing on it there is not.
 */
void n31_ui_extras(int selected, bool disk_mounted);
void n31_ui_extras_opening(bool on);

/*
 * The volume coming up. `pct` of -1 means the current step has no known total,
 * which is indeterminate rather than zero - so the bar says so instead of
 * sitting at the left edge looking stuck. `secs` is how long it has been going,
 * which during a long step is the only thing that moves.
 */
void n31_ui_mounting(int pct, const char *text, int secs);
void n31_ui_mount_failed(const char *reason);

/*
 * An app being started. Some take ten seconds to put anything on screen, and
 * without this the launcher's last frame just sits there looking frozen.
 */
void n31_ui_starting(const char *name, uint32_t accent);

/*
 * The home screen status bar: clock, Bluetooth, playback, tuner, battery.
 * Each part draws only when there is something true to say with it.
 */
void n31_ui_status_bar(const n31_status_t *st);

/* A few pixels of parallax on the tile icons, from the accelerometer. Doing
   nothing here is a perfectly good outcome - there may not be one. */
void n31_ui_tilt(int tilt_x, int tilt_y);

/* Top-right line on the screens that have one. */
void n31_ui_status(const char *text);

/* Mark everything dirty. An app that has been drawing over the framebuffer
   leaves LVGL believing the screen still holds what it last drew. */
void n31_ui_redraw(void);

#endif /* N31_LAUNCHER_UI_H */

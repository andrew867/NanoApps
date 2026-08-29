/*
 * ui.h - the launcher screens.
 *
 * Three of them, and which one you are on decides what the buttons mean.
 *
 *   HOME     three fixed tiles, one per side button. No scrolling.
 *   EXTRAS   whatever is mounted, scrolled with the volume keys.
 *   STARTING an app being handed the screen.
 *
 * Mounting is not among them. Bringing a volume up belongs to a system
 * service; this only reports what is mounted, and follows it as it changes.
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
 * A line of kernel commentary under the tiles, for the minute before a volume
 * is mounted. Pass NULL or "" to take it away again, which is what happens as
 * soon as there is something mounted to talk about instead.
 */
void n31_ui_home_note(const char *text);

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

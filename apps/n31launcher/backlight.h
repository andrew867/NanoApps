/*
 * The panel backlight, through the kernel's backlight class.
 *
 * /sys/class/backlight/<device>/ is the interface every backlight driver
 * exposes: brightness, max_brightness, and bl_power. Nothing here knows which
 * driver is behind it or pokes a register - the device is found by walking
 * the class, so a rename or a second backlight appearing costs nothing.
 *
 * The backlight is most of the power this screen uses, and it is separate
 * from the framebuffer: turning it off leaves LVGL compositing happily into
 * memory that nobody is lighting. That is the point - waking is instant
 * because nothing was torn down.
 */
#ifndef N31_BACKLIGHT_H
#define N31_BACKLIGHT_H

#include <stdbool.h>

/* Find the backlight and note where it is set. False if there is none, in
   which case every call below does nothing and says so once. */
bool n31_backlight_open(void);

/* Off, remembering the level to come back to. */
void n31_backlight_off(void);

/* On, at the level it was before the last off. */
void n31_backlight_on(void);

/* What the class device is called, for the log. NULL when there is none. */
const char *n31_backlight_name(void);

#endif /* N31_BACKLIGHT_H */

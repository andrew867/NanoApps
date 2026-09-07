/*
 * touch.h — find the touch panel, and nothing else.
 *
 * Three apps need this now and each of them builds its own LVGL against its
 * own lv_conf, so this deliberately knows nothing about LVGL: it hands back a
 * path and the caller does its own lv_evdev_create(). A shared file that
 * included lvgl.h would have to agree with three different configurations
 * about what LVGL is, which is a fight worth not having for eight lines.
 *
 * Shared the way backlight.c and status.c already are - compiled straight into
 * whoever wants it, which is how everything else here is shared.
 */

#ifndef N31_TOUCH_H
#define N31_TOUCH_H

/*
 * The touch panel's event node, or NULL when there is not one.
 *
 * Found by asking each device what it can do rather than by hard-coding a
 * number: the numbering depends on which input drivers registered and in what
 * order, and a touch controller that probes late is a different number from
 * the one it had last boot.
 *
 * Capability first, name second. A device that reports absolute X and Y is a
 * pointer whatever it calls itself, and matching on the name alone means a
 * driver that gets renamed - or that spells it "Touchscreen", or "digitizer",
 * or nothing at all - stops being found for a reason nobody would guess. The
 * name is still consulted, because it is what tells a touch panel from an
 * accelerometer, which also reports absolute axes.
 *
 * The returned string is owned by this module and is valid until the next
 * call. Safe to call when there is no /dev/input at all.
 */
const char *n31_touch_find(void);

#endif /* N31_TOUCH_H */

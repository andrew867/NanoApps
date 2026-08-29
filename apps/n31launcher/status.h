/*
 * status.h — what the status bar shows, and where it comes from.
 *
 * Everything here is read from sysfs or procfs on every poll. There is no
 * caching and no state: a status bar that keeps showing the last good value
 * after something goes away is worse than one that admits it does not know,
 * because the whole point of it is to be trusted at a glance.
 *
 * Every field is optional in the sense that its `have_` flag can be false, and
 * the UI draws nothing for it rather than drawing a zero. Nothing in this app
 * depends on any of it existing - the accelerometer especially, which is a nice
 * touch and never a requirement.
 */

#ifndef N31_LAUNCHER_STATUS_H
#define N31_LAUNCHER_STATUS_H

#include <stdbool.h>

typedef struct {
    bool have_battery;
    int  battery_pct;          /* 0-100 */
    bool charging;
    int  millivolts;           /* 0 when unknown */

    /* Present means the controller exists; up means it answered. They differ
       on this device today - hci0 is there and the BCM will not reset - and
       showing "on" for a controller that is not talking would be a lie. */
    bool bt_present;
    bool bt_up;

    bool audio_playing;        /* something has the playback PCM running */
    bool fm_on;                /* the tuner companion is powered */

    /*
     * The clock is only shown when it is real. This device has no battery-
     * backed RTC that survives a power cycle, so an unset clock reads as 1970
     * and a status bar cheerfully showing 00:38 is worse than useless. When it
     * is not set, uptime is displayed instead - which is both honest and the
     * more useful number on a device you are bringing up.
     */
    bool clock_valid;
    int  hours, minutes;       /* wall clock, or uptime, per clock_valid */

    bool have_tilt;
    int  tilt_x, tilt_y;       /* roughly -100..100, 0 is flat */
} n31_status_t;

/* Everything except tilt. Cheap, but not free - a handful of small reads. Once
   a second is plenty. */
void n31_status_read(n31_status_t *s);

/* Just the accelerometer, which is worth sampling faster than the rest.
   Leaves the field alone and returns false if there is no accelerometer. */
bool n31_status_tilt(n31_status_t *s);

#endif /* N31_LAUNCHER_STATUS_H */

/*
 * status.c — see status.h.
 *
 * Nothing here is hardcoded to a device name where scanning is cheap. The
 * battery is found by asking each power supply what type it is rather than by
 * looking for "d1830-battery", for the same reason the app scan looks under
 * every mount rather than /mnt/disk: the name is a detail of today's driver and
 * this file should not need editing when it changes.
 */

#include "status.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- little readers ------------------------------------------------------- */

static bool read_str(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    bool ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (!ok) return false;

    out[strcspn(out, "\r\n")] = 0;
    return true;
}

static bool read_int(const char *path, int *out)
{
    char buf[32];
    if (!read_str(path, buf, sizeof buf)) return false;

    char *end = buf;
    long v = strtol(buf, &end, 10);
    if (end == buf) return false;

    *out = (int)v;
    return true;
}

static bool exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* ---- battery -------------------------------------------------------------- */

/*
 * The gauge is derived from voltage and it is noisy: it moved nine points in
 * two minutes on a device that was sitting still, because the rail sags under
 * load and recovers. Shown raw that reads as a fault in the status bar rather
 * than as a property of the measurement.
 *
 * So it is averaged over about half a minute. Long enough to sit still, short
 * enough to follow a real discharge - which on a battery this size is a point
 * every few minutes, far slower than the window.
 *
 * The window is filled with the first reading rather than with zeros, or the
 * bar would climb from empty over the first half minute of every boot.
 */
#define BATT_WINDOW 60

static int  s_mv_hist[BATT_WINDOW];
static int  s_mv_n;
static int  s_mv_at;

/*
 * The MEDIAN of the last minute of voltage readings, not the mean.
 *
 * The rail sags under load - measured dropping from 3875 mV to 3775 while NAND
 * and CPU are busy, which is a hundred millivolts and most of the top of the
 * scale. Those dips are brief and deep, which is exactly the shape a mean is
 * bad at: a few outliers drag it down and the bar reads low for the whole
 * window afterwards. A median ignores them until they are more than half the
 * window, by which point the battery really has dropped.
 *
 * Voltage is smoothed rather than percentage, because the curve below is not
 * linear - averaging percentages either side of a knee gives a different
 * answer from converting the average, and the voltage is the thing actually
 * being measured.
 */
static int smooth_millivolts(int mv)
{
    if (s_mv_n == 0) {
        for (int i = 0; i < BATT_WINDOW; i++) s_mv_hist[i] = mv;
        s_mv_n = BATT_WINDOW;
        s_mv_at = 0;
        return mv;
    }

    s_mv_hist[s_mv_at] = mv;
    s_mv_at = (s_mv_at + 1) % BATT_WINDOW;

    /* Insertion sort of a copy. Sixty ints once a second is nothing, and it is
       far easier to be sure of than a running median structure. */
    int sorted[BATT_WINDOW];
    for (int i = 0; i < BATT_WINDOW; i++) sorted[i] = s_mv_hist[i];
    for (int i = 1; i < BATT_WINDOW; i++) {
        int k = sorted[i], j = i - 1;
        while (j >= 0 && sorted[j] > k) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = k;
    }
    return sorted[BATT_WINDOW / 2];
}

/*
 * Percent from the cell voltage.
 *
 * The driver's own `capacity` reads a constant 60 on this device while
 * RetailOS shows the same battery full, so it is not a gauge - it is a number.
 * voltage_now is a real measurement from the same supply, and a single-cell
 * lithium battery has a well known and fairly flat discharge curve, so this
 * derives the percentage rather than believing the driver.
 *
 * ANCHORED TO THIS DEVICE. voltage_now reads 3875 mV when RetailOS shows the
 * battery full, which is nowhere near a bare cell's 4200 - so the standard
 * curve applied directly read about 72% on a full battery. Whether that is a
 * divider, an ADC reference or simply where this pack sits, the fixed point is
 * measured and the curve is scaled onto it: every breakpoint below is the
 * standard open-circuit shape multiplied by 3875/4200.
 *
 * So the top of the scale is measured and the shape is assumed. The shape is
 * the safer half to assume - it is the same for every single-cell lithium
 * battery and it is mostly flat - but the low end is unverified, and a reading
 * taken as it approaches empty would pin it properly.
 *
 * Piecewise-linear on purpose. Fitting a smooth function would imply precision
 * the measurement does not have.
 */
static int pct_from_millivolts(int mv)
{
    /* Standard curve x 3875/4200, rounded to the millivolt. */
    static const struct { int mv, pct; } CURVE[] = {
        { 3875, 100 }, { 3783,  95 }, { 3690,  87 }, { 3598,  76 },
        { 3506,  62 }, { 3413,  47 }, { 3321,  30 }, { 3229,  16 },
        { 3137,   7 }, { 3045,   2 }, { 2952,   0 },
    };
    const int n = (int)(sizeof CURVE / sizeof CURVE[0]);

    if (mv >= CURVE[0].mv) return 100;
    if (mv <= CURVE[n - 1].mv) return 0;

    for (int i = 1; i < n; i++) {
        if (mv < CURVE[i].mv) continue;
        /* Between CURVE[i] and CURVE[i-1], which is the higher voltage. */
        int span_mv = CURVE[i - 1].mv - CURVE[i].mv;
        int span_pc = CURVE[i - 1].pct - CURVE[i].pct;
        if (span_mv <= 0) return CURVE[i].pct;
        return CURVE[i].pct + (mv - CURVE[i].mv) * span_pc / span_mv;
    }
    return 0;
}

/*
 * The battery's own `status` reads "Unknown" on this PMIC, so charging comes
 * from the charger supply instead. Both are found by type rather than by name.
 */
static void read_power(n31_status_t *s)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        char base[128], path[192], v[64];
        /* Bounded: d_name can be 255 bytes and these buffers cannot hold
           that plus the prefix. A power supply with a name that long does not
           exist, and snprintf would truncate anyway - saying so here is what
           lets this compile under -Werror where it is shared. */
        snprintf(base, sizeof base, "/sys/class/power_supply/%.96s", e->d_name);

        snprintf(path, sizeof path, "%s/type", base);
        if (!read_str(path, v, sizeof v)) continue;

        if (!strcmp(v, "Battery")) {
            int pct = -1;
            snprintf(path, sizeof path, "%s/capacity", base);
            if (read_int(path, &pct)) {
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
            }

            int uv;
            snprintf(path, sizeof path, "%s/voltage_now", base);
            if (read_int(path, &uv)) s->millivolts = uv / 1000;

            /* Voltage first when there is one, because the driver's capacity
               is not a gauge on this device - see pct_from_millivolts. The
               range test is what makes this safe: a supply that reports a
               plausible cell voltage is one worth deriving from, and anything
               else falls back to whatever capacity said.

               The window is wide because this device reads 3875 mV full and
               2952 empty - nine hundred millivolts for the whole scale - so a
               reading outside it is not this battery. */
            if (s->millivolts >= 2800 && s->millivolts <= 4400) {
                int mv = smooth_millivolts(s->millivolts);
                s->battery_pct = pct_from_millivolts(mv);
                s->have_battery = true;
            } else if (pct >= 0) {
                s->battery_pct = pct;
                s->have_battery = true;
            }

            /* The battery's own status, in case a future driver fills it in.
               Today it says Unknown and the charger below is the answer. */
            snprintf(path, sizeof path, "%s/status", base);
            if (read_str(path, v, sizeof v) && !strcmp(v, "Charging"))
                s->charging = true;
            continue;
        }

        /*
         * Not the battery, so it is a charger or a USB supply. Three ways it
         * can say power is present, and they are checked in the order of how
         * definite they are:
         *
         *   status=Charging   current is going into the cell
         *   online=1          a supply is attached, charging or not
         *   present=1         the hardware sees something on the port
         *
         * `online` matters as much as `status` here: a full battery on a
         * plugged-in cable reports Not charging, and the icon is meant to say
         * "there is a cable" rather than "electrons are moving".
         */
        snprintf(path, sizeof path, "%s/status", base);
        if (read_str(path, v, sizeof v) && !strcmp(v, "Charging"))
            s->charging = true;

        int on = 0;
        snprintf(path, sizeof path, "%s/online", base);
        if (read_int(path, &on) && on) s->plugged = true;

        snprintf(path, sizeof path, "%s/present", base);
        if (read_int(path, &on) && on) s->plugged = true;
    }

    closedir(d);

    /* Charging implies a cable even where no supply exposed online/present. */
    if (s->charging) s->plugged = true;

    /*
     * Failing that, ask the USB gadget whether the link is up. On this device
     * the PMIC's charger node is not always exposed, but the UDC knows: it
     * reports "configured" only when a host has enumerated it, which needs a
     * cable and a port at the other end.
     */
    if (!s->plugged) {
        DIR *u = opendir("/sys/class/udc");
        if (u) {
            struct dirent *ue;
            while ((ue = readdir(u))) {
                if (ue->d_name[0] == '.') continue;
                char p[192], st[64];
                snprintf(p, sizeof p, "/sys/class/udc/%.96s/state", ue->d_name);
                if (read_str(p, st, sizeof st) && strcmp(st, "not attached")) {
                    s->plugged = true;
                    break;
                }
            }
            closedir(u);
        }
    }
}

/* ---- bluetooth ------------------------------------------------------------ */

/*
 * Whether the controller is UP, which sysfs does not say: /sys/class/bluetooth/
 * hci0 has only device, power, reset, subsystem and uevent in it. So this asks
 * the HCI layer directly.
 *
 * The distinction matters on this device rather than being pedantry - hci0
 * exists and the BCM is not answering its reset, so "the node is there" and
 * "Bluetooth is on" are currently different answers.
 */
#define N31_AF_BLUETOOTH 31
#define N31_BTPROTO_HCI  1
#define N31_HCIGETDEVINFO 0x800448d3u   /* _IOR('H', 211, int) */
#define N31_HCI_UP        0             /* bit 0 of flags */

struct n31_hci_dev_stats {
    uint32_t err_rx, err_tx, cmd_tx, evt_rx, acl_tx, acl_rx;
    uint32_t sco_tx, sco_rx, byte_rx, byte_tx;
};

struct n31_hci_dev_info {
    uint16_t dev_id;
    char     name[8];
    uint8_t  bdaddr[6];
    uint32_t flags;
    uint8_t  type;
    uint8_t  features[8];
    uint32_t pkt_type;
    uint32_t link_policy;
    uint32_t link_mode;
    uint16_t acl_mtu;
    uint16_t acl_pkts;
    uint16_t sco_mtu;
    uint16_t sco_pkts;
    struct n31_hci_dev_stats stat;
};

static void read_bluetooth(n31_status_t *s)
{
    s->bt_present = exists("/sys/class/bluetooth/hci0");
    if (!s->bt_present) return;

    int fd = socket(N31_AF_BLUETOOTH, SOCK_RAW, N31_BTPROTO_HCI);
    if (fd < 0) return;                 /* no BT stack: present, not up */

    struct n31_hci_dev_info di;
    memset(&di, 0, sizeof di);
    di.dev_id = 0;

    if (ioctl(fd, N31_HCIGETDEVINFO, &di) >= 0)
        s->bt_up = (di.flags & (1u << N31_HCI_UP)) != 0;

    close(fd);
}

/* ---- audio ---------------------------------------------------------------- */

/*
 * An idle PCM's status file is the single word "closed"; an open one is a block
 * of key/value lines with a state in it. So anything that is not "closed" and
 * is RUNNING counts as playing.
 *
 * Only playback substreams. The capture side is the FM tuner, and showing a
 * play indicator because the radio is being recorded would be wrong.
 */
static void read_audio(n31_status_t *s)
{
    for (int card = 0; card < 4 && !s->audio_playing; card++) {
        for (int pcm = 0; pcm < 4 && !s->audio_playing; pcm++) {
            char path[128], line[64];
            snprintf(path, sizeof path,
                     "/proc/asound/card%d/pcm%dp/sub0/status", card, pcm);

            FILE *f = fopen(path, "r");
            if (!f) continue;

            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "RUNNING")) { s->audio_playing = true; break; }
            }
            fclose(f);
        }
    }
}

/* ---- the tuner ------------------------------------------------------------ */

/*
 * fm_power is write-only on this driver - reading it fails - so the readable
 * power_on of the companion is what the indicator is based on. That means "the
 * tuner is powered" rather than "the radio is playing", which is the honest
 * reading of what can be known from here.
 */
static void read_fm(n31_status_t *s)
{
    static const char *paths[] = {
        "/sys/bus/platform/devices/soc:bcm2078-companion/power_on",
        "/sys/devices/platform/soc/soc:bcm2078-companion/power_on",
    };

    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        int v;
        if (read_int(paths[i], &v)) { s->fm_on = (v != 0); return; }
    }
}

/* ---- the clock ------------------------------------------------------------ */

/* Anything before this is a clock that was never set. 2020-09-13. */
#define CLOCK_LOOKS_REAL 1600000000L

static void read_clock(n31_status_t *s)
{
    time_t now = time(NULL);

    if (now >= CLOCK_LOOKS_REAL) {
        struct tm tm;
        if (localtime_r(&now, &tm)) {
            s->clock_valid = true;
            s->hours = tm.tm_hour;
            s->minutes = tm.tm_min;
            return;
        }
    }

    /* No RTC that survives a power cycle, so the wall clock is 1970 and
       uptime is both honest and the more useful number here. */
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return;

    double up = 0;
    if (fscanf(f, "%lf", &up) == 1) {
        long secs = (long)up;
        s->hours = (int)(secs / 3600);
        s->minutes = (int)((secs % 3600) / 60);
    }
    fclose(f);
}

/* ---- tilt ----------------------------------------------------------------- */

/*
 * lis3lv02d reports "(x,y,z)" in its own units, roughly +/-1000 per g. Scaled
 * to about +/-100 and clamped, because what this drives is a couple of pixels
 * of parallax and the exact calibration does not matter - only that it is
 * steady and centred when the device is flat.
 */
bool n31_status_tilt(n31_status_t *s)
{
    static const char *paths[] = {
        "/sys/bus/platform/devices/lis3lv02d/position",
        "/sys/devices/platform/lis3lv02d/position",
    };

    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        char buf[64];
        if (!read_str(paths[i], buf, sizeof buf)) continue;

        int x, y, z;
        if (sscanf(buf, "(%d,%d,%d)", &x, &y, &z) != 3) continue;
        (void)z;

        x /= 10;
        y /= 10;
        if (x < -100) x = -100;
        if (x > 100) x = 100;
        if (y < -100) y = -100;
        if (y > 100) y = 100;

        s->tilt_x = x;
        s->tilt_y = y;
        s->have_tilt = true;
        return true;
    }

    s->have_tilt = false;
    return false;
}

/* ---- all of it ------------------------------------------------------------ */

void n31_status_read(n31_status_t *s)
{
    bool had_tilt = s->have_tilt;
    int tx = s->tilt_x, ty = s->tilt_y;

    memset(s, 0, sizeof *s);

    read_power(s);
    read_bluetooth(s);
    read_audio(s);
    read_fm(s);
    read_clock(s);

    /* Tilt is sampled on its own schedule; do not blank it here. */
    s->have_tilt = had_tilt;
    s->tilt_x = tx;
    s->tilt_y = ty;
}

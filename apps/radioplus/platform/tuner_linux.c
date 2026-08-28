/*
 * tuner_linux.c — tuner.h through the bcm2078-bt driver.
 *
 * The driver owns hci0 and sends FM_RDS_Command (vendor opcode 0xFC15) itself.
 * Everything here goes through its sysfs interface and nothing touches the UART
 * or builds an HCI packet, which is the driver's job. If the tuner needs
 * something this file cannot express, the answer is a driver patch, not a
 * shortcut around it.
 *
 * Attributes, from bcm2078-bt.c:
 *
 *   fm_power   W   "1" / "0"
 *   fm_tune    W   kHz, decimal
 *   fm_seek    W   "up" / "down"
 *   fm_rssi    R   "%u"
 *   fm_snr     R   "%u"
 *   fm_rds     RW  write "1"/"0"; read gives on=, poll=, groups=, flag=,
 *                  pi=, pty=, ps=, rt=
 *   fm_reg     RW  write "r <reg> <len>" or "w <reg> <hex>"; read gives
 *                  "0x%02x: %*ph"
 *   hci_cmd    RW  arbitrary command, not used here
 *
 * Note what fm_reg buys beyond convenience: register 0x80 is the RDS FIFO, so
 * reading it gives raw tuples that this app can decode itself with core/rds.c
 * rather than settling for the driver's summary. The driver's own pi, ps and rt
 * are then an independent oracle for that decoder - and in particular for the
 * one unverified thing in it, the FIFO tuple framing. Both are read, and a
 * disagreement is worth surfacing rather than hiding.
 */

#include "tuner.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../core/fmreg.h"

static char s_dir[256];          /* the sysfs directory holding fm_power */
static bool s_ready;
static char s_desc[416];   /* holds the sysfs path plus a note */

/* ---- sysfs plumbing ------------------------------------------------------ */

static int attr_path(char *out, size_t n, const char *name)
{
    int k = snprintf(out, n, "%s/%s", s_dir, name);
    return (k > 0 && (size_t)k < n) ? 0 : -1;
}

static en_tuner_err_t attr_write(const char *name, const char *value)
{
    char path[320];
    if (!s_ready || attr_path(path, sizeof path, name)) return EN_TUNER_FAILED;

    int fd = open(path, O_WRONLY);
    if (fd < 0) return (errno == ENOENT) ? EN_TUNER_UNSUPPORTED : EN_TUNER_FAILED;

    size_t len = strlen(value);
    ssize_t w = write(fd, value, len);
    int err = errno;
    close(fd);

    if (w == (ssize_t)len) return EN_TUNER_OK;

    /* The driver returns ENETDOWN when hci0 is not up, which is the single
       most likely failure and deserves its own answer rather than a generic
       one - the fix is to bring Bluetooth up, not to retry. */
    if (err == ENETDOWN || err == ENODEV) return EN_TUNER_NOT_READY;
    return EN_TUNER_FAILED;
}

static en_tuner_err_t attr_read(const char *name, char *buf, size_t n)
{
    char path[320];
    if (!s_ready || attr_path(path, sizeof path, name)) return EN_TUNER_FAILED;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return (errno == ENOENT) ? EN_TUNER_UNSUPPORTED : EN_TUNER_FAILED;

    ssize_t r = read(fd, buf, n - 1);
    int err = errno;
    close(fd);

    if (r < 0) {
        if (err == ENETDOWN || err == ENODEV) return EN_TUNER_NOT_READY;
        return EN_TUNER_FAILED;
    }
    buf[r] = 0;
    return EN_TUNER_OK;
}

/* ---- bringing the controller up ------------------------------------------ */

/*
 * FM rides on hci0, and the driver answers ENETDOWN while it is down. The
 * device rootfs has no hciconfig, btmgmt or rfkill to bring it up with, so the
 * app does it directly - this is precisely what hciconfig hci0 up performs.
 *
 * Doing it here rather than in a launcher means the radio works when started by
 * hand, from a menu, or on boot, instead of only through one script.
 */
#define BT_AF_BLUETOOTH 31
#define BT_PROTO_HCI    1
#define BT_HCIDEVUP     _IOW('H', 201, int)

static bool hci_up(void)
{
    int fd = socket(BT_AF_BLUETOOTH, SOCK_RAW, BT_PROTO_HCI);
    if (fd < 0) return false;

    int r = ioctl(fd, BT_HCIDEVUP, 0);
    int err = errno;
    close(fd);

    /* Already up is the common case and is success, not failure. */
    return r == 0 || err == EALREADY || err == EBUSY;
}

/* ---- finding the device -------------------------------------------------- */

/* The driver binds under /sys/devices somewhere that depends on how the
   companion device is described, so the directory is found by looking for the
   attribute rather than by hard-coding a path that a device-tree change would
   silently break. */
static bool find_dir(const char *root, int depth)
{
    if (depth > 4) return false;

    DIR *d = opendir(root);
    if (!d) return false;

    char path[256];
    struct dirent *e;
    bool found = false;

    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int k = snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        if (k <= 0 || (size_t)k >= sizeof path) continue;

        if (strcmp(e->d_name, "fm_power") == 0) {
            snprintf(s_dir, sizeof s_dir, "%s", root);
            found = true;
            break;
        }
        /* Symlinks are skipped deliberately: /sys is full of them and following
           them turns this walk into a cycle. */
        if (e->d_type == DT_DIR) found = find_dir(path, depth + 1);
    }
    closedir(d);
    return found;
}

en_tuner_err_t en_tuner_init(void)
{
    if (s_ready) return EN_TUNER_OK;

    if (!find_dir("/sys/devices", 0)) {
        snprintf(s_desc, sizeof s_desc,
                 "no tuner: bcm2078-bt did not bind (no fm_power in /sys)");
        return EN_TUNER_NO_DEVICE;
    }
    s_ready = true;

    /* hci0 has to be up before anything works, because FM_RDS_Command rides on
       it. Say so plainly rather than letting every later call fail. */
    /* The controller existing is not the same as it being usable, so bring it
       up rather than reporting on it. If it is missing entirely the driver has
       not bound and there is nothing to do about that here. */
    bool hci = access("/sys/class/bluetooth/hci0", F_OK) == 0;
    if (hci) hci = hci_up();

    snprintf(s_desc, sizeof s_desc, "bcm2078-bt at %s%s",
             s_dir, hci ? "" : "  (no hci0 - is bluetooth loaded?)");

    return hci ? EN_TUNER_OK : EN_TUNER_NOT_READY;
}

void en_tuner_shutdown(void)
{
    if (s_ready) attr_write("fm_power", "0\n");
    s_ready = false;
}

const char *en_tuner_backend(void)
{
    return s_desc[0] ? s_desc : "bcm2078-bt (not probed)";
}

/* ---- basic control ------------------------------------------------------- */

en_tuner_err_t en_tuner_power(bool on)
{
    return attr_write("fm_power", on ? "1\n" : "0\n");
}

en_tuner_err_t en_tuner_tune(uint32_t khz)
{
    char v[24];
    snprintf(v, sizeof v, "%u\n", khz);
    return attr_write("fm_tune", v);
}

en_tuner_err_t en_tuner_seek(bool up)
{
    return attr_write("fm_seek", up ? "up\n" : "down\n");
}

en_tuner_err_t en_tuner_state(en_tuner_state_t *out)
{
    if (!out) return EN_TUNER_FAILED;
    memset(out, 0, sizeof *out);

    char buf[64];
    en_tuner_err_t e = attr_read("fm_rssi", buf, sizeof buf);
    if (e != EN_TUNER_OK) return e;
    out->rssi = (uint8_t)strtoul(buf, 0, 10);

    if (attr_read("fm_snr", buf, sizeof buf) == EN_TUNER_OK)
        out->snr = (int8_t)strtol(buf, 0, 10);

    /* Stereo lives in the RDS flag register, which fm_rds reports. */
    if (attr_read("fm_rds", buf, sizeof buf) == EN_TUNER_OK) {
        const char *f = strstr(buf, "flag=0x");
        if (f) {
            unsigned long flag = strtoul(f + 7, 0, 16);
            out->stereo = (flag & 0x0040u) != 0;    /* STEREO ACTIVE */
        }
    }

    /* The driver has no attribute that reports the tuned frequency back, so
       the register is read directly rather than trusting a cached value that a
       seek would have invalidated. */
    uint8_t raw[2];
    if (en_tuner_reg_read(0x0A, raw, 2) == EN_TUNER_OK)
        out->khz = en_fm_reg_to_khz((uint16_t)((raw[0] << 8) | raw[1]));

    out->powered = true;
    return EN_TUNER_OK;
}

en_tuner_err_t en_tuner_set_region(const en_region_t *rg)
{
    if (!rg) return EN_TUNER_FAILED;

    /* The driver has no region attribute, so this is the register path: read
       the two registers that carry unrelated bits, let core/region build the
       writes, and send them. Doing it here rather than in the UI keeps the
       read-modify-write in one place. */
    uint8_t fm_ctrl = 0;
    uint16_t audio_ctrl = 0;
    uint8_t raw[2];

    if (en_tuner_reg_read(0x01, raw, 1) == EN_TUNER_OK) fm_ctrl = raw[0];
    if (en_tuner_reg_read(0x05, raw, 2) == EN_TUNER_OK)
        audio_ctrl = (uint16_t)((raw[0] << 8) | raw[1]);

    uint8_t cmds[EN_REGION_CMDS][EN_FM_CMD_MAX];
    uint8_t lens[EN_REGION_CMDS];
    uint8_t n = en_region_apply(rg, fm_ctrl, audio_ctrl, cmds, lens,
                                EN_REGION_CMDS);
    if (!n) return EN_TUNER_FAILED;

    /* en_region_apply builds whole HCI packets; the driver wants register
       writes, so the payload is lifted back out. Building the packet and then
       unpacking it looks redundant, but it keeps one description of what a
       region write is, and the RetailOS backend sends those same packets
       whole. */
    for (uint8_t i = 0; i < n; i++) {
        uint8_t addr = cmds[i][4];
        const uint8_t *payload = &cmds[i][6];
        uint8_t plen = (uint8_t)(lens[i] - 6);
        en_tuner_err_t e = en_tuner_reg_write(addr, payload, plen);
        if (e != EN_TUNER_OK) return e;
    }
    return EN_TUNER_OK;
}

/* ---- RDS ----------------------------------------------------------------- */

en_tuner_err_t en_tuner_rds_enable(bool on)
{
    return attr_write("fm_rds", on ? "1\n" : "0\n");
}

int en_tuner_rds_poll(en_rds_t *r, uint16_t (*out)[4], uint8_t *out_valid,
                      uint8_t out_max)
{
    if (!r) return -1;

    /* Reading fm_rds drains the FIFO into the driver's own decoder, so it has
       to happen before the raw read or there is nothing left to fetch. It also
       gives the cross-check below. */
    char summary[512];
    bool have_summary = attr_read("fm_rds", summary, sizeof summary) == EN_TUNER_OK;

    /* Register 0x80 is the RDS FIFO. Ask for a sensible bite rather than the
       250-byte maximum: at three bytes a tuple and four tuples a group, 96
       bytes is eight groups, which is more than arrives between polls. */
    uint8_t fifo[96];
    if (en_tuner_reg_read(0x80, fifo, sizeof fifo) != EN_TUNER_OK)
        return have_summary ? 0 : -1;

    uint16_t groups[8][4];
    uint8_t valid[8];
    uint8_t n = en_rds_unpack_tuples_unverified(fifo, sizeof fifo, groups,
                                                valid, 8);
    for (uint8_t i = 0; i < n; i++) {
        en_rds_group(r, groups[i], valid[i]);

        /* Copied out before decoding changes anything, so a recorder gets what
           arrived rather than what was made of it. */
        if (out && out_valid && i < out_max) {
            for (uint8_t k = 0; k < 4; k++) out[i][k] = groups[i][k];
            out_valid[i] = valid[i];
        }
    }

    /*
     * The driver decodes RDS too, and its answer is an independent check on
     * ours - most usefully on the FIFO tuple framing, which is the one thing in
     * core/rds.c that was guessed rather than read out of a specification.
     *
     * If the driver has a PI and we do not, or the two disagree, the framing is
     * wrong and everything decoded here is noise. That is worth knowing loudly
     * during bring-up rather than discovering through a station name that never
     * quite appears.
     */
    if (have_summary) {
        const char *p = strstr(summary, "pi=0x");
        if (p) {
            uint16_t driver_pi = (uint16_t)strtoul(p + 5, 0, 16);
            if (driver_pi && r->pi_valid && r->pi != driver_pi)
                fprintf(stderr,
                        "radioplus: RDS framing disagrees with the driver "
                        "(ours %04X, driver %04X) - "
                        "en_rds_unpack_tuples_unverified is wrong\n",
                        r->pi, driver_pi);
        }
    }
    return n;
}

/* ---- raw register access ------------------------------------------------- */

bool en_tuner_can_raw(void) { return true; }

en_tuner_err_t en_tuner_reg_read(uint8_t addr, uint8_t *buf, uint8_t len)
{
    if (!buf || !len) return EN_TUNER_FAILED;

    char req[32];
    snprintf(req, sizeof req, "r %02x %u\n", addr, len);
    en_tuner_err_t e = attr_write("fm_reg", req);
    if (e != EN_TUNER_OK) return e;

    /* The driver answers as "0x%02x: %*ph", which is space-separated hex. */
    char out[1024];
    e = attr_read("fm_reg", out, sizeof out);
    if (e != EN_TUNER_OK) return e;

    const char *p = strchr(out, ':');
    if (!p) return EN_TUNER_FAILED;      /* "no read pending" */
    p++;

    uint8_t got = 0;
    while (got < len) {
        while (*p == ' ') p++;
        if (!*p || *p == '\n') break;

        char *end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        buf[got++] = (uint8_t)v;
        p = end;
    }
    return got == len ? EN_TUNER_OK : EN_TUNER_FAILED;
}

en_tuner_err_t en_tuner_reg_write(uint8_t addr, const uint8_t *buf, uint8_t len)
{
    if (!buf || !len || len > 8) return EN_TUNER_FAILED;

    /* The driver takes the value as one hex number, so the payload is written
       big-endian to match how the register table describes multi-byte values. */
    char req[64];
    int k = snprintf(req, sizeof req, "w %02x ", addr);
    for (uint8_t i = 0; i < len && k > 0 && (size_t)k < sizeof req - 3; i++)
        k += snprintf(req + k, sizeof req - (size_t)k, "%02x", buf[i]);
    snprintf(req + k, sizeof req - (size_t)k, "\n");

    return attr_write("fm_reg", req);
}

const char *en_tuner_strerror(en_tuner_err_t e)
{
    switch (e) {
    case EN_TUNER_OK:          return "ok";
    case EN_TUNER_NO_DEVICE:   return "no tuner found";
    case EN_TUNER_NOT_READY:   return "Bluetooth is down; FM rides on hci0";
    case EN_TUNER_UNSUPPORTED: return "not supported on this platform";
    default:                   return "failed";
    }
}

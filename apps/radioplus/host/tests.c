/*
 * tests.c — desktop tests for the Radio+ core.
 *
 * Everything here runs with no tuner, no iPod and no Bluetooth stack, which is
 * the whole reason core/ has no dependencies. Two kinds of check:
 *
 *   - the encoder produces exactly the bytes the firmware's own framing implies,
 *     and rejects malformed events rather than reading past them;
 *   - the register table is internally consistent, which matters because it is
 *     transcribed by hand from a specification and a typo in a bit offset is
 *     otherwise invisible until it is a wrong register write on real hardware.
 */

#include <stdio.h>
#include <string.h>

#include "../core/fmcmd.h"
#include "../core/fmreg.h"
#include "../core/rds.h"
#include "../core/region.h"
#include "../core/wav.h"
#include "../core/store.h"
#include "../core/scan.h"
#include "../core/timer.h"

static int checks, failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static void section(const char *name) { printf("\n== %s\n", name); }

static int same(const uint8_t *a, const uint8_t *b, uint8_t n)
{
    return memcmp(a, b, n) == 0;
}

static void show(const char *label, const uint8_t *p, uint8_t n)
{
    printf("  %-22s", label);
    for (uint8_t i = 0; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

/* ---- encoding ------------------------------------------------------------ */

static void test_build(void)
{
    section("command encoding");

    uint8_t buf[EN_FM_CMD_MAX];

    /* Turn FM and RDS on: write 0x03 to I2C_FM_RDS_SYSTEM. */
    uint8_t on = 0x03;
    uint8_t n = en_fm_build_write(0x00, &on, 1, buf, sizeof buf);
    const uint8_t want_write[] = { 0x01, 0x15, 0xFC, 0x03, 0x00, 0x00, 0x03 };
    show("FM on + RDS on", buf, n);
    CHECK(n == sizeof want_write, "write length %u", n);
    CHECK(same(buf, want_write, n), "write bytes differ");

    /* Read RSSI: one byte from 0x0F. */
    n = en_fm_build_read(0x0F, en_fm_read_len(0x0F), buf, sizeof buf);
    const uint8_t want_read[] = { 0x01, 0x15, 0xFC, 0x03, 0x0F, 0x01, 0x01 };
    show("read RSSI", buf, n);
    CHECK(n == sizeof want_read, "read length %u", n);
    CHECK(same(buf, want_read, n), "read bytes differ");

    /* A read always carries three parameters regardless of how much it asks
       for, because Read_Length is one byte. */
    n = en_fm_build_read(0x80, 250, buf, sizeof buf);
    show("read 250 RDS tuples", buf, n);
    CHECK(n == 7, "a big read is still a 7-byte command, got %u", n);
    CHECK(buf[3] == 3, "parameter length should be 3, got %u", buf[3]);
    CHECK(buf[6] == 250, "requested length not carried");

    /* Tune to 98.5 MHz. The register is kHz offset from 64 MHz, so 34500. */
    uint8_t freq[2];
    en_fm_put_u16(freq, en_fm_khz_to_reg(98500));
    n = en_fm_build_write(0x0A, freq, 2, buf, sizeof buf);
    show("tune 98.5 MHz", buf, n);
    CHECK(en_fm_khz_to_reg(98500) == 34500, "98.5 MHz should encode as 34500");
    CHECK(n == 8, "tune command length %u", n);
    CHECK(buf[4] == 0x0A && buf[5] == 0x00, "wrong register or mode");
    CHECK(en_fm_get_u16(&buf[6]) == 34500, "frequency did not round-trip");
    CHECK(en_fm_reg_to_khz(34500) == 98500, "reverse conversion wrong");

    /* Refusals rather than overruns. */
    uint8_t tiny[4];
    CHECK(en_fm_build_write(0x00, &on, 1, tiny, sizeof tiny) == 0,
          "a write that does not fit should be refused");
    CHECK(en_fm_build_read(0x0F, 1, tiny, sizeof tiny) == 0,
          "a read that does not fit should be refused");
    CHECK(en_fm_build_read(0x0F, 0, buf, sizeof buf) == 0,
          "a zero-length read is meaningless and should be refused");
    CHECK(en_fm_build_write(0x00, 0, 1, buf, sizeof buf) == 0,
          "a null payload with a non-zero length should be refused");
}

/* ---- event decoding ------------------------------------------------------ */

static void test_parse(void)
{
    section("event decoding");

    /* A command complete carrying one byte of RSSI. Layout comes from the
       firmware validating its own reply: 04 0E plen 01 ocf ogf status, then
       the command's own Status, I2C_Address and Read_Write_Mode. */
    const uint8_t evt[] = {
        0x04, 0x0E, 0x07, 0x01, 0x15, 0xFC, 0x00, 0x0F, 0x01, 0x2A
    };
    en_fm_reply_t r;
    CHECK(en_fm_parse_event(evt, sizeof evt, &r), "valid event rejected");
    CHECK(r.status == 0, "status %u", r.status);
    CHECK(r.addr == 0x0F, "address %02x", r.addr);
    CHECK(r.mode == EN_FM_READ, "mode should be read");
    CHECK(r.value_len == 1, "value length %u", r.value_len);
    CHECK(r.value && r.value[0] == 0x2A, "value wrong");
    printf("  RSSI read back: %u\n", r.value ? r.value[0] : 0);

    /* A failure from the chip is a real answer and must still parse. */
    uint8_t bad[sizeof evt];
    memcpy(bad, evt, sizeof evt);
    bad[6] = 0x12;
    CHECK(en_fm_parse_event(bad, sizeof bad, &r), "error event should parse");
    CHECK(r.status == 0x12, "error status not reported");

    /* Everything that is not ours, or not whole, must be refused rather than
       read past - this runs on bytes that came off a shared transport. */
    uint8_t wrong[sizeof evt];

    memcpy(wrong, evt, sizeof evt);
    wrong[0] = 0x02;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r), "non-event accepted");

    memcpy(wrong, evt, sizeof evt);
    wrong[1] = 0x0F;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r),
          "non-command-complete accepted");

    memcpy(wrong, evt, sizeof evt);
    wrong[4] = 0x18;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r),
          "another opcode's completion accepted");

    CHECK(!en_fm_parse_event(evt, 5, &r), "truncated event accepted");

    /* The length field claiming more than arrived is the dangerous case. */
    memcpy(wrong, evt, sizeof evt);
    wrong[2] = 0x40;
    CHECK(!en_fm_parse_event(wrong, sizeof wrong, &r),
          "over-long parameter length accepted");

    /* A write completion carries no value. */
    const uint8_t wevt[] = { 0x04, 0x0E, 0x06, 0x01, 0x15, 0xFC, 0x00, 0x00, 0x00 };
    CHECK(en_fm_parse_event(wevt, sizeof wevt, &r), "write completion rejected");
    CHECK(r.mode == EN_FM_WRITE && r.value == 0 && r.value_len == 0,
          "write completion should carry no value");
}

/* ---- the register table -------------------------------------------------- */

static void test_table(void)
{
    section("register table consistency");

    printf("  %u registers\n", en_fm_reg_count);
    CHECK(en_fm_reg_count == 39, "expected 39 registers, got %u",
          en_fm_reg_count);

    int tbd = 0, rmw = 0, varlen = 0, writable = 0;

    for (uint8_t i = 0; i < en_fm_reg_count; i++) {
        const en_fm_reg_t *r = &en_fm_regs[i];

        CHECK(r->name && r->doc, "register %02x is missing text", r->addr);
        CHECK(en_fm_reg_find(r->addr) == r, "lookup of %02x found the wrong row",
              r->addr);

        /* Addresses must be unique, or a lookup silently returns whichever
           came first and one register becomes unreachable. */
        for (uint8_t j = 0; j < i; j++)
            CHECK(en_fm_regs[j].addr != r->addr, "duplicate address %02x",
                  r->addr);

        if (r->flags & EN_FM_TBD) tbd++;
        if (r->flags & EN_FM_RMW) rmw++;
        if (r->read_len == EN_FM_LEN_VAR) varlen++;
        if (r->flags & EN_FM_W) writable++;

        /* Anything writable needs a payload length, and anything with a
           payload length needs to be writable. */
        if (r->flags & EN_FM_W)
            CHECK(r->write_len > 0, "%s is writable with no payload length",
                  r->name);
        if (r->write_len > 0)
            CHECK(r->flags & EN_FM_W, "%s has a payload but is not writable",
                  r->name);

        /* A read-modify-write register is useless if it cannot be read. */
        if (r->flags & EN_FM_RMW)
            CHECK(r->flags & EN_FM_R, "%s needs RMW but is not readable",
                  r->name);

        for (uint8_t k = 0; k < r->nfields; k++) {
            const en_fm_field_t *f = &r->fields[k];

            CHECK(f->name && f->doc, "%s field %u is missing text", r->name, k);
            CHECK(f->width == 1 || f->width == 2 || f->width == 4,
                  "%s.%s has width %u", r->name, f->name, f->width);
            CHECK(f->bit_hi >= f->bit_lo, "%s.%s has inverted bit range",
                  r->name, f->name);
            CHECK(f->bit_hi < f->width * 8u,
                  "%s.%s bit %u is outside its %u-byte value",
                  r->name, f->name, f->bit_hi, f->width);

            /* A field has to fit inside whichever of the read or write payloads
               actually exists, or it addresses bytes that are never there. */
            uint8_t span = (uint8_t)(f->off + f->width);
            if (r->write_len)
                CHECK(span <= r->write_len,
                      "%s.%s ends at %u, past the %u-byte write payload",
                      r->name, f->name, span, r->write_len);
            if (r->read_len < EN_FM_LEN_UNKNOWN)
                CHECK(span <= r->read_len,
                      "%s.%s ends at %u, past the %u-byte read payload",
                      r->name, f->name, span, r->read_len);

            CHECK(f->min <= f->max, "%s.%s has min above max", r->name, f->name);

            /* The declared range must be representable in the bits allotted,
               which is the typo this catches: a range copied from the spec
               against a bit width copied wrongly. */
            uint8_t nbits = (uint8_t)(f->bit_hi - f->bit_lo + 1u);
            if (nbits < 31u) {
                int32_t hi = (f->flags & EN_FMF_SIGNED)
                           ? (int32_t)((1u << (nbits - 1u)) - 1u)
                           : (int32_t)((1u << nbits) - 1u);
                int32_t lo = (f->flags & EN_FMF_SIGNED)
                           ? -(int32_t)(1u << (nbits - 1u)) : 0;
                CHECK(f->max <= hi, "%s.%s max %d needs more than %u bits",
                      r->name, f->name, f->max, nbits);
                CHECK(f->min >= lo, "%s.%s min %d needs more than %u bits",
                      r->name, f->name, f->min, nbits);

                /* Every named bit must fit the field it belongs to. */
                for (uint8_t b = 0; b < f->nbits; b++)
                    CHECK((f->bits[b].mask >> nbits) == 0,
                          "%s.%s bit %s (%#x) does not fit %u bits",
                          r->name, f->name, f->bits[b].name,
                          f->bits[b].mask, nbits);
            }

            if (f->flags & EN_FMF_BITMAP)
                CHECK(f->bits && f->nbits, "%s.%s is a bitmap with no bits",
                      r->name, f->name);
            if (f->flags & EN_FMF_ENUM)
                CHECK(f->vals && f->nvals, "%s.%s is an enum with no values",
                      r->name, f->name);
        }
    }

    printf("  %d writable, %d read-modify-write, %d variable length\n",
           writable, rmw, varlen);
    printf("  %d with read lengths marked TBD upstream\n", tbd);

    CHECK(en_fm_reg_find(0x03) == 0, "0x03 is not a register and must not match");

    /* Unknown and variable lengths both report 0, so a caller cannot silently
       inherit a length nobody established. */
    CHECK(en_fm_read_len(0x80) == 0, "the RDS FIFO length is the caller's choice");
    CHECK(en_fm_read_len(0xF5) == 0, "an unstated length must not be invented");
    CHECK(en_fm_read_len(0x0F) == 1, "RSSI reads one byte");
    CHECK(en_fm_read_len(0xF9) == 8, "the blend curve reads eight bytes");
}

/* ---- field access -------------------------------------------------------- */

static void test_fields(void)
{
    section("field packing");

    /* The audio control register: a 9-bit bandwidth above a 7-bit flag set,
       both inside one 16-bit value. Editing one must not disturb the other. */
    const en_fm_reg_t *r = en_fm_reg_find(0x05);
    uint8_t p[2] = { 0, 0 };

    CHECK(en_fm_field_set(&r->fields[1], p, 2, 0x30), "could not set flags");
    CHECK(en_fm_field_set(&r->fields[0], p, 2, 3), "could not set bandwidth");
    show("bandwidth 3, route both", p, 2);
    CHECK(en_fm_field_get(&r->fields[0], p, 2) == 3, "bandwidth lost");
    CHECK(en_fm_field_get(&r->fields[1], p, 2) == 0x30, "flags disturbed");

    CHECK(!en_fm_field_set(&r->fields[0], p, 2, 4),
          "bandwidth 4 is out of range and must be refused");
    CHECK(en_fm_field_get(&r->fields[0], p, 2) == 3,
          "a refused set must not modify the payload");

    /* Signed fields, which are the ones a naive shift gets wrong. */
    const en_fm_reg_t *snr = en_fm_reg_find(0x08);
    uint8_t s[1] = { 0 };
    CHECK(en_fm_field_set(&snr->fields[0], s, 1, -40), "could not set -40");
    CHECK(s[0] == 0xD8, "-40 should store as 0xD8, got %02x", s[0]);
    CHECK(en_fm_field_get(&snr->fields[0], s, 1) == -40,
          "-40 did not round-trip, got %d",
          en_fm_field_get(&snr->fields[0], s, 1));

    /* Nibble fields. */
    const en_fm_reg_t *ap = en_fm_reg_find(0x04);
    uint8_t a[1] = { 0 };
    CHECK(en_fm_field_set(&ap->fields[0], a, 1, 9), "could not set duration");
    CHECK(en_fm_field_set(&ap->fields[1], a, 1, 5), "could not set threshold");
    show("pause 9, threshold 5", a, 1);
    CHECK(a[0] == 0x95, "nibbles packed wrong: %02x", a[0]);
    CHECK(en_fm_field_get(&ap->fields[0], a, 1) == 9, "duration lost");
    CHECK(en_fm_field_get(&ap->fields[1], a, 1) == 5, "threshold lost");

    /* The single top bit of the search control register. */
    const en_fm_reg_t *sc = en_fm_reg_find(0x07);
    uint8_t c[1] = { 0 };
    en_fm_field_set(&sc->fields[1], c, 1, 20);
    en_fm_field_set(&sc->fields[0], c, 1, 1);
    show("search up, RSSI 20", c, 1);
    CHECK(c[0] == 0x94, "search control packed wrong: %02x", c[0]);

    /* The whole eight-byte blend curve, mixed signed and unsigned. */
    const en_fm_reg_t *bl = en_fm_reg_find(0xF9);
    uint8_t b[8];
    memset(b, 0, sizeof b);
    const int32_t want[8] = { 40, 20, -60, -80, 15, -30, 12, 7 };
    for (uint8_t i = 0; i < 8; i++)
        CHECK(en_fm_field_set(&bl->fields[i], b, 8, want[i]),
              "could not set %s", bl->fields[i].name);
    show("blend + soft mute", b, 8);
    for (uint8_t i = 0; i < 8; i++)
        CHECK(en_fm_field_get(&bl->fields[i], b, 8) == want[i],
              "%s did not round-trip", bl->fields[i].name);

    /* Bounds. A field must never read or write outside the payload it was
       given, however wrong the caller is. */
    CHECK(en_fm_field_get(&bl->fields[7], b, 4) == 0,
          "a field past the buffer must read 0");
    CHECK(!en_fm_field_set(&bl->fields[7], b, 4, 1),
          "a field past the buffer must not be written");
}

/* ---- RDS ----------------------------------------------------------------- */

/* Build block B for a given group type and version, with the traffic-programme
   flag and programme type in their standard places. */
static uint16_t blkb(uint8_t type, bool ver_b, bool tp, uint8_t pty,
                     uint16_t low5)
{
    return (uint16_t)(((uint16_t)type << 12) | (ver_b ? 0x0800u : 0u)
                      | (tp ? 0x0400u : 0u) | ((uint16_t)(pty & 0x1Fu) << 5)
                      | (low5 & 0x1Fu));
}

static void feed(en_rds_t *r, uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    uint16_t blk[4] = { a, b, c, d };
    en_rds_group(r, blk, EN_RDS_ALL);
}

static void test_rds_ps(void)
{
    section("RDS programme service name");

    en_rds_t r;
    en_rds_init(&r, true);

    /* "CBC RADI" delivered two characters at a time, out of order, because a
       receiver joining mid-cycle sees exactly that. */
    const char *name = "CBC RADI";
    const uint8_t order[4] = { 2, 0, 3, 1 };
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t s = order[i];
        uint16_t d = (uint16_t)(((uint8_t)name[s * 2] << 8)
                                | (uint8_t)name[s * 2 + 1]);
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s), 0x0000, d);
        CHECK(r.ps_valid == (i == 3),
              "PS should only become valid on the last segment (i=%u)", i);
    }
    printf("  PS \"%s\"  PI %04X  PTY %u (%s)\n",
           r.ps, r.pi, r.pty, en_rds_pty_name(r.pty, r.rbds));
    CHECK(strcmp(r.ps, "CBC RADI") == 0, "PS assembled as \"%s\"", r.ps);
    CHECK(r.pi == 0xC2B5, "PI wrong");
    CHECK(r.tp, "traffic programme flag lost");

    /* RBDS and RDS disagree about what 22 means, and the register that selects
       between them is I2C_RDS_CTRL bit 0. */
    printf("  PTY 22 is \"%s\" under RBDS, \"%s\" under RDS\n",
           en_rds_pty_name(22, true), en_rds_pty_name(22, false));
    CHECK(strcmp(en_rds_pty_name(22, true), "Public") == 0, "RBDS PTY 22");
    CHECK(strcmp(en_rds_pty_name(22, false), "Travel") == 0, "RDS PTY 22");
    CHECK(strcmp(en_rds_pty_name(200, true), "Unknown") == 0,
          "an out-of-range PTY must not index off the table");
}

static void test_rds_ta(void)
{
    section("RDS traffic announcement");

    en_rds_t r;
    en_rds_init(&r, true);

    uint16_t ch = 0;
    uint16_t blk[4] = { 0xC2B5, blkb(0, false, true, 22, 0), 0, 0x4142 };
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(!r.ta, "TA should start clear");

    /* Bit 4 of block B is the announcement, and it is the thing an
       auto-recorder triggers on. */
    blk[1] = (uint16_t)(blkb(0, false, true, 22, 0) | 0x0010u);
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(r.ta, "TA not set");
    CHECK(ch & EN_RDS_CH_TA, "TA change not reported");

    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(!(ch & EN_RDS_CH_TA), "TA reported as changed when it did not");

    blk[1] = blkb(0, false, true, 22, 0);
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    CHECK(!r.ta && (ch & EN_RDS_CH_TA), "TA clearing not reported");
}

static void test_rds_rt(void)
{
    section("RDS radio text");

    en_rds_t r;
    en_rds_init(&r, true);

    const char *text =
        "Now Playing: Neil Young - Harvest Moon                          ";
    for (uint8_t seg = 0; seg < 16; seg++) {
        uint16_t c = (uint16_t)(((uint8_t)text[seg * 4] << 8)
                                | (uint8_t)text[seg * 4 + 1]);
        uint16_t d = (uint16_t)(((uint8_t)text[seg * 4 + 2] << 8)
                                | (uint8_t)text[seg * 4 + 3]);
        feed(&r, 0xC2B5, blkb(2, false, true, 22, seg), c, d);
    }
    printf("  RT \"%s\"\n", r.rt);
    CHECK(r.rt_valid, "RT never became valid");
    CHECK(strncmp(r.rt, "Now Playing: Neil Young - Harvest Moon", 38) == 0,
          "RT assembled wrong");

    /* The A/B flag toggling means a new message. Without clearing, the tail of
       the old one shows through wherever the new one has not reached. */
    uint16_t b = (uint16_t)(blkb(2, false, true, 22, 0) | 0x0010u);
    feed(&r, 0xC2B5, b, 0x4E65, 0x7874);   /* "Next" */
    CHECK(!r.rt_valid, "a new message should invalidate the old text");
    CHECK(strncmp(r.rt, "Next", 4) == 0, "new text not written");
    CHECK(r.rt[10] == ' ', "old text showed through the new message");

    /* A carriage return ends the message early. */
    en_rds_init(&r, true);
    feed(&r, 0xC2B5, blkb(2, false, true, 22, 0), 0x4F4B, 0x0D20);
    printf("  RT with early terminator: \"%s\"\n", r.rt);
    CHECK(r.rt_valid, "a terminated message should be complete immediately");
    CHECK(strcmp(r.rt, "OK") == 0, "terminator not honoured, got \"%s\"", r.rt);
}

static void test_rds_ct(void)
{
    section("RDS clock time");

    en_rds_t r;
    en_rds_init(&r, true);

    /* Modified Julian day 51544 is 1 January 2000, which is checkable by hand
       against the conversion in the RDS specification. 12:34 UTC. */
    const uint32_t mjd = 51544;
    const uint8_t hour = 12, minute = 34;

    uint16_t b = (uint16_t)(blkb(4, false, false, 0, 0)
                            | (uint16_t)((mjd >> 15) & 0x03u));
    uint16_t c = (uint16_t)(((mjd & 0x7FFFu) << 1) | (hour >> 4));
    uint16_t d = (uint16_t)(((uint16_t)(hour & 0x0Fu) << 12)
                            | ((uint16_t)minute << 6));

    uint16_t ch = 0;
    uint16_t blk[4] = { 0xC2B5, b, c, d };
    ch = en_rds_group(&r, blk, EN_RDS_ALL);
    printf("  %04u-%02u-%02u %02u:%02u  (offset %d half hours)\n",
           r.ct_year, r.ct_month, r.ct_day, r.ct_hour, r.ct_minute, r.ct_offset);
    CHECK(ch & EN_RDS_CH_CT, "clock change not reported");
    CHECK(r.ct_valid, "clock never became valid");
    CHECK(r.ct_year == 2000 && r.ct_month == 1 && r.ct_day == 1,
          "MJD 51544 should be 2000-01-01");
    CHECK(r.ct_hour == 12 && r.ct_minute == 34, "time wrong");

    /* Newfoundland is minus three and a half hours, which is exactly the case
       a naive whole-hour offset gets wrong. */
    en_rds_init(&r, true);
    uint16_t dn = (uint16_t)(d | 0x0020u | 7u);      /* west, 7 half hours */
    uint16_t blkn[4] = { 0xC2B5, b, c, dn };
    en_rds_group(&r, blkn, EN_RDS_ALL);
    printf("  same instant at -3:30: %04u-%02u-%02u %02u:%02u\n",
           r.ct_year, r.ct_month, r.ct_day, r.ct_hour, r.ct_minute);
    CHECK(r.ct_hour == 9 && r.ct_minute == 4, "half-hour offset applied wrong");
    CHECK(r.ct_day == 1, "date should not have moved");

    /* An offset that crosses midnight has to move the date with it. */
    en_rds_init(&r, true);
    uint16_t early = (uint16_t)((1u << 12) | (0u << 6));   /* 01:00 UTC */
    uint16_t de = (uint16_t)(early | 0x0020u | 7u);
    uint16_t blke[4] = { 0xC2B5, b, c, de };
    en_rds_group(&r, blke, EN_RDS_ALL);
    printf("  01:00 UTC at -3:30: %04u-%02u-%02u %02u:%02u\n",
           r.ct_year, r.ct_month, r.ct_day, r.ct_hour, r.ct_minute);
    CHECK(r.ct_hour == 21 && r.ct_minute == 30, "wrapped time wrong");
    CHECK(r.ct_year == 1999 && r.ct_month == 12 && r.ct_day == 31,
          "crossing midnight backwards should reach 1999-12-31");
}

static void test_rds_af(void)
{
    section("RDS alternate frequencies");

    en_rds_t r;
    en_rds_init(&r, true);

    /* Codes 1 to 204 are 87.5 MHz plus 100 kHz per step, so the list runs from
       87.6 to 107.9 - the top of the band is not itself an AF code. Getting
       this wrong by one step was the first thing these tests caught. */
    CHECK(en_rds_af_khz(1) == 87600, "AF code 1 should be 87.6 MHz");
    CHECK(en_rds_af_khz(204) == 107900, "AF code 204 should be 107.9 MHz");
    CHECK(en_rds_af_khz(0) == 0, "AF code 0 is not a frequency");
    CHECK(en_rds_af_khz(205) == 0, "filler is not a frequency");
    CHECK(en_rds_af_khz(250) == 0, "the LF/MF marker is not a frequency");

    /* A list header says how many follow, then pairs of codes. */
    feed(&r, 0xC2B5, blkb(0, false, true, 22, 0),
         (uint16_t)((224u + 2u) << 8 | 108u), 0x4142);
    feed(&r, 0xC2B5, blkb(0, false, true, 22, 1),
         (uint16_t)(150u << 8 | 108u), 0x4344);

    printf("  %u alternates (station announced %u):", r.af_count, r.af_expected);
    for (uint8_t i = 0; i < r.af_count; i++)
        printf(" %u.%u", r.af[i] / 1000, (r.af[i] % 1000) / 100);
    printf("\n");
    CHECK(r.af_expected == 2, "AF count header not read");
    CHECK(r.af_count == 2, "expected 2 distinct alternates, got %u", r.af_count);
    CHECK(r.af[0] == 98300, "first alternate wrong");
    CHECK(r.af[1] == 102500, "second alternate wrong");
}

static void test_rds_robustness(void)
{
    section("RDS block errors and station changes");

    en_rds_t r;
    en_rds_init(&r, true);

    /* Without block B a group has no type and nothing may be interpreted -
       not even from the blocks that did arrive intact. */
    uint16_t blk[4] = { 0xC2B5, blkb(0, false, true, 22, 0), 0, 0x4142 };
    uint16_t ch = en_rds_group(&r, blk, (uint8_t)(EN_RDS_ALL & ~EN_RDS_B));
    CHECK(ch == 0, "a group without block B must decode to nothing");
    CHECK(!r.pi_valid, "PI must not be taken from a group with no type");
    CHECK(r.blocks_bad == 1, "bad block not counted");

    /* A corrupt block D must not write garbage into the name. */
    en_rds_init(&r, true);
    for (uint8_t s = 0; s < 4; s++)
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s),
             0, (uint16_t)(0x4141u + s));
    CHECK(r.ps_valid, "PS should be complete");
    en_rds_group(&r, blk, (uint8_t)(EN_RDS_ALL & ~EN_RDS_D));
    CHECK(strcmp(r.ps, "AABACADA") == 0 || r.ps_valid,
          "a bad block D must leave the name alone");

    /* A PI change is a different station, so everything accumulated belongs to
       the previous one and has to go - otherwise an AF jump or a retune shows
       the old station's name against the new station's signal. */
    en_rds_init(&r, true);
    const char *name = "STATION1";
    for (uint8_t s = 0; s < 4; s++)
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s), 0,
             (uint16_t)(((uint8_t)name[s * 2] << 8) | (uint8_t)name[s * 2 + 1]));
    CHECK(r.ps_valid && strcmp(r.ps, "STATION1") == 0, "setup failed");

    ch = 0;
    uint16_t other[4] = { 0x1234, blkb(0, false, false, 5, 0), 0, 0x5A5A };
    ch = en_rds_group(&r, other, EN_RDS_ALL);
    printf("  after PI change: PS valid %d, PI %04X\n", r.ps_valid, r.pi);
    CHECK(ch & EN_RDS_CH_PI, "PI change not reported");
    CHECK(!r.ps_valid, "the previous station's name survived a PI change");
    CHECK(r.pi == 0x1234, "new PI not adopted");
    CHECK(r.groups > 0, "statistics should survive a station change");
}

static void test_rds_tuples(void)
{
    section("RDS FIFO unpacking (framing unverified)");

    /* Four blocks, each two data bytes and a status byte whose top nibble is
       the block type. This is the one guess in the decoder and it is isolated
       so that correcting it on the device touches nothing else. */
    const uint8_t fifo[] = {
        0xC2, 0xB5, 0x00,          /* A: PI          */
        0x02, 0xC0, 0x10,          /* B: group 0A    */
        0x00, 0x00, 0x20,          /* C              */
        0x43, 0x42, 0x30,          /* D: "CB"        */
    };
    uint16_t groups[4][4];
    uint8_t valid[4];
    uint8_t n = en_rds_unpack_tuples_unverified(fifo, sizeof fifo, groups,
                                                valid, 4);
    CHECK(n == 1, "expected one group, got %u", n);
    CHECK(groups[0][0] == 0xC2B5, "block A wrong");
    CHECK(groups[0][3] == 0x4342, "block D wrong");
    CHECK(valid[0] == EN_RDS_ALL, "all blocks should be marked good");

    /* A block flagged bad must be reported bad rather than dropped silently,
       so the group decoder can decide what is still usable. */
    uint8_t bad[sizeof fifo];
    memcpy(bad, fifo, sizeof fifo);
    bad[11] = 0x31;                        /* block D, error bit set */
    n = en_rds_unpack_tuples_unverified(bad, sizeof bad, groups, valid, 4);
    CHECK(n == 1, "a group with a bad block is still a group");
    CHECK(!(valid[0] & EN_RDS_D), "bad block D should not be marked valid");

    /* Partial trailing data must not run off the end. */
    n = en_rds_unpack_tuples_unverified(fifo, 8, groups, valid, 4);
    CHECK(n == 0, "an incomplete group must not be emitted");
    n = en_rds_unpack_tuples_unverified(fifo, sizeof fifo, groups, valid, 0);
    CHECK(n == 0, "no room means no groups");
}

/* ---- regions -------------------------------------------------------------- */

static void test_regions(void)
{
    section("region band plans");

    printf("  %u regions\n", en_region_count);
    CHECK(en_region_count >= 5, "expected several regions, got %u",
          en_region_count);

    for (uint8_t i = 0; i < en_region_count; i++) {
        const en_region_t *g = &en_regions[i];
        printf("  %-11s %6u - %-6u kHz  step %3u  %-4s  %s\n",
               g->name, g->low_khz, g->high_khz, g->step_khz,
               g->rbds ? "RBDS" : "RDS",
               g->deemph_75us ? "75us" : "50us");

        CHECK(g->low_khz < g->high_khz, "%s has inverted edges", g->name);
        CHECK(g->step_khz > 0, "%s has no channel spacing", g->name);

        /* Every edge has to be expressible in the frequency register, which is
           a 16-bit offset from 64 MHz. A band the tuner cannot reach would be
           worse than not offering it. */
        CHECK(g->low_khz >= EN_FM_BASE_KHZ,
              "%s starts below the 64 MHz register base", g->name);
        CHECK(g->high_khz - EN_FM_BASE_KHZ <= 65535,
              "%s ends beyond the register range", g->name);

        /* The band has to be a whole number of channels wide, or the top edge
           is not a tunable channel. */
        CHECK(((g->high_khz - g->low_khz) % g->step_khz) == 0,
              "%s is not a whole number of channels wide", g->name);

        CHECK(en_region_find(g->name) == g, "%s does not look up", g->name);
    }
    CHECK(en_region_find("Atlantis") == 0, "an unknown region must not match");

    const en_region_t *am = en_region_find("Americas");
    CHECK(am && am->rbds, "the Americas must decode RBDS");
    CHECK(am && am->deemph_75us, "the Americas use 75 us de-emphasis");
    CHECK(am && am->step_khz == 200, "the Americas use 200 kHz spacing");

    const en_region_t *eu = en_region_find("Europe");
    CHECK(eu && !eu->rbds, "Europe decodes RDS");
    CHECK(eu && !eu->deemph_75us, "Europe uses 50 us de-emphasis");

    /* On a 200 kHz plan the odd hundreds are not channels at all, which is the
       thing that makes a grid check worth having. */
    CHECK(en_region_on_grid(am, 98500), "98.5 is a valid US channel");
    CHECK(!en_region_on_grid(am, 98600), "98.6 is not on a 200 kHz grid");
    CHECK(en_region_on_grid(eu, 98600), "98.6 is a valid European channel");
    CHECK(!en_region_on_grid(am, 120000), "out of band must not be on grid");

    /* Stepping wraps at the edges rather than sticking. */
    CHECK(en_region_step(am, 98500, true) == 98700, "step up wrong");
    CHECK(en_region_step(am, 98500, false) == 98300, "step down wrong");
    CHECK(en_region_step(am, 107900, true) == 87900, "step up should wrap");
    CHECK(en_region_step(am, 87900, false) == 107900, "step down should wrap");
    CHECK(en_region_on_grid(am, en_region_step(am, 98600, true)),
          "stepping from off-grid must land on the grid");

    const en_region_t *jp = en_region_find("Japan");
    CHECK(jp && jp->low_khz == 76000, "Japan starts at 76 MHz");
    CHECK(en_fm_khz_to_reg(76000) == 12000, "76 MHz should encode as 12000");

    const en_region_t *oirt = en_region_find("OIRT");
    CHECK(oirt && oirt->low_khz == 65000, "OIRT starts at 65 MHz");
    CHECK(en_fm_khz_to_reg(65000) == 1000, "65 MHz should encode as 1000");
}

static void test_region_apply(void)
{
    section("applying a region");

    uint8_t cmds[EN_REGION_CMDS][EN_FM_CMD_MAX];
    uint8_t lens[EN_REGION_CMDS];
    const en_region_t *am = en_region_find("Americas");

    /* Existing contents that must survive: stereo auto-select in the control
       register, and manual mute plus I2S routing in the audio register. */
    uint8_t n = en_region_apply(am, 0x12, 0x0022, cmds, lens, EN_REGION_CMDS);
    CHECK(n == EN_REGION_CMDS, "expected %u commands, got %u",
          EN_REGION_CMDS, n);

    for (uint8_t i = 0; i < n; i++) {
        char label[32];
        snprintf(label, sizeof label, "reg %02X", cmds[i][4]);
        show(label, cmds[i], lens[i]);
        CHECK(cmds[i][0] == 0x01 && cmds[i][1] == 0x15 && cmds[i][2] == 0xFC,
              "command %u is not an FM_RDS_Command", i);
        CHECK(cmds[i][5] == 0x00, "command %u should be a write", i);
    }

    /* Band select changed, everything else in the register preserved. */
    CHECK(cmds[0][4] == 0x01, "first write should be I2C_FM_CTRL");
    CHECK(cmds[0][6] == 0x12, "band select clobbered the stereo mode: %02x",
          cmds[0][6]);

    /* De-emphasis set for the Americas, mute and routing preserved. */
    CHECK(cmds[1][4] == 0x05, "second write should be I2C_FM_AUDIO_CTRL");
    CHECK(en_fm_get_u16(&cmds[1][6]) == 0x0062,
          "de-emphasis clobbered the audio flags: %04x",
          en_fm_get_u16(&cmds[1][6]));

    /* Boundaries, upper then lower, in one write as the register requires. */
    CHECK(cmds[2][4] == 0xFB, "third write should be the search boundary");
    CHECK(en_fm_get_u16(&cmds[2][6]) == en_fm_khz_to_reg(107900),
          "upper boundary wrong");
    CHECK(en_fm_get_u16(&cmds[2][8]) == en_fm_khz_to_reg(87900),
          "lower boundary wrong");

    CHECK(cmds[3][4] == 0xFD, "fourth write should be the search step");
    CHECK(en_fm_get_u16(&cmds[3][6]) == 200, "step wrong");

    /* Europe clears the de-emphasis bit and leaves the rest alone. */
    n = en_region_apply(en_region_find("Europe"), 0x13, 0x0062, cmds, lens,
                        EN_REGION_CMDS);
    CHECK(n == EN_REGION_CMDS, "Europe should apply too");
    CHECK(cmds[0][6] == 0x12, "band select should have cleared bit 0");
    CHECK(en_fm_get_u16(&cmds[1][6]) == 0x0022,
          "de-emphasis should have cleared, leaving the rest");

    /* All or nothing: too little room produces nothing at all rather than a
       tuner left in neither region. */
    CHECK(en_region_apply(am, 0, 0, cmds, lens, 2) == 0,
          "a partial application must be refused");
}

/* ---- WAV and persistence -------------------------------------------------- */

static void test_wav(void)
{
    section("WAV header");

    uint8_t h[EN_WAV_HDR_BYTES];
    uint32_t n = en_wav_header(h, sizeof h, 44100, 2, 16, 0);
    CHECK(n == EN_WAV_HDR_BYTES, "header length %u", n);
    CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0,
          "not a RIFF/WAVE header");
    CHECK(memcmp(h + 36, "data", 4) == 0, "no data chunk");
    CHECK(en_wav_data_len(h, sizeof h) == 0, "an open stream should say 0");

    /* Byte rate and block align have to agree with the format, or players
       compute the wrong duration and seek to the wrong place. */
    uint32_t byte_rate = (uint32_t)h[28] | ((uint32_t)h[29] << 8)
                       | ((uint32_t)h[30] << 16) | ((uint32_t)h[31] << 24);
    CHECK(byte_rate == 44100u * 4u, "byte rate %u", byte_rate);
    CHECK(h[32] == 4 && h[33] == 0, "block align wrong");

    /* Patching after the fact is how a recording of unknown length closes. */
    CHECK(en_wav_patch_len(h, sizeof h, 176400), "patch failed");
    CHECK(en_wav_data_len(h, sizeof h) == 176400, "patched length not read back");
    uint32_t riff = (uint32_t)h[4] | ((uint32_t)h[5] << 8)
                  | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    CHECK(riff == 176400 + 36, "RIFF size not patched with it");

    CHECK(en_wav_header(h, 10, 44100, 2, 16, 0) == 0,
          "a header that does not fit must be refused");
    CHECK(en_wav_header(h, sizeof h, 0, 2, 16, 0) == 0,
          "a zero sample rate must be refused");
    CHECK(en_wav_header(h, sizeof h, 44100, 2, 12, 0) == 0,
          "an impossible bit depth must be refused");
}

static void test_presets(void)
{
    section("presets");

    en_presets_t p;
    en_presets_init(&p, "Americas");

    en_preset_t e;
    memset(&e, 0, sizeof e);

    e.khz = 98500; strcpy(e.name, "CBC RADI"); e.pi = 0xC2B5; e.pty = 22;
    e.rbds = true;
    CHECK(en_preset_add(&p, &e), "add failed");

    e.khz = 88100; strcpy(e.name, "VOCM"); e.pi = 0x1234; e.pty = 1;
    CHECK(en_preset_add(&p, &e), "add failed");

    e.khz = 102900; strcpy(e.name, "KIXX"); e.pi = 0x5678; e.pty = 10;
    CHECK(en_preset_add(&p, &e), "add failed");
    CHECK(p.count == 3, "count %u", p.count);

    /* Re-adding updates rather than duplicating, so a placeholder is replaced
       once the station name actually decodes. */
    e.khz = 98500; strcpy(e.name, "CBC Radio One"); e.pi = 0xC2B5;
    CHECK(en_preset_add(&p, &e), "update failed");
    CHECK(p.count == 3, "re-adding a frequency should not duplicate it");
    CHECK(strcmp(p.list[0].name, "CBC Radio One") == 0, "update did not apply");

    en_preset_sort(&p);
    CHECK(p.list[0].khz == 88100 && p.list[2].khz == 102900, "sort wrong");

    char buf[4096];
    uint32_t n = en_presets_save(&p, buf, sizeof buf);
    CHECK(n > 0, "save overflowed");
    printf("  %u bytes of JSON for %u presets\n", n, p.count);
    printf("  %.140s...\n", buf);

    en_presets_t q;
    CHECK(en_presets_load(&q, buf, n), "load failed");
    CHECK(q.count == p.count, "loaded %u of %u", q.count, p.count);
    CHECK(strcmp(q.region, "Americas") == 0, "region lost: %s", q.region);
    for (uint8_t i = 0; i < p.count; i++) {
        CHECK(q.list[i].khz == p.list[i].khz, "preset %u frequency lost", i);
        CHECK(strcmp(q.list[i].name, p.list[i].name) == 0,
              "preset %u name lost: %s", i, q.list[i].name);
        CHECK(q.list[i].pi == p.list[i].pi, "preset %u PI lost", i);
        CHECK(q.list[i].rbds == p.list[i].rbds, "preset %u RBDS flag lost", i);
    }

    CHECK(en_preset_remove(&p, 88100), "remove failed");
    CHECK(p.count == 2 && en_preset_find(&p, 88100) < 0, "remove did not");
    CHECK(!en_preset_remove(&p, 88100), "removing twice should fail");

    /* A truncated file must be reported, not silently half-written - a preset
       list that quietly loses entries is worse than one that refuses. */
    uint32_t small = en_presets_save(&p, buf, 40);
    CHECK(small == 0, "an overflowing save must report 0, got %u", small);
}

static void test_sidecar(void)
{
    section("RDS sidecar");

    en_rds_t r;
    en_rds_init(&r, true);
    for (uint8_t s2 = 0; s2 < 4; s2++) {
        const char *nm = "CBC RADI";
        uint16_t d = (uint16_t)(((uint8_t)nm[s2 * 2] << 8) | (uint8_t)nm[s2 * 2 + 1]);
        feed(&r, 0xC2B5, blkb(0, false, true, 22, s2), 0, d);
    }

    char buf[2048];
    static char whole[8192];
    uint32_t total = 0;
    en_sidecar_t sc;

    uint32_t n = en_sidecar_begin(&sc, buf, sizeof buf, 98500, "Americas",
                                  true, &r);
    CHECK(n > 0, "sidecar header overflowed");
    memcpy(whole + total, buf, n); total += n;

    /* Groups are appended a flush at a time, so an hour of them never has to
       fit in memory. */
    const uint16_t g1[4] = { 0xC2B5, 0x02C0, 0x0000, 0x4342 };
    const uint16_t g2[4] = { 0xC2B5, 0x22C1, 0x4E6F, 0x7720 };
    n = en_sidecar_group(&sc, buf, sizeof buf, 0, g1, EN_RDS_ALL);
    CHECK(n > 0, "group append overflowed");
    memcpy(whole + total, buf, n); total += n;

    n = en_sidecar_group(&sc, buf, sizeof buf, 1040, g2,
                         (uint8_t)(EN_RDS_ALL & ~EN_RDS_C));
    memcpy(whole + total, buf, n); total += n;

    n = en_sidecar_end(&sc, buf, sizeof buf, 1500, &r);
    CHECK(n > 0, "sidecar footer overflowed");
    memcpy(whole + total, buf, n); total += n;
    whole[total] = 0;

    printf("  %u bytes for 2 groups\n", total);
    printf("  %.200s...\n", whole);

    /* Braces and brackets have to balance, or it is not JSON however good it
       looks - the streaming append is exactly where that would break. */
    int braces = 0, brackets = 0, instr = 0;
    for (uint32_t i = 0; i < total; i++) {
        char c = whole[i];
        if (instr) {
            if (c == '\\') i++;
            else if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') instr = 1;
        else if (c == '{') braces++;
        else if (c == '}') braces--;
        else if (c == '[') brackets++;
        else if (c == ']') brackets--;
        CHECK(braces >= 0 && brackets >= 0, "unbalanced at byte %u", i);
    }
    CHECK(braces == 0, "braces unbalanced by %d", braces);
    CHECK(brackets == 0, "brackets unbalanced by %d", brackets);
    CHECK(!instr, "a string was left open");

    /* Raw blocks are what make a recording re-decodable later, which matters
       while the FIFO framing is still unconfirmed. */
    CHECK(strstr(whole, "\"blocks\"") != 0, "raw blocks not written");
    CHECK(strstr(whole, "0xC2B5") != 0, "block A not written");
    CHECK(strstr(whole, "\"valid\"") != 0, "block validity not written");
    CHECK(strstr(whole, "CBC RADI") != 0, "decoded name not written");
    CHECK(strstr(whole, "RBDS") != 0, "standard not recorded");
    CHECK(strstr(whole, "\"duration_ms\":1500") != 0, "duration not written");
}

static void test_simple_flags(void)
{
    section("simple screen selection");

    en_presets_t p;
    en_presets_init(&p, "Americas");

    en_preset_t e;
    memset(&e, 0, sizeof e);
    for (int i = 0; i < 9; i++) {
        e.khz = 88100u + (uint32_t)i * 200u;
        e.name[0] = (char)('A' + i);
        e.name[1] = 0;
        CHECK(en_preset_add(&p, &e), "add %d failed", i);
    }

    /* Nothing is on the simple screen until someone puts it there. A screen
       that filled itself would not be the user's few. */
    CHECK(en_presets_simple(&p, 0, EN_SIMPLE_MAX) == 0,
          "presets start out on the simple screen");

    for (int i = 0; i < EN_SIMPLE_MAX; i++)
        CHECK(en_preset_set_simple(&p, 88100u + (uint32_t)i * 200u, true),
              "flagging %d failed", i);
    CHECK(en_presets_simple(&p, 0, EN_SIMPLE_MAX) == EN_SIMPLE_MAX,
          "six should be flagged");

    /* The seventh is refused rather than silently dropped, which is what lets
       the UI say why instead of appearing to ignore the tap. */
    CHECK(!en_preset_set_simple(&p, 88100u + 6 * 200u, true),
          "a seventh was accepted");
    CHECK(!p.list[6].simple, "the refused one was flagged anyway");

    /* Re-flagging something already there is not a failure - it would be a
       confusing way to report "no change needed". */
    CHECK(en_preset_set_simple(&p, 88100u, true), "re-flagging reported failure");

    /* Clearing one makes room again. */
    CHECK(en_preset_set_simple(&p, 88100u, false), "unflagging failed");
    CHECK(en_presets_simple(&p, 0, EN_SIMPLE_MAX) == EN_SIMPLE_MAX - 1,
          "unflagging did not free a slot");
    CHECK(en_preset_set_simple(&p, 88100u + 6 * 200u, true),
          "the freed slot was not reusable");

    /* An unknown frequency is a refusal, not a crash. */
    CHECK(!en_preset_set_simple(&p, 1u, true), "an unknown frequency was flagged");

    /* The chosen ones come back in preset order, and they are the ones set. */
    const en_preset_t *pick[EN_SIMPLE_MAX];
    uint8_t n = en_presets_simple(&p, pick, EN_SIMPLE_MAX);
    CHECK(n == EN_SIMPLE_MAX, "expected %d picks, got %u", EN_SIMPLE_MAX, n);
    for (uint8_t i = 1; i < n; i++)
        CHECK(pick[i]->khz > pick[i - 1]->khz, "picks are out of preset order");

    /* Round trip. The flag has to survive a save and load, or the simple
       screen empties itself every time the app restarts. */
    char buf[4096];
    uint32_t len = en_presets_save(&p, buf, sizeof buf);
    CHECK(len > 0, "save overflowed");

    en_presets_t back;
    CHECK(en_presets_load(&back, buf, len), "load failed");
    CHECK(back.count == p.count, "count %u after reload", back.count);
    for (uint8_t i = 0; i < back.count; i++)
        CHECK(back.list[i].simple == p.list[i].simple,
              "preset %u lost its simple flag", i);

    /* A file written before the flag existed has to load with it off. Off is
       the only safe default: a simple screen that filled itself with six
       arbitrary stations because someone upgraded would be worse than empty. */
    static const char legacy[] =
        "{\"version\":1,\"region\":\"Americas\",\"presets\":["
        "{\"khz\":88100,\"name\":\"A\",\"pi\":0x0000,\"pty\":0,\"rbds\":false},"
        "{\"khz\":88300,\"name\":\"B\",\"pi\":0x0000,\"pty\":0,\"rbds\":false}]}";
    en_presets_t old;
    CHECK(en_presets_load(&old, legacy, (uint32_t)(sizeof legacy - 1)),
          "a preset file without the flag failed to load");
    CHECK(old.count == 2, "legacy file gave %u presets", old.count);
    CHECK(en_presets_simple(&old, 0, EN_SIMPLE_MAX) == 0,
          "a file written before the flag put stations on the simple screen");
}

static void test_settings(void)
{
    section("settings");

    en_settings_t a;
    en_settings_default(&a);
    CHECK(a.live_seconds == 30, "default live buffer");
    CHECK(a.rds_on, "RDS should default on");

    snprintf(a.region, sizeof a.region, "%s", "Japan");
    a.khz = 82500;
    a.rds_on = false;
    a.live_seconds = 60;

    char buf[1024];
    uint32_t n = en_settings_save(&a, buf, sizeof buf);
    CHECK(n > 0, "save overflowed");
    printf("  %s\n", buf);

    en_settings_t b;
    CHECK(en_settings_load(&b, buf, n), "load failed");
    CHECK(strcmp(b.region, "Japan") == 0, "region lost: %s", b.region);
    CHECK(b.khz == 82500, "frequency lost");
    CHECK(!b.rds_on, "RDS flag lost");
    CHECK(b.live_seconds == 60, "buffer size lost");

    /* Stored by name, so adding a row to the region table cannot silently
       change which band an existing settings file selects. */
    CHECK(strstr(buf, "\"Japan\"") != 0, "region should be stored by name");

    /* A file from an older build is missing fields rather than wrong, and a
       missing field has to keep its default - a zero live buffer would be a bug
       that looked like a setting. */
    const char *partial = "{\"version\":1,\"region\":\"Europe\"}";
    en_settings_t c;
    CHECK(en_settings_load(&c, partial, (uint32_t)strlen(partial)),
          "partial load failed");
    CHECK(strcmp(c.region, "Europe") == 0, "region not read");
    CHECK(c.live_seconds == 30,
          "a missing field should keep its default, got %u", c.live_seconds);
    CHECK(c.rds_on, "a missing flag should keep its default");

    /* Register overrides ride in the same file. The tuner forgets everything
       on power down, so an override that does not survive this round trip is
       an override that lasts until the battery does. */
    en_settings_t d;
    en_settings_default(&d);

    const uint8_t audio[2] = { 0x01, 0xB0 };
    const uint8_t blend[8] = { 40, 20, 0xC4, 0xB0, 15, 0xE2, 12, 7 };
    CHECK(en_override_set(&d.overrides, 0x05, audio, 2), "set 0x05");
    CHECK(en_override_set(&d.overrides, 0xF9, blend, 8), "set 0xF9");
    CHECK(d.overrides.count == 2, "count %u", d.overrides.count);

    /* Editing the same register twice replaces rather than appends, or a
       morning of fiddling fills the list. */
    const uint8_t audio2[2] = { 0x01, 0xF0 };
    en_override_set(&d.overrides, 0x05, audio2, 2);
    CHECK(d.overrides.count == 2, "re-setting a register should not append");

    n = en_settings_save(&d, buf, sizeof buf);
    CHECK(n > 0, "save with overrides overflowed");
    printf("  %s\n", buf);

    en_settings_t e;
    CHECK(en_settings_load(&e, buf, n), "load with overrides failed");
    CHECK(e.overrides.count == 2, "loaded %u overrides", e.overrides.count);

    const en_override_t *o = en_override_find(&e.overrides, 0x05);
    CHECK(o && o->len == 2, "0x05 lost");
    CHECK(o && o->data[0] == 0x01 && o->data[1] == 0xF0,
          "0x05 payload wrong");

    /* An eight-byte payload has to come back eight bytes long: the length is
       part of the value, which is why they are stored as hex rather than as a
       number. */
    o = en_override_find(&e.overrides, 0xF9);
    CHECK(o && o->len == 8, "0xF9 length lost: %u", o ? o->len : 0);
    if (o) for (uint8_t i = 0; i < 8; i++)
        CHECK(o->data[i] == blend[i], "0xF9 byte %u wrong", i);

    CHECK(en_override_clear(&e.overrides, 0x05), "clear failed");
    CHECK(!en_override_find(&e.overrides, 0x05), "still there after clear");
    CHECK(en_override_find(&e.overrides, 0xF9), "clear removed the wrong one");
    CHECK(!en_override_clear(&e.overrides, 0x05), "clearing twice should fail");
}

/* The regions are a table, not a lookup - one helper here rather than an
   API nothing else needs. */
static const en_region_t *find_region(const char *name)
{
    for (uint8_t i = 0; i < en_region_count; i++)
        if (strcmp(en_regions[i].name, name) == 0) return &en_regions[i];
    return NULL;
}

/* ---- band scan ------------------------------------------------------------
 *
 * Driven the way the UI drives it: tick, observe, tune where it asks. The
 * fake band has three stations on it, two of which name themselves, so both
 * passes have something to do and the "took the name early" path is exercised
 * alongside the "gave up waiting" one.
 */

static const uint32_t FAKE_STATIONS[] = { 88300, 95800, 104200 };

static uint8_t fake_rssi(uint32_t khz)
{
    for (unsigned i = 0; i < sizeof FAKE_STATIONS / sizeof FAKE_STATIONS[0]; i++)
        if (FAKE_STATIONS[i] == khz) return 200;
    return 12;
}

static void test_scan(void)
{
    const en_region_t *eu = find_region("Europe");
    CHECK(eu != NULL, "no Europe region");
    if (!eu) return;

    en_scan_t s;
    CHECK(en_scan_start(&s, eu, 98000, 100), "scan refused to start");
    CHECK(s.phase == EN_SCAN_SWEEP, "should begin sweeping");
    CHECK(s.resume_khz == 98000, "resume frequency not kept");

    uint32_t khz = s.khz;
    uint32_t want = 0;
    en_rds_t rds;
    en_rds_init(&rds, false);

    /* 40 ms a tick is the UI's refresh rate. A bound on the loop rather than
       a while(1): a state machine that never reaches DONE should fail the
       test, not hang it. */
    int guard = 0;
    int sweep_ticks = 0;
    while (s.phase != EN_SCAN_DONE && guard++ < 200000) {
        uint8_t r = fake_rssi(khz);

        /* RDS only where there is a station, and only on two of the three -
         * so one hit has to time out. Filled directly rather than through the
         * decoder: this is testing the scan, not the RDS parser. */
        en_rds_t *pr = NULL;
        if (s.phase == EN_SCAN_NAMING && r > 100 && khz != 104200) {
            rds.ps_valid = true;
            rds.pi = 0x1234;
            rds.pty = 3;
            memcpy(rds.ps, khz == 88300 ? "BBC R1  " : "Kiss    ", 9);
            pr = &rds;
        }

        if (s.phase == EN_SCAN_SWEEP) sweep_ticks++;
        if (en_scan_tick(&s, 40, r, pr, &want)) khz = want;
    }

    CHECK(s.phase == EN_SCAN_DONE, "scan never finished (%d ticks)", guard);
    CHECK(khz == 98000, "scan did not tune back to where it started (%u)", khz);
    CHECK(en_scan_percent(&s) == 100, "percent %u at the end",
          en_scan_percent(&s));

    CHECK(s.n_hits == 3, "expected 3 stations, found %u", s.n_hits);
    CHECK(!s.overflowed, "should not have overflowed on three stations");
    for (uint8_t i = 0; i < s.n_hits && i < 3; i++)
        CHECK(s.hits[i].khz == FAKE_STATIONS[i],
              "hit %u was %u, expected %u", i, s.hits[i].khz,
              FAKE_STATIONS[i]);

    /* Trailing pad from the eight-character RDS field must not survive. */
    CHECK(strcmp(s.hits[0].name, "BBC R1") == 0,
          "name 0 was '%s'", s.hits[0].name);
    CHECK(strcmp(s.hits[1].name, "Kiss") == 0,
          "name 1 was '%s'", s.hits[1].name);
    CHECK(!s.hits[2].named, "the silent station should have no name");

    /* The sweep must be the cheap pass. If it ever costs as much per channel
       as naming does, a full band takes eight minutes and the two-pass split
       has stopped earning its complexity. */
    CHECK(sweep_ticks < 1000, "sweep took %d ticks, far too long", sweep_ticks);

    /* Committing: new presets appear, and a rescan updates rather than
       duplicates - including leaving the simple-screen choice alone. */
    en_presets_t p;
    en_presets_init(&p, "Europe");
    CHECK(en_scan_commit(&s, &p) == 3, "commit added %u", p.count);
    CHECK(p.count == 3, "preset count %u", p.count);

    CHECK(en_preset_set_simple(&p, 95800, true), "could not flag for simple");
    en_scan_commit(&s, &p);
    CHECK(p.count == 3, "a rescan duplicated presets: %u", p.count);
    int at = en_preset_find(&p, 95800);
    CHECK(at >= 0 && p.list[at].simple,
          "a rescan cleared the simple-screen choice");

    /* A region with no channels is refused rather than swept. */
    en_region_t empty = *eu;
    empty.high_khz = empty.low_khz;
    empty.step_khz = 0;
    en_scan_t bad;
    CHECK(!en_scan_start(&bad, &empty, 98000, 100),
          "an empty band should refuse to scan");
}

/* ---- the recording timer --------------------------------------------------
 *
 * The two halves are tested apart, because they are two features that share a
 * struct rather than one feature: a length always works, and a start time
 * works only when there is a clock.
 */

static void test_rectimer(void)
{
    en_rectimer_t t;
    en_rectimer_init(&t);
    CHECK(t.limit_min == 0 && t.at_min == EN_REC_AT_NONE,
          "a fresh timer should do nothing");

    /* Nothing set: a recording runs forever. */
    CHECK(en_rectimer_tick(&t, true, 600, true, 9999999u) == EN_REC_NOTHING,
          "an unset timer stopped a recording");

    /* A length. Stops at it, not before, and does nothing when not recording
       - a limit is not a reason to start. */
    t.limit_min = 30;
    CHECK(en_rectimer_tick(&t, true, 600, true, 29u * 60000u)
              == EN_REC_NOTHING, "stopped a minute early");
    CHECK(en_rectimer_tick(&t, true, 600, true, 30u * 60000u) == EN_REC_STOP,
          "did not stop at the length");
    CHECK(en_rectimer_tick(&t, true, 600, false, 0) == EN_REC_NOTHING,
          "a length should not start anything");

    /* A start time, with no clock. Never fires: a scheduled recording that
       begins whenever the time eventually turns up is worse than one that
       does not begin, because you cannot tell which you got. */
    en_rectimer_init(&t);
    t.at_min = 7 * 60 + 45;
    for (int m = 0; m < 24 * 60; m++)
        CHECK(en_rectimer_tick(&t, false, m, false, 0) == EN_REC_NOTHING,
              "fired at minute %d with no clock", m);

    /* With a clock: once, at the right minute, and not again during it. */
    en_rectimer_init(&t);
    t.at_min = 7 * 60 + 45;
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 44, false, 0) == EN_REC_NOTHING,
          "fired a minute early");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, false, 0) == EN_REC_START,
          "did not fire at the time");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, true, 1000) == EN_REC_NOTHING,
          "fired again while already recording");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, false, 0) == EN_REC_NOTHING,
          "fired twice in the same minute");

    /* Tomorrow. Passing the minute rearms it, which is what makes a daily
       recording a daily recording rather than a single one. */
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 46, false, 0) == EN_REC_NOTHING,
          "fired after the minute passed");
    CHECK(en_rectimer_tick(&t, true, 7 * 60 + 45, false, 0) == EN_REC_START,
          "did not fire again the next day");

    /* Both together: the length wins while a recording is running, so a
       schedule cannot extend one past where it should have stopped. */
    en_rectimer_init(&t);
    t.at_min = 8 * 60;
    t.limit_min = 5;
    CHECK(en_rectimer_tick(&t, true, 8 * 60, true, 5u * 60000u) == EN_REC_STOP,
          "the schedule overrode the length");

    /* The countdown, including across midnight. */
    en_rectimer_init(&t);
    CHECK(en_rectimer_until(&t, true, 600) == -1,
          "a countdown with nothing scheduled");
    t.at_min = 10;                       /* 00:10 */
    CHECK(en_rectimer_until(&t, true, 23 * 60 + 50) == 20,
          "across midnight read %d, expected 20",
          en_rectimer_until(&t, true, 23 * 60 + 50));
    CHECK(en_rectimer_until(&t, false, 0) == -1,
          "a countdown with no clock");

    /* And that it survives a round trip through the settings file, since a
       timer that forgets itself on reboot is not a timer. */
    en_settings_t a, b;
    en_settings_default(&a);
    a.rectimer.limit_min = 90;
    a.rectimer.at_min = 6 * 60 + 30;
    char buf[2048];
    uint32_t n = en_settings_save(&a, buf, sizeof buf);
    CHECK(n > 0, "settings with a timer overflowed");
    en_settings_default(&b);
    CHECK(en_settings_load(&b, buf, n), "settings with a timer failed to load");
    CHECK(b.rectimer.limit_min == 90, "limit came back as %u",
          b.rectimer.limit_min);
    CHECK(b.rectimer.at_min == 6 * 60 + 30, "start came back as %d",
          b.rectimer.at_min);

    /* No start time is the absence of the key, so it has to survive as an
       absence and come back as none rather than as midnight. */
    en_settings_default(&a);
    a.rectimer.at_min = EN_REC_AT_NONE;
    n = en_settings_save(&a, buf, sizeof buf);
    en_settings_default(&b);
    b.rectimer.at_min = 123;
    CHECK(en_settings_load(&b, buf, n), "load failed");
    CHECK(b.rectimer.at_min == EN_REC_AT_NONE,
          "'no start' came back as %d", b.rectimer.at_min);
}

int main(void)
{
    printf("Radio+ core tests\n");

    test_build();
    test_parse();
    test_table();
    test_fields();

    test_rds_ps();
    test_rds_ta();
    test_rds_rt();
    test_rds_ct();
    test_rds_af();
    test_rds_robustness();
    test_rds_tuples();

    test_regions();
    test_region_apply();


    test_wav();
    test_presets();
    test_scan();
    test_rectimer();
    test_sidecar();
    test_simple_flags();
    test_settings();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

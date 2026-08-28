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

int main(void)
{
    printf("Radio+ core tests\n");

    test_build();
    test_parse();
    test_table();
    test_fields();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

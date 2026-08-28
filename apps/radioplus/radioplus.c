/*
 * clock_face — modern digital clock. Big HH:MM, blinking colon-style
 * seconds bar, weekday/date underneath, battery readout.
 * Refreshes ~twice per second from the OS RTC.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

static lv_obj_t *s_hhmm;
static lv_obj_t *s_seconds;
static lv_obj_t *s_weekday;
static lv_obj_t *s_date;
static lv_obj_t *s_battery;
static lv_obj_t *s_secbar;     /* progress bar 0..60 */
static uint32_t s_last_ms = 0;

static const char *weekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};
static const char *months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void d2(char *out, int v)
{
    int t = v < 0 ? 0 : v;
    if (t > 99) t = 99;
    out[0] = '0' + t / 10;
    out[1] = '0' + t % 10;
}

static void itoa_u(uint32_t v, char *out)
{
    char buf[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    int k = 0;
    while (i) out[k++] = buf[--i];
    out[k] = 0;
}

static void refresh(void)
{
    hb_rtc_time_t t;
    hb_rtc_read(&t);

    char hhmm[8];
    d2(hhmm,   t.hours);   hhmm[2] = ':';
    d2(hhmm+3, t.minutes); hhmm[5] = 0;
    lv_label_set_text(s_hhmm, hhmm);

    char ss[4];
    d2(ss, t.seconds); ss[2] = 0;
    lv_label_set_text(s_seconds, ss);

    lv_bar_set_value(s_secbar, t.seconds + 1, LV_ANIM_OFF);

    int wd = t.weekday;
    if (wd < 0 || wd > 6) wd = 0;
    lv_label_set_text(s_weekday, weekdays[wd]);

    int mo = t.month;
    if (mo < 1 || mo > 12) mo = 1;
    char date[40]; int k = 0;
    const char *mn = months[mo - 1];
    while (*mn) date[k++] = *mn++;
    date[k++] = ' ';
    if (t.day_of_month >= 10) date[k++] = '0' + t.day_of_month / 10;
    date[k++] = '0' + t.day_of_month % 10;
    date[k++] = ',';
    date[k++] = ' ';
    char yr[8]; itoa_u(t.year, yr);
    for (int i = 0; yr[i]; i++) date[k++] = yr[i];
    date[k] = 0;
    lv_label_set_text(s_date, date);

    uint32_t lvl = hb_battery_level_0_to_15();
    const char *icon;
    if (hb_battery_is_charging()) icon = LV_SYMBOL_CHARGE;
    else if (lvl >= 14) icon = LV_SYMBOL_BATTERY_FULL;
    else if (lvl >= 10) icon = LV_SYMBOL_BATTERY_3;
    else if (lvl >= 6)  icon = LV_SYMBOL_BATTERY_2;
    else if (lvl >= 3)  icon = LV_SYMBOL_BATTERY_1;
    else                icon = LV_SYMBOL_BATTERY_EMPTY;

    /* Voltage + charger state (carried over from the original SDK
       clock app's debug readout, useful to see at a glance). */
    uint32_t mv = hb_battery_voltage_mv();
    uint32_t cs = hb_battery_charger_state();
    static const char *chg_names[] = {
        "Unkn","Off","CurOff","Susp","EnOff","LowChg","Chg"
    };
    const char *cn = cs < 7 ? chg_names[cs] : "?";

    char bat[64]; int bi = 0;
    while (icon[bi]) { bat[bi] = icon[bi]; bi++; }
    bat[bi++] = ' ';
    char pct[8]; itoa_u(lvl * 100 / 15, pct);
    for (int i = 0; pct[i]; i++) bat[bi++] = pct[i];
    bat[bi++] = '%';
    bat[bi++] = ' '; bat[bi++] = ' ';
    char mvs[8]; itoa_u(mv, mvs);
    for (int i = 0; mvs[i]; i++) bat[bi++] = mvs[i];
    bat[bi++] = 'm'; bat[bi++] = 'V';
    bat[bi++] = ' '; bat[bi++] = ' ';
    for (int i = 0; cn[i]; i++) bat[bi++] = cn[i];
    bat[bi] = 0;
    lv_label_set_text(s_battery, bat);
}

static void on_tick(void)
{
    uint32_t now = hb_time_uptime_ms();
    if (now - s_last_ms >= 500) {
        refresh();
        s_last_ms = now;
    }
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05060a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Big HH:MM, centered upper third. Fixed near-white for high contrast on the
       always-dark (0x05060a) clock face — theme colors would blend into it. */
    s_hhmm = lv_label_create(scr);
    lv_obj_set_style_text_color(s_hhmm, lv_color_hex(0xf2f4fa), 0);
    lv_obj_set_style_text_font(s_hhmm, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_hhmm, "--:--");
    lv_obj_align(s_hhmm, LV_ALIGN_TOP_MID, 0, 80);

    /* Seconds, smaller, just below */
    s_seconds = lv_label_create(scr);
    lv_obj_set_style_text_color(s_seconds, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_seconds, &lv_font_montserrat_36, 0);
    lv_label_set_text(s_seconds, "--");
    lv_obj_align(s_seconds, LV_ALIGN_TOP_MID, 0, 150);

    /* Sec progress bar — fills once per minute */
    s_secbar = lv_bar_create(scr);
    lv_obj_set_size(s_secbar, 180, 6);
    lv_bar_set_range(s_secbar, 0, 60);
    lv_obj_align(s_secbar, LV_ALIGN_TOP_MID, 0, 210);
    lv_obj_set_style_bg_color(s_secbar, lv_color_hex(0x222531), 0);
    lv_obj_set_style_bg_color(s_secbar, lv_color_hex(hb_color_primary()),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_secbar, 3, LV_PART_INDICATOR);

    /* Weekday and date */
    s_weekday = lv_label_create(scr);
    lv_obj_set_style_text_color(s_weekday, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_weekday, &lv_font_montserrat_24, 0);
    lv_obj_align(s_weekday, LV_ALIGN_TOP_MID, 0, 240);

    s_date = lv_label_create(scr);
    lv_obj_set_style_text_color(s_date, lv_color_hex(0xb0b0b0), 0);
    lv_obj_set_style_text_font(s_date, &lv_font_montserrat_20, 0);
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 280);

    /* Battery */
    s_battery = lv_label_create(scr);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(0x60697c), 0);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_16, 0);
    lv_obj_align(s_battery, LV_ALIGN_BOTTOM_MID, 0, -32);

    refresh();
    hb_lv_set_frame_cb(on_tick);
}

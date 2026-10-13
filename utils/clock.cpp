// System wall clock: see clock.h for the design. Time source, in order:
//   1. the BL616 HBN RTC counter (32 kHz, HBN power domain). It keeps
//      counting across the GLB software reset that a MODE press ends in
//      (clock_init only sets the HBN enable bit via HBN_Enable_RTC_Counter,
//      which never clears the counter), so the clock survives MODE like the
//      user's decision asks.
//   2. the mtimer uptime, anchored at the time persisted in tangcore.cfg.
// After a power cycle the HBN counter restarts from 0, which the anchor
// check in clock_init() detects (negative or absurd jump) and ignores:
// time resumes from the last persisted anchor, i.e. it stood still while
// the console was off.
#include "clock.h"

extern "C" {
#include "bflb_mtimer.h"
#include "bflb_rtc.h"
#include "bl616_hbn.h"
}
#include <stdio.h>

#include "settings.h"
#include "utils.h"
#include "overlay.h"          // dprint

#define CLOCK_UNSET_UNIX 946684800u     // 2000-01-01 00:00:00 UTC
#define CLOCK_MAX_HBN_JUMP_S (60u * 24u * 3600u)
// The BL616 HBN counter is 40 bits at 32768 Hz: it wraps every 2^25 s.
#define CLOCK_HBN_WRAP_S (1ull << 25)

static struct bflb_device_s *rtc_dev;
static bool clk_inited;
static bool clk_set;
static uint32_t clk_base_unix;          // unix time at the base instant
static uint64_t clk_base_ms;            // mtimer ms at the base instant
static uint64_t clk_base_hbn;           // HBN ticks at the base instant

static uint64_t hbn_ticks(void) {
    return rtc_dev ? bflb_rtc_get_time(rtc_dev) : 0;
}

uint32_t clock_hbn_seconds(void) {
    return rtc_dev ? (uint32_t)(hbn_ticks() / 32768u) : 0;
}

uint32_t clock_now_unix(void) {
    if (rtc_dev)
        return clk_base_unix +
               (uint32_t)((hbn_ticks() - clk_base_hbn) / 32768u);
    return clk_base_unix +
           (uint32_t)((bflb_mtimer_get_time_ms() - clk_base_ms) / 1000u);
}

void clock_now(DateTime &dt) {
    unix_to_datetime(clock_now_unix(), dt);
}

bool clock_is_set(void) {
    return clk_set;
}

static void rebase(uint32_t unix_s) {
    clk_base_unix = unix_s;
    clk_base_ms = bflb_mtimer_get_time_ms();
    clk_base_hbn = hbn_ticks();
}

void clock_init(void) {
    rtc_dev = bflb_device_get_by_name("rtc");
    if (rtc_dev)
        HBN_Enable_RTC_Counter();   // set the enable bit only: never clears
                                    // the counter, so it survives MODE

    uint32_t saved = settings.clock_unix;
    clk_set = saved != 0;
    if (!saved)
        saved = CLOCK_UNSET_UNIX;

    rebase(saved);
    if (rtc_dev && settings.clock_unix) {
        // Did the HBN counter keep running since the anchor was saved
        // (a MODE software reset)? Then the elapsed HBN seconds are real
        // elapsed time. A power cycle restarts the counter near 0, which
        // shows up as a negative or huge jump and is ignored.
        int64_t diff = (int64_t)clock_hbn_seconds() - (int64_t)settings.clock_hbn;
        if (diff < 0)
            diff += (int64_t)CLOCK_HBN_WRAP_S;  // one 40-bit wrap at most
        if (diff >= 0 && diff <= (int64_t)CLOCK_MAX_HBN_JUMP_S)
            rebase(saved + (uint32_t)diff);
    }
    clk_inited = true;
    DateTime dt;
    clock_now(dt);
    char buf[24];
    clock_format(buf, sizeof buf, dt);
    dprint("clock: %s (%s, hbn %s)", buf, clk_set ? "set" : "unset",
           rtc_dev ? "on" : "off");
}

void clock_set(const DateTime &dt) {
    DateTime d = dt;
    datetime_clamp(d);
    clk_set = true;
    rebase(datetime_to_unix(d));
}

void clock_checkpoint(void) {
    if (!clk_inited)
        return;
    settings.clock_unix = clock_now_unix();
    settings.clock_hbn = clock_hbn_seconds();
}

void clock_send_to_core(uint8_t gba_flags) {
    uint32_t unix_s = clock_now_unix();
    DateTime dt;
    unix_to_datetime(unix_s, dt);
    uint8_t flags = (uint8_t)((gba_flags & 3u) | (clk_set ? 4u : 0u));
    taskENTER_CRITICAL();
    fpga_tx_header(0x14, 14);
    fpga_tx_byte((uint8_t)(unix_s >> 24));
    fpga_tx_byte((uint8_t)(unix_s >> 16));
    fpga_tx_byte((uint8_t)(unix_s >> 8));
    fpga_tx_byte((uint8_t)unix_s);
    fpga_tx_byte(clock_to_bcd(dt.year >= 2000 ? dt.year - 2000 : dt.year % 100));
    fpga_tx_byte(clock_to_bcd(dt.mon));
    fpga_tx_byte(clock_to_bcd(dt.day));
    fpga_tx_byte(clock_to_bcd(dt.wday));
    fpga_tx_byte(clock_to_bcd(dt.hour));
    fpga_tx_byte(clock_to_bcd(dt.min));
    fpga_tx_byte(clock_to_bcd(dt.sec));
    fpga_tx_byte(settings.solar_pct);
    fpga_tx_byte(flags);
    taskEXIT_CRITICAL();
}

void clock_format(char *buf, size_t n, const DateTime &dt) {
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.mon,
             dt.day, dt.hour, dt.min, dt.sec);
}

void clock_format_short(char *buf, size_t n, const DateTime &dt) {
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d", dt.year, dt.mon, dt.day,
             dt.hour, dt.min);
}

uint8_t clock_to_bcd(int v) {
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

int clock_from_bcd(uint8_t b) {
    return (b >> 4) * 10 + (b & 0xf);
}

/////////////////////////////////////////////////////////////////////////////////
// Civil <-> unix (days-from-civil, after Howard Hinnant's public-domain
// algorithms; valid for every date the editor can produce and far beyond).

static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);                       // [0, 399]
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      // [0, 146096]
    return era * 146097 + doe - 719468;                   // days since 1970-01-01
}

static void civil_from_days(int64_t z, int &y, int &m, int &d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);                    // [0, 146096]
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);    // [0, 365]
    int mp = (5 * doy + 2) / 153;                         // [0, 11]
    d = doy - (153 * mp + 2) / 5 + 1;                     // [1, 31]
    m = mp + (mp < 10 ? 3 : -9);                          // [1, 12]
    y = (int)(yy + (m <= 2));
}

static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int datetime_days_in_month(int year, int mon) {
    static const int days[12] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
    if (mon < 1 || mon > 12)
        return 31;
    return days[mon - 1] + (mon == 2 && is_leap(year) ? 1 : 0);
}

void datetime_clamp(DateTime &dt) {
    if (dt.year < 2000) dt.year = 2000;
    if (dt.year > 2099) dt.year = 2099;
    if (dt.mon < 1) dt.mon = 1;
    if (dt.mon > 12) dt.mon = 12;
    int dim = datetime_days_in_month(dt.year, dt.mon);
    if (dt.day < 1) dt.day = 1;
    if (dt.day > dim) dt.day = dim;
    if (dt.hour < 0) dt.hour = 0;
    if (dt.hour > 23) dt.hour = 23;
    if (dt.min < 0) dt.min = 0;
    if (dt.min > 59) dt.min = 59;
    if (dt.sec < 0) dt.sec = 0;
    if (dt.sec > 59) dt.sec = 59;
    int64_t days = days_from_civil(dt.year, dt.mon, dt.day);
    dt.wday = (int)((days + 4) % 7);        // 1970-01-01 was a Thursday
    if (dt.wday < 0) dt.wday += 7;
}

uint32_t datetime_to_unix(const DateTime &dt) {
    int64_t days = days_from_civil(dt.year, dt.mon, dt.day);
    int64_t s = days * 86400 + dt.hour * 3600 + dt.min * 60 + dt.sec;
    return s > 0 ? (uint32_t)s : 0;
}

void unix_to_datetime(uint32_t unix_s, DateTime &dt) {
    int64_t days = (int64_t)(unix_s / 86400u);
    uint32_t rem = unix_s % 86400u;
    civil_from_days(days, dt.year, dt.mon, dt.day);
    dt.wday = (int)((days + 4) % 7);
    dt.hour = (int)(rem / 3600u);
    dt.min = (int)((rem / 60u) % 60u);
    dt.sec = (int)(rem % 60u);
}

#pragma once

#include <stddef.h>
#include <stdint.h>

// The system wall clock. The console has no clock that runs while powered
// off, so the firmware keeps time in software: a unix anchor persisted in
// tangcore.cfg plus a running counter (the BL616 HBN RTC when available,
// the mtimer uptime otherwise). Time therefore stands still while the
// console is off and resumes from the last persisted anchor; the user sets
// it in Options -> Date & time. See clock.cpp.

struct DateTime {
    int year;                   // full year, e.g. 2026
    int mon;                    // 1..12
    int day;                    // 1..31
    int wday;                   // 0 = Sunday .. 6 = Saturday
    int hour, min, sec;         // 0..23, 0..59, 0..59
};

// Civil <-> unix conversions (UTC, no time zones anywhere in TangCore).
uint32_t datetime_to_unix(const DateTime &dt);
void unix_to_datetime(uint32_t unix_s, DateTime &dt);
int datetime_days_in_month(int year, int mon);
void datetime_clamp(DateTime &dt);      // bring every field back in range

uint8_t clock_to_bcd(int v);            // 0..99 -> packed BCD
int clock_from_bcd(uint8_t b);

// Call once after settings_load(). Picks up the persisted anchor and the
// HBN counter (which survives a MODE software reset, so time survives one
// too); falls back to the anchor + uptime after a power cycle.
void clock_init(void);

uint32_t clock_now_unix(void);
void clock_now(DateTime &dt);
bool clock_is_set(void);                // the user set it at least once

// Set the clock (Options editor). Updates the in-RAM anchors; the caller
// persists them with settings_save().
void clock_set(const DateTime &dt);

// Refresh settings.clock_unix / clock_hbn with the current anchors.
// settings_save() calls it, so every settings write is also a clock
// checkpoint.
void clock_checkpoint(void);

// Current HBN RTC counter in seconds, or 0 when there is no RTC device.
// Exposed for clock_checkpoint(); also handy in diagnostics.
uint32_t clock_hbn_seconds(void);

// Send frame 0x14 (unix + BCD datetime + solar + flags) to the running
// core. gba_flags bit0 = cart has RTC, bit1 = cart has solar sensor
// (core/gba.cpp); pass 0 for other cores. Old bitstreams ignore the frame.
void clock_send_to_core(uint8_t gba_flags);

// "2026-10-13 12:34:05" (19 chars + NUL). Also the Options row format
// without the seconds when n is small: see clock_format_short.
void clock_format(char *buf, size_t n, const DateTime &dt);
void clock_format_short(char *buf, size_t n, const DateTime &dt); // "2026-10-13 12:34"

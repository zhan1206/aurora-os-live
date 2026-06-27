/*
 * rtc.h - CMOS Real-Time Clock driver interface
 */
#ifndef RTC_H
#define RTC_H

#include <stddef.h>
#include <stdint.h>

/* RTC time structure */
struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

/* Initialize the RTC subsystem */
void rtc_init(void);

/* Read current time from CMOS RTC.
 * Returns 0 on success, -1 if RTC is not updating or data is invalid. */
int rtc_read_time(struct rtc_time *tm);

/* Get system uptime in seconds since boot */
uint64_t rtc_get_uptime_seconds(void);

/* Called by PIT interrupt handler to update uptime counter.
 * Should be called every 100 ticks (once per second at 100 Hz). */
void rtc_tick_update(void);

/* Format time as "HH:MM" string (e.g., "14:30").
 * buf must be at least 6 bytes. Returns 0 on success, -1 on RTC error. */
int rtc_format_time(char *buf, size_t bufsize);

/* Format date as "YYYY-MM-DD  DayOfWeek" string (e.g., "2026-06-19  Fri").
 * buf must be at least 32 bytes. Uses Zeller's formula for day of week.
 * Returns 0 on success, -1 on RTC error. */
int rtc_format_date(char *buf, size_t bufsize);

/* Get time as seconds + microseconds since epoch (for gettimeofday).
 * Fills tv_sec and tv_usec with the current time approximated from RTC. */
void rtc_get_timeval(uint64_t *tv_sec, uint64_t *tv_usec);

#endif /* RTC_H */
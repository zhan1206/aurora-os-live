/*
 * rtc.c - CMOS Real-Time Clock driver
 *
 * Reads time from the CMOS RTC via I/O ports 0x70 (index) and 0x71 (data).
 * Handles BCD-to-binary conversion and waits for RTC update completion.
 *
 * RTC register map:
 *   0x00 - Seconds
 *   0x02 - Minutes
 *   0x04 - Hours
 *   0x07 - Day of month
 *   0x08 - Month
 *   0x09 - Year (last two digits)
 *   0x0A - Status Register A
 *   0x0B - Status Register B
 */
#include "rtc.h"
#include "include/portio.h"
#include "include/log.h"
#include "include/kstdio.h"
#include <stdint.h>

/* RTC I/O ports */
#define RTC_INDEX  0x70
#define RTC_DATA   0x71

/* Status Register B flags */
#define RTC_B_24H  0x02    /* 24-hour mode */
#define RTC_B_BIN  0x04    /* Binary mode (vs BCD) */

/* Uptime tracking — incremented by PIT interrupt */
static volatile uint64_t g_uptime_seconds = 0;

/* RTC update flag */
static int g_rtc_ready = 0;

/* Check if RTC is currently updating */
static int rtc_is_updating(void) {
    outb(RTC_INDEX, 0x0A);
    return (inb(RTC_DATA) & 0x80) ? 1 : 0;
}

/* Read a CMOS register */
static uint8_t rtc_read_reg(uint8_t reg) {
    outb(RTC_INDEX, reg);
    return inb(RTC_DATA);
}

/* Convert BCD to binary */
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

void rtc_init(void) {
    /* Read Status Register B to determine format */
    uint8_t status_b = rtc_read_reg(0x0B);
    int is_binary = (status_b & RTC_B_BIN) ? 1 : 0;
    int is_24h = (status_b & RTC_B_24H) ? 1 : 0;

    log_printf(LOG_LEVEL_INFO, "rtc: CMOS RTC detected (%s, %s)\n",
               is_24h ? "24h" : "12h",
               is_binary ? "binary" : "BCD");

    g_rtc_ready = 1;
}

int rtc_read_time(struct rtc_time *tm) {
    if (!tm || !g_rtc_ready) return -1;

    /* Wait until RTC is not in the middle of an update */
    int timeout = 10000;
    while (rtc_is_updating() && --timeout > 0) {
        /* spin */
    }
    if (timeout <= 0) return -1;

    /* Read Status Register B to determine format */
    uint8_t status_b = rtc_read_reg(0x0B);
    int is_binary = (status_b & RTC_B_BIN) ? 1 : 0;

    /* Read all time registers */
    uint8_t second = rtc_read_reg(0x00);
    uint8_t minute = rtc_read_reg(0x02);
    uint8_t hour   = rtc_read_reg(0x04);
    uint8_t day    = rtc_read_reg(0x07);
    uint8_t month  = rtc_read_reg(0x08);
    uint8_t year   = rtc_read_reg(0x09);

    /* Convert BCD to binary if needed */
    if (!is_binary) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = bcd_to_bin(hour);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }

    /* Fill output structure */
    tm->second = second;
    tm->minute = minute;
    tm->hour   = hour;
    tm->day    = day;
    tm->month  = month;
    tm->year   = (uint16_t)year + 2000;  /* CMOS year is 0-99, assume 2000+ */

    return 0;
}

uint64_t rtc_get_uptime_seconds(void) {
    return g_uptime_seconds;
}

/* Called from PIT tick handler to increment uptime counter */
void rtc_tick_update(void) {
    /* PIT runs at 100 Hz, so increment every 100 ticks */
    static uint32_t tick_counter = 0;
    tick_counter++;
    if (tick_counter >= 100) {
        tick_counter = 0;
        g_uptime_seconds++;
    }
}

/* ================================================================
 * rtc_format_time: Format current time as "HH:MM"
 *
 * Used by shell's lock/login screens to avoid duplicate formatting
 * code. Uses stack buffer and itoa for efficient conversion.
 * ================================================================ */
int rtc_format_time(char *buf, size_t bufsize) {
    if (!buf || bufsize < 6) return -1;
    struct rtc_time tm;
    if (rtc_read_time(&tm) != 0) return -1;

    int n = 0;
    if (tm.hour < 10) buf[n++] = '0';
    n += itoa(tm.hour, buf + n, bufsize - (size_t)n);
    buf[n++] = ':';
    if (tm.minute < 10) buf[n++] = '0';
    n += itoa(tm.minute, buf + n, bufsize - (size_t)n);
    buf[n] = '\0';
    return 0;
}

/* ================================================================
 * rtc_format_date: Format current date as "YYYY-MM-DD  DayOfWeek"
 *
 * Uses Zeller's formula for day-of-week calculation.
 * Day names: Sun, Mon, Tue, Wed, Thu, Fri, Sat.
 * ================================================================ */
int rtc_format_date(char *buf, size_t bufsize) {
    if (!buf || bufsize < 32) return -1;
    struct rtc_time tm;
    if (rtc_read_time(&tm) != 0) return -1;

    const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    /* Zeller's formula for day of week */
    int m = tm.month, y = tm.year;
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100, j = y / 100;
    int dow = (tm.day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    if (dow < 0) dow += 7;
    int dow_idx = (dow + 1) % 7;  /* Zeller: 0=Sat→days[0]=Sun */

    int n = 0;
    n += itoa(tm.year, buf + n, bufsize - (size_t)n);
    buf[n++] = '-';
    if (tm.month < 10) buf[n++] = '0';
    n += itoa(tm.month, buf + n, bufsize - (size_t)n);
    buf[n++] = '-';
    if (tm.day < 10) buf[n++] = '0';
    n += itoa(tm.day, buf + n, bufsize - (size_t)n);
    buf[n++] = ' ';
    buf[n++] = ' ';
    for (int i = 0; i < 3 && days[dow_idx][i]; i++) buf[n++] = days[dow_idx][i];
    buf[n] = '\0';
    return 0;
}
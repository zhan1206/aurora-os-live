/*
 * perf.c - Performance counter implementation for AuroraOS kernel
 *
 * Uses RDTSC for high-resolution timing. Calibrates TSC frequency
 * using the PIT (Programmable Interval Timer) at init time.
 * All counter operations use atomic __sync_fetch_and_add for
 * SMP safety.
 */

#include "perf.h"
#include "include/log.h"
#include "include/portio.h"
#include "include/kstdio.h"
#include "include/theme.h"
#include "console.h"
#include <string.h>

/* ================================================================
 * Global state
 * ================================================================ */
struct perf_stats perf;
static uint64_t tsc_freq_hz = 0;   /* calibrated TSC ticks per second */

/* ================================================================
 * per event name strings
 * ================================================================ */
static const char *perf_event_names[PERF_MAX] = {
    "ctx_switches",
    "syscall_count",
    "syscall_latency",
    "page_faults",
    "cow_count",
    "malloc_count",
    "free_count",
    "irq_count",
    "vruntime_updates"
};

/* ================================================================
 * RDTSC helper
 * ================================================================ */
static inline uint64_t read_tsc(void) {
    uint32_t low, high;
    asm volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

/* ================================================================
 * TSC frequency calibration via PIT
 *
 * Uses the PIT channel 0 in one-shot mode to measure a known
 * time interval. The PIT base frequency is 1193180 Hz.
 *
 * Procedure:
 *   1. Set PIT to one-shot mode with a divisor that gives ~10ms
 *   2. Read TSC
 *   3. Poll PIT output pin (bit 5 of port 0x61) until it goes high
 *   4. Read TSC
 *   5. tsc_freq = (tsc_diff * 1193180) / divisor
 * ================================================================ */
static void tsc_calibrate(void) {
    /*
     * PIT calibration: use divisor 11930 (≈10ms at 1193180 Hz).
     * Set channel 0 to mode 0 (interrupt on terminal count),
     * load divisor, then poll the output pin.
     *
     * PIT ports:
     *   0x40 = channel 0 data
     *   0x43 = mode/command register
     * Mode 0 command: 0x30 = channel 0, lobyte/hibyte, mode 0, binary
     *
     * PIT output pin status: bit 5 of port 0x61 (NMI Status and Control)
     *   When bit 5 = 0, count is in progress. When bit 5 = 1, count done.
     *
     * We use a conservative approach: measure multiple ~10ms intervals
     * and take the average for better accuracy.
     */

    uint16_t divisor = 11930;  /* ~10ms */
    uint64_t best_tsc_freq = 0;

    /* Calibrate over 3 samples and take the average */
    for (int sample = 0; sample < 3; sample++) {
        /* Set PIT channel 0 to mode 0 (interrupt on terminal count) */
        outb(0x43, 0x30);  /* channel 0, lobyte/hibyte, mode 0 */

        /* Load divisor */
        outb(0x40, (uint8_t)(divisor & 0xFF));
        outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

        /* Read TSC at start */
        uint64_t tsc_start = read_tsc();

        /* Poll until PIT output goes high (count reached 0) */
        uint32_t timeout = 0;
        while (!(inb(0x61) & 0x20)) {
            timeout++;
            if (timeout > 10000000) {
                log_printf(LOG_LEVEL_WARN, "perf: TSC calibration PIT timeout, skipping sample\n");
                goto next_sample;  /* skip this sample, don't use bad data */
            }
        }

        /* Read TSC at end */
        uint64_t tsc_end = read_tsc();
        uint64_t tsc_diff = tsc_end - tsc_start;

        /* Skip if TSC didn't advance (shouldn't happen normally) */
        if (tsc_diff == 0) {
            log_printf(LOG_LEVEL_WARN, "perf: TSC calibration zero diff, skipping\n");
            goto next_sample;
        }

        /* Calculate TSC frequency: tsc_diff ticks in (divisor / 1193180) seconds.
         * tsc_freq = tsc_diff * 1193180 / divisor */
        uint64_t sample_freq = (tsc_diff * 1193180ULL) / divisor;
        best_tsc_freq += sample_freq;
    next_sample:
        continue;
    }

    tsc_freq_hz = best_tsc_freq / 3;

    if (tsc_freq_hz == 0) {
        /* Fallback: assume 2 GHz if calibration fails */
        tsc_freq_hz = 2000000000ULL;
        log_printf(LOG_LEVEL_WARN, "perf: TSC calibration failed, using %d MHz\n",
                   (int)(tsc_freq_hz / 1000000));
    } else {
        log_printf(LOG_LEVEL_INFO, "perf: TSC calibrated at %d MHz\n",
                   (int)(tsc_freq_hz / 1000000));
    }
}

/* ================================================================
 * perf_init: Initialize all counters and calibrate TSC
 * ================================================================ */
void perf_init(void) {
    memset(&perf, 0, sizeof(perf));

    /* Set up counter names */
    for (int i = 0; i < PERF_MAX; i++) {
        perf.counters[i].name = perf_event_names[i];
        perf.counters[i].min_latency = UINT64_MAX;
    }

    /* Initialize IRQ counters */
    for (int i = 0; i < IRQ_MAX_VECTORS; i++) {
        perf.irq_counts[i].count = 0;
        perf.irq_counts[i].name = NULL;
    }

    /* Record boot time via RDTSC */
    perf.boot_time = read_tsc();

    /* Calibrate TSC frequency using PIT */
    tsc_calibrate();

    log_printf(LOG_LEVEL_INFO, "perf: performance counters initialized\n");
}

/* ================================================================
 * perf_inc: Atomic increment of a counter
 * ================================================================ */
void perf_inc(int event) {
    if (event < 0 || event >= PERF_MAX) return;
    __sync_fetch_and_add(&perf.counters[event].count, 1);
}

/* ================================================================
 * perf_add_latency: Add latency measurement, update min/max
 *
 * Uses atomic operations for SMP safety. The min/max update
 * uses a compare-and-swap loop.
 * ================================================================ */
void perf_add_latency(int event, uint64_t latency_ns) {
    if (event < 0 || event >= PERF_MAX) return;

    struct perf_counter *c = &perf.counters[event];

    __sync_fetch_and_add(&c->count, 1);
    __sync_fetch_and_add(&c->total_latency, latency_ns);

    /* Update max_latency atomically */
    uint64_t old_max, new_max;
    do {
        old_max = c->max_latency;
        new_max = (latency_ns > old_max) ? latency_ns : old_max;
    } while (!__sync_bool_compare_and_swap(&c->max_latency, old_max, new_max));

    /* Update min_latency atomically */
    uint64_t old_min, new_min;
    do {
        old_min = c->min_latency;
        new_min = (latency_ns < old_min) ? latency_ns : old_min;
    } while (!__sync_bool_compare_and_swap(&c->min_latency, old_min, new_min));
}

/* ================================================================
 * perf_dump: Formatted output of all performance counters
 * ================================================================ */
void perf_dump(void) {
    console_write_ansi(CLR_PRIMARY_BOLD);
    console_write("=== Performance Statistics ===\n");
    console_write_ansi(SGR_RESET);

    /* Uptime */
    uint64_t uptime_ns = perf_ticks_to_ns(perf.uptime_ticks);
    console_write("  Uptime: ");
    {
        char buf[24];
        uint64_t sec = uptime_ns / 1000000000ULL;
        uint64_t min = sec / 60;
        uint64_t hr  = min / 60;
        sec = sec % 60;
        min = min % 60;
        console_write_ansi(CLR_WARN);
        uitoa(hr, buf, sizeof(buf));
        console_write(buf);
        console_write("h ");
        uitoa(min, buf, sizeof(buf));
        console_write(buf);
        console_write("m ");
        uitoa(sec, buf, sizeof(buf));
        console_write(buf);
        console_write("s");
        console_write_ansi(SGR_RESET);
    }
    console_putc('\n');

    /* TSC frequency */
    console_write("  TSC Freq: ");
    {
        char buf[24];
        uitoa(tsc_freq_hz / 1000000, buf, sizeof(buf));
        console_write(buf);
        console_write(" MHz\n");
    }

    console_putc('\n');

    /* Counter table header */
    console_write_ansi(CLR_MUTED);
    console_write("  Counter              Count       Avg Latency    Min      Max\n");
    console_write("  ");
    for (int i = 0; i < 62; i++) console_putc(0xC4);
    console_putc('\n');
    console_write_ansi(SGR_RESET);

    for (int i = 0; i < PERF_MAX; i++) {
        struct perf_counter *c = &perf.counters[i];

        /* Name (padded to 20 chars) */
        console_write("  ");
        console_write_ansi(CLR_PRIMARY);
        console_write(c->name);
        console_write_ansi(SGR_RESET);

        int namelen = 0;
        for (const char *p = c->name; *p; p++) namelen++;
        for (int j = namelen; j < 20; j++) console_putc(' ');

        /* Count */
        {
            char buf[24];
            uitoa(c->count, buf, sizeof(buf));
            console_write("  ");
            console_write_ansi(CLR_SUCCESS);
            console_write(buf);
            console_write_ansi(SGR_RESET);
            int clen = 0;
            for (const char *p = buf; *p; p++) clen++;
            for (int j = clen; j < 12; j++) console_putc(' ');
        }

        /* Average latency (only for counters with count > 0) */
        if (c->total_latency > 0 && c->count > 0) {
            uint64_t avg = c->total_latency / c->count;
            char buf[24];
            uitoa(avg, buf, sizeof(buf));
            console_write(buf);
            console_write(" ns");
            int alen = 0;
            for (const char *p = buf; *p; p++) alen++;
            for (int j = alen + 3; j < 14; j++) console_putc(' ');
        } else {
            console_write_ansi(CLR_MUTED);
            console_write("    --");
            console_write_ansi(SGR_RESET);
            console_write("        ");
        }

        /* Min latency */
        if (c->min_latency != UINT64_MAX) {
            char buf[24];
            uitoa(c->min_latency, buf, sizeof(buf));
            console_write(buf);
            console_write(" ns");
            int mlen = 0;
            for (const char *p = buf; *p; p++) mlen++;
            for (int j = mlen + 3; j < 10; j++) console_putc(' ');
        } else {
            console_write_ansi(CLR_MUTED);
            console_write("--");
            console_write_ansi(SGR_RESET);
            console_write("       ");
        }

        /* Max latency */
        if (c->max_latency > 0) {
            char buf[24];
            uitoa(c->max_latency, buf, sizeof(buf));
            console_write(buf);
            console_write(" ns");
        } else {
            console_write_ansi(CLR_MUTED);
            console_write("--");
            console_write_ansi(SGR_RESET);
        }

        console_putc('\n');
    }

    console_write_ansi(CLR_MUTED);
    console_write("  ");
    for (int i = 0; i < 62; i++) console_putc(0xC4);
    console_putc('\n');
    console_write_ansi(SGR_RESET);
}

/* ================================================================
 * perf_reset: Reset all counters to zero
 * ================================================================ */
void perf_reset(void) {
    for (int i = 0; i < PERF_MAX; i++) {
        perf.counters[i].count = 0;
        perf.counters[i].total_latency = 0;
        perf.counters[i].max_latency = 0;
        perf.counters[i].min_latency = UINT64_MAX;
    }
    console_write_ansi(CLR_SUCCESS);
    console_write("Performance counters reset.\n");
    console_write_ansi(SGR_RESET);
}

/* ================================================================
 * perf_get_ticks: Return current uptime in ticks
 * ================================================================ */
uint64_t perf_get_ticks(void) {
    return perf.uptime_ticks;
}

/* ================================================================
 * perf_ticks_to_ns: Convert ticks to nanoseconds
 *
 * Uses the calibrated TSC frequency. Since we track uptime in
 * ticks (which are incremented by the timer interrupt at a known
 * rate), we convert based on the timer frequency.
 * ================================================================ */
uint64_t perf_ticks_to_ns(uint64_t ticks) {
    /*
     * The uptime_ticks is incremented by the PIT/APIC timer at 100 Hz.
     * So each tick = 10 ms = 10,000,000 ns.
     */
    return ticks * 10000000ULL;
}

/* ================================================================
 * perf_tsc_to_ns: Convert raw TSC delta to nanoseconds
 *
 * Uses the calibrated TSC frequency. This is the correct function
 * to use for RDTSC-based latency measurements.
 * ================================================================ */
uint64_t perf_tsc_to_ns(uint64_t tsc) {
    if (tsc_freq_hz == 0) return 0;
    /* Bug #40: large TSC values can overflow when multiplied by 1e9.
     * Use 128-bit arithmetic via __int128 if available, otherwise
     * check for overflow before multiplication. */
    if (tsc > UINT64_MAX / 1000000000ULL) {
        /* Overflow would occur; fall back to division-first approach */
        return (tsc / tsc_freq_hz) * 1000000000ULL
             + ((tsc % tsc_freq_hz) * 1000000000ULL) / tsc_freq_hz;
    }
    return (tsc * 1000000000ULL) / tsc_freq_hz;
}

/* ================================================================
 * IRQ counter tracking (like CoolPotOS /proc/interrupts)
 * ================================================================ */

void perf_irq_inc(int vector, const char *name) {
    if (vector < 0 || vector >= IRQ_MAX_VECTORS) return;
    __sync_fetch_and_add(&perf.irq_counts[vector].count, 1);
    if (name && !perf.irq_counts[vector].name) {
        perf.irq_counts[vector].name = name;
    }
}

void perf_irq_dump(void) {
    console_write_ansi(CLR_PRIMARY_BOLD);
    console_write("           CPU0       \n");
    console_write_ansi(SGR_RESET);

    int has_any = 0;
    for (int i = 0; i < IRQ_MAX_VECTORS; i++) {
        if (perf.irq_counts[i].count > 0 || perf.irq_counts[i].name) {
            has_any = 1;
            break;
        }
    }

    if (!has_any) {
        console_write_ansi(CLR_MUTED);
        console_write("  No interrupts recorded\n");
        console_write_ansi(SGR_RESET);
        return;
    }

    for (int i = 0; i < IRQ_MAX_VECTORS; i++) {
        if (perf.irq_counts[i].count > 0 || perf.irq_counts[i].name) {
            char buf[24];
            char num[8];
            itoa(i, num, sizeof(num));

            console_write("  ");
            console_write_ansi(CLR_INFO);
            console_write(num);
            console_write_ansi(SGR_RESET);
            console_write(": ");

            const char *name = perf.irq_counts[i].name;
            if (name) {
                console_write(name);
                int nlen = 0;
                for (const char *p = name; *p; p++) nlen++;
                for (int j = nlen; j < 20; j++) console_putc(' ');
            } else {
                console_write_ansi(CLR_MUTED);
                console_write("unknown");
                console_write_ansi(SGR_RESET);
                console_write("             ");
            }

            console_write_ansi(CLR_SUCCESS);
            uitoa(perf.irq_counts[i].count, buf, sizeof(buf));
            console_write(buf);
            console_write_ansi(SGR_RESET);
            console_putc('\n');
        }
    }
}
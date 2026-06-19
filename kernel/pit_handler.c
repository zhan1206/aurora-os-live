/*
 * pit_handler.c - PIT/APIC timer interrupt handler (UPDATED for SMP)
 *
 * Fix: Instead of calling schedule() directly from interrupt context
 * (which breaks the iretq return path), set a need_resched flag.
 * The flag is checked at safe points: after iretq returns to the
 * interrupted context, and in yield() / explicit reschedule points.
 *
 * SMP: Periodically triggers load balancing (every ~1 second, i.e.,
 * every 100 ticks at 100 Hz).
 */
#include "include/log.h"
#include "sched.h"
#include "perf.h"

/* Reschedule flag: set by interrupt handlers, checked at safe points */
volatile int need_resched = 0;

/* SMP load balancing counter */
static int smp_balance_counter = 0;
#define SMP_BALANCE_INTERVAL 100  /* every 100 ticks (~1 sec at 100 Hz) */

void pit_irq_c_handler(void *rsp) {
    (void)rsp;
    /* Performance counter: count each timer interrupt */
    perf_inc(PERF_IRQ_COUNT);

    /* Update uptime ticks */
    __sync_fetch_and_add(&perf.uptime_ticks, 1);

    /* Set flag instead of calling schedule() directly.
     * The interrupted code will check need_resched at its next
     * safe point (iretq return path in the IRQ wrapper). */
    need_resched = 1;

    /* Periodically run SMP load balancing */
    smp_balance_counter++;
    if (smp_balance_counter >= SMP_BALANCE_INTERVAL) {
        smp_balance_counter = 0;
        /* Run load balancing for CPU 0 (BSP).
         * On SMP, the APIC timer handler on each CPU will also
         * do per-CPU balancing. */
        smp_schedule(0);
    }
}

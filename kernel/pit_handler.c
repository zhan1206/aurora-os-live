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
 *
 * PREEMPTIVE SCHEDULING: Each tick decrements the current task's
 * time_slice. When time_slice reaches 0, need_resched is set and
 * the task is preempted at the next safe point. The task's vruntime
 * is updated to account for the consumed CPU time.
 */
#include "include/log.h"
#include "sched.h"
#include "smp.h"
#include "perf.h"
#include "rtc.h"

/* Reschedule flag: set by interrupt handlers, checked at safe points.
 * SMP: use atomic operations to ensure visibility across cores. */
volatile int need_resched = 0;

/* SMP load balancing counter — atomic increment for correctness */
static volatile int smp_balance_counter = 0;
#define SMP_BALANCE_INTERVAL 100  /* every 100 ticks (~1 sec at 100 Hz) */

/* Preemption tick counter for time slice accounting */
static volatile int preempt_tick = 0;

void pit_irq_c_handler(void *rsp) {
    (void)rsp;
    /* Performance counter: count each timer interrupt */
    perf_inc(PERF_IRQ_COUNT);

    /* Update uptime ticks */
    uint64_t tick = __sync_fetch_and_add(&perf.uptime_ticks, 1);

    /* Update RTC uptime counter */
    rtc_tick_update();

    /* === Sleep/Wakeup: Check all tasks for timeout ===
     * Walk the run queue and wake up any task whose sleep_until
     * has expired. This is a simple O(n) scan; for production
     * use a timer wheel or priority queue would be better.
     */
    {
        /* NOTE: This code runs in interrupt context (PIT/APIC timer IRQ),
         * so interrupts are already disabled on this CPU. The spin_lock
         * here is safe because the target CPU cannot hold the rq lock
         * with IRQs enabled while we are in our IRQ handler. If the
         * target CPU holds the lock, it will release it without needing
         * IRQs to be enabled on our side. */
        extern struct run_queue per_cpu_rq[];
        extern int num_cpus;
        for (int cpu = 0; cpu < num_cpus && cpu < MAX_CPUS; cpu++) {
            struct run_queue *rq = &per_cpu_rq[cpu];
            spin_lock((spinlock_t*)&rq->lock);  /* FIXED: protect run queue traversal */
            if (!rq->head || rq->count == 0) {
                spin_unlock((spinlock_t*)&rq->lock);
                continue;
            }
            struct task_struct *t = rq->head;
            int scanned = 0;
            do {
                if (t->state == TASK_BLOCKED && t->sleep_until > 0 &&
                    t->sleep_until <= tick) {
                    t->sleep_until = 0;
                    t->state = TASK_READY;
                    __sync_lock_test_and_set(&need_resched, 1);
                }
                t = t->next;
                scanned++;
            } while (t != rq->head && scanned < rq->count + 1);
            spin_unlock((spinlock_t*)&rq->lock);  /* FIXED: release run queue lock */
        }
    }

    /* === Preemptive Scheduling: Time Slice Accounting ===
     * Delegates to schedule_tick() in sched.c which decrements
     * the current task's time_slice and sets need_resched when
     * the time slice is exhausted.
     */
    schedule_tick();

    /* Set flag instead of calling schedule() directly.
     * The interrupted code will check need_resched at its next
     * safe point (iretq return path in the IRQ wrapper).
     * Use atomic store for SMP visibility. */
    __sync_lock_test_and_set(&need_resched, 1);

    /* Periodically run SMP load balancing */
    int count = __sync_fetch_and_add(&smp_balance_counter, 1);
    if (count + 1 >= SMP_BALANCE_INTERVAL) {
        __sync_lock_test_and_set(&smp_balance_counter, 0);
        smp_schedule(current_cpu_id());
    }
}

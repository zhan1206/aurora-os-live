/*
 * smp.h - Symmetric Multi-Processing support for AuroraOS
 *
 * Provides:
 *   - Per-CPU data structure (cpu_data) with current task, LAPIC ID, etc.
 *   - SMP initialization via ACPI MADT parsing
 *   - AP startup trampoline
 *   - Per-CPU TSS and GDT setup
 *   - IPI (Inter-Processor Interrupt) sending
 *   - TLB shootdown for cross-CPU page table invalidation
 *   - Atomic spinlock (upgraded from CLI/STI)
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "sched.h"

/* ================================================================
 * Constants
 * ================================================================ */
#ifndef MAX_CPUS
#define MAX_CPUS            8
#endif
#define TRAMPOLINE_ADDR     0x8000ULL     /* AP startup trampoline (physical) */
#define TRAMPOLINE_SIZE     4096           /* one page for trampoline code */

/* GDT entry count for per-CPU GDT (same layout as global GDT) */
#define PERCPU_GDT_ENTRIES  7              /* null, kcode, kdata, ucode, udata, tss_lo, tss_hi */

/* ================================================================
 * Per-CPU data structure
 * ================================================================ */

struct cpu_data {
    int cpu_id;                          /* logical CPU number (0..MAX_CPUS-1) */
    int lapic_id;                        /* local APIC ID from hardware */
    struct task_struct *current_task;    /* currently running task on this CPU */
    uint64_t tsc_freq;                   /* TSC frequency in Hz */
    int online;                          /* 1 = CPU is online, 0 = not yet */
    uint64_t gdt[PERCPU_GDT_ENTRIES];   /* per-CPU GDT (TSS descriptor differs) */
    uint8_t tss[104];                    /* per-CPU TSS (IST stack pointers) */
};

/* ================================================================
 * Per-CPU data arrays
 * ================================================================ */

extern struct cpu_data cpu_data[MAX_CPUS];
extern int num_cpus;

/* ================================================================
 * Get current CPU's data structure (fast access via LAPIC ID)
 * ================================================================ */

static inline struct cpu_data *this_cpu(void) {
    /* On each CPU, we store a pointer to that CPU's cpu_data entry
     * in the GS segment base. The entry code sets this up. */
    struct cpu_data *ptr;
    asm volatile ("mov %%gs:0, %0" : "=r"(ptr));
    return ptr;
}

/* ================================================================
 * SMP API
 * ================================================================ */

void smp_init(void *mb_info);
void ap_startup(void);
int get_cpu_count(void);
void smp_send_ipi(int cpu_id, int vector);

/*
 * smp_tlb_shootdown: Invalidate a TLB entry on all other CPUs.
 * Sends IPI_TLB_VECTOR to every online CPU except self.
 */
void smp_tlb_shootdown(uint64_t vaddr);

/* ================================================================
 * Atomic spinlock (replaces CLI/STI-based spinlock)
 * ================================================================ */

typedef struct spinlock {
    volatile uint32_t locked;
} spinlock_t;

/* Initialize a spinlock */
static inline void spin_init(spinlock_t *lock) {
    lock->locked = 0;
}

/* Acquire spinlock with atomic test-and-set + pause */
static inline void spin_lock(spinlock_t *lock) {
    while (1) {
        /* Atomically test and set: if locked was 0, set to 1 and break */
        uint32_t old = 0;
        uint32_t new = 1;
        asm volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(lock->locked)
            : "r"(new), "0"(old)
            : "memory"
        );
        if (old == 0) break;
        /* Spin with PAUSE to reduce bus contention */
        asm volatile ("pause" ::: "memory");
    }
}

/* Release spinlock */
static inline void spin_unlock(spinlock_t *lock) {
    asm volatile ("movl $0, %0" : "=m"(lock->locked) : : "memory");
}

/* ================================================================
 * Ticket spinlock (fair, scalable alternative)
 * ================================================================ */

typedef struct ticket_lock {
    volatile uint32_t next_ticket;
    volatile uint32_t now_serving;
} ticket_lock_t;

static inline void ticket_lock_init(ticket_lock_t *lock) {
    lock->next_ticket = 0;
    lock->now_serving = 0;
}

static inline void ticket_lock(ticket_lock_t *lock) {
    uint32_t my_ticket;
    asm volatile (
        "lock xaddl %0, %1"
        : "=r"(my_ticket), "+m"(lock->next_ticket)
        : "0"(1)
        : "memory"
    );
    while (lock->now_serving != my_ticket) {
        asm volatile ("pause" ::: "memory");
    }
}

static inline void ticket_unlock(ticket_lock_t *lock) {
    asm volatile ("lock incl %0" : "+m"(lock->now_serving) : : "memory");
}

#endif /* SMP_H */
/*
 * apic.c - Local APIC and I/O APIC initialization and management
 *
 * LAPIC is memory-mapped at 0xFEE00000 (physical). We map it into
 * kernel virtual address space via page tables.
 * All register access is 32-bit, aligned. ICR is special: write
 * high 32 bits first, then low 32 bits (which triggers the IPI).
 */
#include "apic.h"
#include "include/log.h"
#include "include/portio.h"
#include "mem.h"
#include "pagetable.h"
#include <stdint.h>

/* ================================================================
 * Globals — LAPIC virtual base
 * ================================================================ */
static volatile uint32_t *lapic_vbase = NULL;
static int lapic_timer_divisor = 0;
static uint32_t lapic_timer_calib_count = 0;
static int bsp_lapic_id = -1;

/* ================================================================
 * LAPIC MMIO helpers
 * ================================================================ */

static inline uint32_t lapic_read(uint32_t offset) {
    return *(volatile uint32_t *)((uintptr_t)lapic_vbase + offset);
}

static inline void lapic_write(uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)((uintptr_t)lapic_vbase + offset) = val;
}

/* ================================================================
 * lapic_init: Map LAPIC base, enable LAPIC, detect BSP
 * ================================================================ */

void lapic_init(void) {
    /* Map LAPIC MMIO region (one page = 4KB covers all LAPIC registers).
     * If already mapped, map_page handles the idempotent re-mapping safely. */
    uint64_t lapic_phys = LAPIC_BASE_PHYS;
    uint64_t lapic_virt = 0xFFFF8000FEE00000ULL; /* canonical kernel address */

    if (!lapic_vbase) {
        int ret = map_page(get_kernel_cr3(), lapic_virt, lapic_phys,
                           PTE_PRESENT | PTE_RW | PTE_NX);
        if (ret != 0) {
            log_printf(LOG_LEVEL_ERR, "apic: failed to map LAPIC at %p -> %p\n",
                       (void *)lapic_virt, (void *)lapic_phys);
            return;
        }
        lapic_vbase = (volatile uint32_t *)(uintptr_t)lapic_virt;
    }

    /* Read LAPIC ID */
    uint32_t id_reg = lapic_read(LAPIC_ID);
    if (bsp_lapic_id < 0) {
        bsp_lapic_id = (int)(id_reg >> 24);
    }
    int my_id = (int)(id_reg >> 24);
    log_printf(LOG_LEVEL_INFO, "apic: LAPIC mapped at %p, CPU ID=%d, version=0x%x\n",
               (void *)lapic_vbase, my_id,
               lapic_read(LAPIC_VERSION) & 0xFF);

    /* Enable LAPIC via Spurious Interrupt Vector Register.
     * Spurious vector = 0xFF (must be 0x0F upper nibble).
     * Software enable bit = 0x100.
     * Focus processor checking disabled (bit 9 = 0). */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr = (svr & ~0xFFU) | 0xFF;  /* spurious vector = 0xFF */
    lapic_write(LAPIC_SVR, svr);

    /* Set Task Priority Register to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    log_printf(LOG_LEVEL_INFO, "apic: LAPIC enabled (SVR=0x%x)\n",
               lapic_read(LAPIC_SVR));
}

/* ================================================================
 * lapic_eoi: Send End-Of-Interrupt
 * ================================================================ */

void lapic_eoi(void) {
    if (lapic_vbase) {
        lapic_write(LAPIC_EOI, 0);
    }
}

/* ================================================================
 * lapic_read_id: Return this CPU's LAPIC ID
 * ================================================================ */

uint32_t lapic_read_id(void) {
    if (!lapic_vbase) return 0;
    return lapic_read(LAPIC_ID) >> 24;
}

/* ================================================================
 * lapic_is_bsp: Check if this CPU is the BSP
 * ================================================================ */

int lapic_is_bsp(void) {
    return (int)lapic_read_id() == bsp_lapic_id;
}

/* ================================================================
 * lapic_timer_calibrate: Measure the LAPIC timer rate
 *
 * Uses the PIT to calibrate: set a known PIT count, start the
 * LAPIC timer at maximum, wait for PIT to expire, then read
 * how many LAPIC timer ticks elapsed.
 * ================================================================ */

static void lapic_timer_calibrate(void) {
    if (!lapic_vbase) return;

    /* Set timer divide to 16 (divider value = 3) */
    lapic_write(LAPIC_TIMER_DIV, 3);
    lapic_timer_divisor = 16;

    /* Set initial count to max (0xFFFFFFFF) */
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFU);

    /* Calibrate over 10ms using PIT channel 2.
     * PIT frequency = 1193180 Hz.
     * For 10ms: 1193180 / 100 = 11932 ticks. */
    uint32_t pit_count = 11932;
    uint8_t pit_cmd = 0xB0; /* channel 2, lobyte/hibyte, mode 0, binary */
    outb(0x43, pit_cmd);
    outb(0x42, (uint8_t)(pit_count & 0xFF));
    outb(0x42, (uint8_t)((pit_count >> 8) & 0xFF));

    /* Wait for PIT to count down (mode 0: output goes high when done).
     * Read back command: latch count for channel 2. */
    while (1) {
        outb(0x43, 0xE2); /* read-back: channel 2, latch count, don't latch status */
        uint8_t lo = inb(0x42);
        uint8_t hi = inb(0x42);
        uint16_t count = lo | ((uint16_t)hi << 8);
        if (count == 0 || count > pit_count) break;
    }

    /* Read current count and calculate elapsed */
    uint32_t elapsed = 0xFFFFFFFFU - lapic_read(LAPIC_TIMER_CURRENT);
    lapic_timer_calib_count = elapsed;

    log_printf(LOG_LEVEL_INFO, "apic: timer calibrated: %u ticks in %d ms "
               "(~%u ticks/ms)\n",
               elapsed, APIC_TIMER_CALIB_MS,
               elapsed / APIC_TIMER_CALIB_MS);
}

/* ================================================================
 * lapic_timer_init: Set up per-CPU timer interrupt
 *
 * @freq_hz: desired frequency in Hz (e.g., 100 for 100 Hz)
 * ================================================================ */

void lapic_timer_init(uint32_t freq_hz) {
    if (!lapic_vbase) return;

    if (lapic_timer_calib_count == 0) {
        lapic_timer_calibrate();
    }

    if (freq_hz == 0) freq_hz = 100;  /* default 100 Hz */

    /* Calculate initial count: ticks_per_ms * 1000 / freq_hz */
    uint32_t ticks_per_ms = lapic_timer_calib_count / APIC_TIMER_CALIB_MS;
    uint32_t init_count = ticks_per_ms * 1000 / freq_hz;

    if (init_count == 0) init_count = 1;

    /* Set timer divider (already set during calibration) */
    lapic_write(LAPIC_TIMER_DIV, 3);  /* divide by 16 */

    /* Set LVT Timer entry:
     * Vector = 32 (same as PIT IRQ0), periodic mode */
    uint32_t lvt = 32 | LAPIC_TIMER_PERIODIC;
    lapic_write(LAPIC_LVT_TIMER, lvt);

    /* Set initial count */
    lapic_write(LAPIC_TIMER_INIT, init_count);

    log_printf(LOG_LEVEL_INFO, "apic: LAPIC timer started at %u Hz "
               "(init_count=%u, divisor=%d)\n",
               freq_hz, init_count, lapic_timer_divisor);
}

/* ================================================================
 * lapic_send_ipi: Send an inter-processor interrupt
 *
 * @lapic_id: target CPU's LAPIC ID
 * @vector:   interrupt vector to deliver
 * ================================================================ */

void lapic_send_ipi(int lapic_id, int vector) {
    if (!lapic_vbase) return;

    /* Wait for previous IPI to complete */
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12)) {
        asm volatile ("pause" ::: "memory");
    }

    /*
     * NOTE: The ICR high/low write sequence is NOT atomic.  Between
     * setting ICR_HI and writing ICR_LO, the destination field could
     * theoretically be clobbered by another CPU writing to the same
     * local APIC.  This is acceptable on single-socket / single-APIC
     * systems where only one CPU's LAPIC is accessed at a time.
     * Multi-socket systems with x2APIC should use the 64-bit MSR write
     * (wrmsr 0x830) which is atomic.
     */
    /* Set destination in ICR high */
    lapic_write(LAPIC_ICR_HI, (uint32_t)lapic_id << 24);

    /* Write ICR low: fixed delivery, physical destination, edge,
     * assert, no shorthand, vector */
    uint32_t icr_lo = (uint32_t)(vector & 0xFF)
                    | ICR_DSH_DEST
                    | LAPIC_DM_FIXED;
    lapic_write(LAPIC_ICR_LO, icr_lo);
}

/* ================================================================
 * lapic_start_ap: Send INIT-SIPI-SIPI sequence to start an AP
 *
 * @lapic_id:   target AP's LAPIC ID
 * @entry_addr: physical address of trampoline code (must be 4KB-aligned)
 *              -> value is the page number (entry_addr >> 12)
 * ================================================================ */

void lapic_start_ap(int lapic_id, uint32_t entry_addr) {
    if (!lapic_vbase) return;

    uint32_t vector = entry_addr >> 12;  /* SIPI vector = physical address / 4096 */

    log_printf(LOG_LEVEL_INFO, "apic: starting AP id=%d, trampoline@0x%x "
               "(vector=0x%x)\n", lapic_id, entry_addr, vector);

    /* Step 1: Send INIT IPI */
    lapic_write(LAPIC_ICR_HI, (uint32_t)lapic_id << 24);
    lapic_write(LAPIC_ICR_LO, (uint32_t)(LAPIC_DM_INIT | ICR_DSH_DEST | 0x0C00));
    /* bit 15 = 1 (level assert), bit 12 = delivery status */

    /* Wait for delivery */
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12)) {
        asm volatile ("pause" ::: "memory");
    }

    /* 10ms delay — use PIT for approximate delay */
    {
        uint32_t pit_delay = 11932;  /* ~10ms */
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(pit_delay & 0xFF));
        outb(0x42, (uint8_t)((pit_delay >> 8) & 0xFF));
        while (1) {
            outb(0x43, 0xE2);
            uint8_t lo = inb(0x42);
            uint8_t hi = inb(0x42);
            uint16_t count = lo | ((uint16_t)hi << 8);
            if (count == 0 || count > pit_delay) break;
        }
    }

    /* Send deassert INIT (level deassert) */
    lapic_write(LAPIC_ICR_HI, (uint32_t)lapic_id << 24);
    lapic_write(LAPIC_ICR_LO, (uint32_t)(LAPIC_DM_INIT | ICR_DSH_DEST | 0x0800));
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12)) {
        asm volatile ("pause" ::: "memory");
    }

    /* 200us delay */
    {
        uint32_t pit_delay = 238;  /* ~200us */
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(pit_delay & 0xFF));
        outb(0x42, (uint8_t)((pit_delay >> 8) & 0xFF));
        while (1) {
            outb(0x43, 0xE2);
            uint8_t lo = inb(0x42);
            uint8_t hi = inb(0x42);
            uint16_t count = lo | ((uint16_t)hi << 8);
            if (count == 0 || count > pit_delay) break;
        }
    }

    /* Step 2: Send two STARTUP IPIs */
    for (int i = 0; i < 2; i++) {
        lapic_write(LAPIC_ICR_HI, (uint32_t)lapic_id << 24);
        lapic_write(LAPIC_ICR_LO, (uint32_t)(vector | LAPIC_DM_STARTUP | ICR_DSH_DEST));

        while (lapic_read(LAPIC_ICR_LO) & (1 << 12)) {
            asm volatile ("pause" ::: "memory");
        }

        /* 200us delay between SIPIs */
        uint32_t pit_delay = 238;
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)(pit_delay & 0xFF));
        outb(0x42, (uint8_t)((pit_delay >> 8) & 0xFF));
        while (1) {
            outb(0x43, 0xE2);
            uint8_t lo = inb(0x42);
            uint8_t hi = inb(0x42);
            uint16_t count = lo | ((uint16_t)hi << 8);
            if (count == 0 || count > pit_delay) break;
        }
    }

    log_printf(LOG_LEVEL_INFO, "apic: INIT-SIPI-SIPI sent to AP id=%d\n", lapic_id);
}

/* ================================================================
 * ioapic_init: Remap I/O APIC, replace legacy PIC
 *
 * @ioapic_base: physical address of I/O APIC (usually 0xFEC00000)
 * ================================================================ */

void ioapic_init(uint64_t ioapic_base) {
    /* Map I/O APIC MMIO region */
    uint64_t ioapic_virt = 0xFFFF8000FEC00000ULL;

    int ret = map_page(get_kernel_cr3(), ioapic_virt, ioapic_base,
                       PTE_PRESENT | PTE_RW | PTE_NX);
    if (ret != 0) {
        log_printf(LOG_LEVEL_ERR, "apic: failed to map IOAPIC at %p\n",
                   (void *)ioapic_base);
        return;
    }

    volatile uint32_t *ioapic_base_v = (volatile uint32_t *)(uintptr_t)ioapic_virt;

    /* Read IOAPIC version */
    *(volatile uint32_t *)((uintptr_t)ioapic_base_v + IOAPIC_REGSEL) = IOAPIC_VER;
    uint32_t ver = *(volatile uint32_t *)((uintptr_t)ioapic_base_v + IOAPIC_IOWIN);
    int max_redir = (int)((ver >> 16) & 0xFF);

    log_printf(LOG_LEVEL_INFO, "apic: IOAPIC at %p, version=0x%x, max_redir=%d\n",
               (void *)ioapic_base, ver, max_redir);

    /* Disable legacy PIC: mask all interrupts on both master and slave */
    outb(0x21, 0xFF);  /* mask all on master PIC */
    outb(0xA1, 0xFF);  /* mask all on slave PIC */

    /* Mask all I/O APIC redirection entries */
    for (int i = 0; i <= max_redir; i++) {
        uint32_t regsel = IOAPIC_REDTBL_BASE + 2 * i;
        *(volatile uint32_t *)((uintptr_t)ioapic_base_v + IOAPIC_REGSEL) = regsel + 1;
        *(volatile uint32_t *)((uintptr_t)ioapic_base_v + IOAPIC_IOWIN) = 0;
        /* mask bit = 0x10000 in high 32 bits */
        uint32_t high = *(volatile uint32_t *)((uintptr_t)ioapic_base_v + IOAPIC_IOWIN);
        *(volatile uint32_t *)((uintptr_t)ioapic_base_v + IOAPIC_IOWIN) = high | 0x10000;
    }

    /* Route IRQ0 (PIT) -> vector 0x20 (remapped via I/O APIC).
     * Normally IRQ0 is pin 2 on the I/O APIC, but we use the APIC timer
     * instead, so we just mask everything for now. */
    log_printf(LOG_LEVEL_INFO, "apic: IOAPIC initialized, legacy PIC disabled\n");
}
#include "include/log.h"
#include "include/portio.h"
#include "apic.h"
#include "sched.h"
#include "smp.h"
#include <stdint.h>

extern void load_idt(void);

/* IDT table: 256 entries × 16 bytes, defined in idt.S */
extern uint64_t idt[];

void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);
    outb(0xA1, 0x0);
}

/*
 * Disable legacy PIC by masking all interrupts.
 * Called when IOAPIC is available to take over interrupt routing.
 */
void pic_disable(void) {
    outb(0x21, 0xFF);  /* mask all on master PIC */
    outb(0xA1, 0xFF);  /* mask all on slave PIC */
    log_printf(LOG_LEVEL_INFO, "IRQ: legacy PIC disabled\n");
}

/*
 * Fill a 16-byte IDT gate at the given vector index.
 * type_attr: 0x8E = interrupt gate (P=1, DPL=0), 0xEE = DPL=3
 */
static void idt_set_gate(int vector, uint64_t offset, uint16_t selector,
                         uint8_t ist, uint8_t type_attr) {
    uint64_t *entry = &idt[vector * 2]; /* each entry is 16 bytes = 2 × uint64_t */

    entry[0] = (offset & 0xFFFF)
             | ((uint64_t)selector << 16)
             | (((uint64_t)ist & 0x7) << 32)
             | ((uint64_t)type_attr << 40)
             | (((offset >> 16) & 0xFFFF) << 48);

    entry[1] = (offset >> 32);
}

/* Exception handler symbols (from exception_handlers.S) */
extern void exc_handler_0(void);
extern void exc_handler_1(void);
extern void exc_handler_2(void);
extern void exc_handler_3(void);
extern void exc_handler_4(void);
extern void exc_handler_5(void);
extern void exc_handler_6(void);
extern void exc_handler_7(void);
extern void exc_handler_8(void);
extern void exc_handler_9(void);
extern void exc_handler_10(void);
extern void exc_handler_11(void);
extern void exc_handler_12(void);
extern void exc_handler_13(void);
extern void exc_handler_16(void);
extern void exc_handler_17(void);
extern void exc_handler_18(void);
extern void exc_handler_19(void);
extern void exc_handler_20(void);
extern void exc_handler_21(void);
extern void exc_handler_22(void);
extern void exc_handler_23(void);
extern void exc_handler_24(void);
extern void exc_handler_25(void);
extern void exc_handler_26(void);
extern void exc_handler_27(void);
extern void exc_handler_28(void);
extern void exc_handler_29(void);
extern void exc_handler_30(void);
extern void exc_handler_31(void);
extern void pf_handler(void);
extern void irq0_handler(void);
extern void irq1_handler(void);

/* ================================================================
 * IPI handlers (SMP Inter-Processor Interrupts)
 *
 * Vector 0xFE (IPI_RESCHED_VECTOR): Reschedule IPI
 *   - Sent by smp_send_ipi() to trigger a reschedule on another CPU.
 *   - Sets need_resched and sends EOI to the local APIC.
 *
 * Vector 0xFD (IPI_TLB_VECTOR): TLB shootdown IPI
 *   - Sent by smp_tlb_shootdown() to invalidate a TLB entry on
 *     other CPUs. The target address is passed via a shared variable.
 *   - Flushes the TLB entry and sends EOI.
 * ================================================================ */

extern volatile int need_resched;

/*
 * ipi_resched_handler: Handler for reschedule IPI (vector 0xFE).
 * Uses GCC's __attribute__((interrupt)) which automatically saves/restores
 * all registers and uses iretq to return.
 */
__attribute__((interrupt))
static void ipi_resched_handler(void *frame) {
    (void)frame;
    need_resched = 1;
    lapic_eoi();
}

/*
 * ipi_tlb_handler: Handler for TLB shootdown IPI (vector 0xFD).
 * Invalidates the TLB entry for the address stored in the global
 * shootdown_vaddr variable.
 */
static volatile uint64_t shootdown_vaddr = 0;

__attribute__((interrupt))
static void ipi_tlb_handler(void *frame) {
    (void)frame;
    if (shootdown_vaddr) {
        asm volatile ("invlpg (%0)" :: "r"(shootdown_vaddr) : "memory");
    }
    lapic_eoi();
}

extern void keyboard_init(void);

void irq_init(void) {
    /* Fill IDT gates at runtime (avoids assembler relocation issues) */
    idt_set_gate(0,  (uint64_t)(uintptr_t)exc_handler_0,  0x08, 0, 0x8E);
    idt_set_gate(1,  (uint64_t)(uintptr_t)exc_handler_1,  0x08, 0, 0x8E);
    idt_set_gate(2,  (uint64_t)(uintptr_t)exc_handler_2,  0x08, 0, 0x8E);
    idt_set_gate(3,  (uint64_t)(uintptr_t)exc_handler_3,  0x08, 3, 0xEE); /* #BP DPL=3 */
    idt_set_gate(4,  (uint64_t)(uintptr_t)exc_handler_4,  0x08, 0, 0x8E);
    idt_set_gate(5,  (uint64_t)(uintptr_t)exc_handler_5,  0x08, 0, 0x8E);
    idt_set_gate(6,  (uint64_t)(uintptr_t)exc_handler_6,  0x08, 0, 0x8E);
    idt_set_gate(7,  (uint64_t)(uintptr_t)exc_handler_7,  0x08, 0, 0x8E);
    idt_set_gate(8,  (uint64_t)(uintptr_t)exc_handler_8,  0x08, 1, 0x8E); /* #DF IST=1 */
    idt_set_gate(9,  (uint64_t)(uintptr_t)exc_handler_9,  0x08, 0, 0x8E);
    idt_set_gate(10, (uint64_t)(uintptr_t)exc_handler_10, 0x08, 0, 0x8E);
    idt_set_gate(11, (uint64_t)(uintptr_t)exc_handler_11, 0x08, 0, 0x8E);
    idt_set_gate(12, (uint64_t)(uintptr_t)exc_handler_12, 0x08, 0, 0x8E);
    idt_set_gate(13, (uint64_t)(uintptr_t)exc_handler_13, 0x08, 0, 0x8E);
    /* Vector 14: Page Fault */
    idt_set_gate(14, (uint64_t)(uintptr_t)pf_handler,      0x08, 0, 0x8E);
    /* Vector 15: Reserved (leave zeroed) */
    idt_set_gate(16, (uint64_t)(uintptr_t)exc_handler_16,  0x08, 0, 0x8E);
    idt_set_gate(17, (uint64_t)(uintptr_t)exc_handler_17,  0x08, 0, 0x8E);
    idt_set_gate(18, (uint64_t)(uintptr_t)exc_handler_18,  0x08, 0, 0x8E);
    idt_set_gate(19, (uint64_t)(uintptr_t)exc_handler_19,  0x08, 0, 0x8E);
    idt_set_gate(20, (uint64_t)(uintptr_t)exc_handler_20,  0x08, 0, 0x8E);
    idt_set_gate(21, (uint64_t)(uintptr_t)exc_handler_21,  0x08, 0, 0x8E);
    idt_set_gate(22, (uint64_t)(uintptr_t)exc_handler_22,  0x08, 0, 0x8E);
    idt_set_gate(23, (uint64_t)(uintptr_t)exc_handler_23,  0x08, 0, 0x8E);
    idt_set_gate(24, (uint64_t)(uintptr_t)exc_handler_24,  0x08, 0, 0x8E);
    idt_set_gate(25, (uint64_t)(uintptr_t)exc_handler_25,  0x08, 0, 0x8E);
    idt_set_gate(26, (uint64_t)(uintptr_t)exc_handler_26,  0x08, 0, 0x8E);
    idt_set_gate(27, (uint64_t)(uintptr_t)exc_handler_27,  0x08, 0, 0x8E);
    idt_set_gate(28, (uint64_t)(uintptr_t)exc_handler_28,  0x08, 0, 0x8E);
    idt_set_gate(29, (uint64_t)(uintptr_t)exc_handler_29,  0x08, 0, 0x8E);
    idt_set_gate(30, (uint64_t)(uintptr_t)exc_handler_30,  0x08, 0, 0x8E);
    idt_set_gate(31, (uint64_t)(uintptr_t)exc_handler_31,  0x08, 0, 0x8E);
    /* IRQ0 (PIT) at vector 32 */
    idt_set_gate(32, (uint64_t)(uintptr_t)irq0_handler,    0x08, 0, 0x8E);
    /* IRQ1 (Keyboard) at vector 33 */
    idt_set_gate(33, (uint64_t)(uintptr_t)irq1_handler,    0x08, 0, 0x8E);
    /* IPI: TLB shootdown (vector 0xFD) */
    idt_set_gate(IPI_TLB_VECTOR, (uint64_t)(uintptr_t)&ipi_tlb_handler, 0x08, 0, 0x8E);
    /* IPI: Reschedule (vector 0xFE) */
    idt_set_gate(IPI_RESCHED_VECTOR, (uint64_t)(uintptr_t)&ipi_resched_handler, 0x08, 0, 0x8E);

    load_idt();
    pic_remap();
    keyboard_init();
    log_printf(LOG_LEVEL_INFO, "IRQ/IDT initialized\n");

    /* Initialize LAPIC and APIC timer as per-CPU timer source.
     * This replaces the PIT for scheduling ticks on SMP systems.
     * On single-core (no APIC), the PIT continues to work. */
    lapic_init();
    lapic_timer_init(100);  /* 100 Hz timer */

    /* Enable interrupts — IDT and PIC are now configured, so it's safe.
     * Before this point, interrupts were disabled (from cli in entry.S)
     * to prevent crashes on the garbage IDT. */
    asm volatile ("sti" ::: "memory");
    log_printf(LOG_LEVEL_DEBUG, "IRQ: interrupts enabled (sti)\n");
}

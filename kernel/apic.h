/*
 * apic.h - Local APIC and I/O APIC register definitions
 *
 * LAPIC base: 0xFEE00000 (physical) — mapped via page tables.
 * All register offsets are 32-bit aligned (must be accessed as 32-bit).
 * Except ICR: two 32-bit writes (low first, then high).
 */
#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* ================================================================
 * LAPIC base address
 * ================================================================ */
#define LAPIC_BASE_PHYS     0xFEE00000ULL

/* ================================================================
 * LAPIC register offsets (from base, in bytes)
 * ================================================================ */
#define LAPIC_ID            0x0020   /* Local APIC ID */
#define LAPIC_VERSION       0x0030   /* Local APIC Version */
#define LAPIC_TPR           0x0080   /* Task Priority Register */
#define LAPIC_APR           0x0090   /* Arbitration Priority Register */
#define LAPIC_PPR           0x00A0   /* Processor Priority Register */
#define LAPIC_EOI           0x00B0   /* End-Of-Interrupt */
#define LAPIC_RRD           0x00C0   /* Remote Read */
#define LAPIC_LDR           0x00D0   /* Logical Destination */
#define LAPIC_DFR           0x00E0   /* Destination Format */
#define LAPIC_SVR           0x00F0   /* Spurious Interrupt Vector */
#define LAPIC_ISR_BASE      0x0100   /* In-Service Register (256 bits = 8 × 32) */
#define LAPIC_TMR_BASE      0x0180   /* Trigger Mode Register (256 bits) */
#define LAPIC_IRR_BASE      0x0200   /* Interrupt Request Register (256 bits) */
#define LAPIC_ESR           0x0280   /* Error Status Register */
#define LAPIC_ICR_LO        0x0300   /* Interrupt Command Register (low 32) */
#define LAPIC_ICR_HI        0x0310   /* Interrupt Command Register (high 32) */
#define LAPIC_LVT_TIMER     0x0320   /* LVT Timer */
#define LAPIC_LVT_THERMAL   0x0330   /* LVT Thermal Sensor */
#define LAPIC_LVT_PERF      0x0340   /* LVT Performance Counter */
#define LAPIC_LVT_LINT0     0x0350   /* LVT LINT0 */
#define LAPIC_LVT_LINT1     0x0360   /* LVT LINT1 */
#define LAPIC_LVT_ERROR     0x0370   /* LVT Error */
#define LAPIC_TIMER_INIT    0x0380   /* Timer Initial Count */
#define LAPIC_TIMER_CURRENT 0x0390   /* Timer Current Count */
#define LAPIC_TIMER_DIV     0x03E0   /* Timer Divide Configuration */

/* ================================================================
 * LAPIC delivery modes (for ICR and LVT)
 * ================================================================ */
#define LAPIC_DM_FIXED      0x000   /* Fixed */
#define LAPIC_DM_SMI        0x200   /* SMI */
#define LAPIC_DM_NMI        0x400   /* NMI */
#define LAPIC_DM_INIT       0x500   /* INIT */
#define LAPIC_DM_STARTUP    0x600   /* Start-Up (SIPI) */

/* ================================================================
 * LAPIC destination shorthand (for ICR)
 * ================================================================ */
#define ICR_DSH_DEST        0x00000000   /* use destination field */
#define ICR_DSH_SELF        0x00040000   /* self */
#define ICR_DSH_ALL         0x00080000   /* all including self */
#define ICR_DSH_OTHER       0x000C0000   /* all excluding self */

/* ================================================================
 * LAPIC timer modes
 * ================================================================ */
#define LAPIC_TIMER_ONESHOT 0x00000
#define LAPIC_TIMER_PERIODIC 0x20000

/* LAPIC timer mask bit */
#define LAPIC_LVT_MASKED    0x10000

/* ================================================================
 * LAPIC software enable bit (in SVR)
 * ================================================================ */
#define LAPIC_SVR_ENABLE    0x100

/* ================================================================
 * I/O APIC register offsets
 * ================================================================ */
#define IOAPIC_REGSEL       0x00   /* I/O Register Select */
#define IOAPIC_IOWIN        0x10   /* I/O Window (data) */
#define IOAPIC_ID           0x00   /* IOAPIC ID */
#define IOAPIC_VER          0x01   /* IOAPIC Version */
#define IOAPIC_ARB          0x02   /* Arbitration ID */
#define IOAPIC_REDTBL_BASE  0x10   /* Redirection Table Base */

/* I/O APIC base address (typical, may be overridden by ACPI) */
#define IOAPIC_BASE_PHYS    0xFEC00000ULL

/* ================================================================
 * IPI vector assignments
 * ================================================================ */
#define IPI_RESCHED_VECTOR  0xFE   /* Reschedule IPI */
#define IPI_TLB_VECTOR      0xFD   /* TLB shootdown IPI */

/* ================================================================
 * APIC timer calibration constants
 * ================================================================ */
#define APIC_TIMER_CALIB_MS 10     /* calibrate over 10ms */

/* ================================================================
 * APIC API
 * ================================================================ */

void lapic_init(void);
void lapic_eoi(void);
void lapic_timer_init(uint32_t freq_hz);
void lapic_send_ipi(int lapic_id, int vector);
void lapic_start_ap(int lapic_id, uint32_t entry_addr);
uint32_t lapic_read_id(void);
int lapic_is_bsp(void);

void ioapic_init(uint64_t ioapic_base);

#endif /* APIC_H */
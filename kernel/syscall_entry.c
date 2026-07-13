/*
 * syscall_entry.c - SYSCALL/SYSRET MSR configuration (FIXED)
 *
 * Fixes:
 *   - Initialize MSR_GS_BASE and MSR_KERNEL_GS_BASE so that swapgs
 *     works correctly. Without this, swapgs in syscall_entry.S and
 *     IRQ handlers would exchange undefined GS bases.
 *     (Report §6.1, issue #1: "swapgs without GS base setup")
 *
 *   - MSR_GS_BASE:      user-space GS base (set to 0 for now).
 *   - MSR_KERNEL_GS_BASE: kernel GS base (set to per-CPU data area,
 *     or 0 for single-core with no per-CPU data).
 *
 *   - Added proper FMASK value: mask IF (bit 9) to disable interrupts
 *     during syscall entry, plus DF (bit 10) for direction flag clear.
 */

#include "include/log.h"
#include <stdint.h>

extern void syscall_entry(void);

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low  = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init(void) {
    /* MSR addresses */
    const uint32_t IA32_EFER          = 0xC0000080;
    const uint32_t IA32_STAR          = 0xC0000081;
    const uint32_t IA32_LSTAR         = 0xC0000082;
    const uint32_t IA32_FMASK         = 0xC0000084;
    const uint32_t IA32_GS_BASE       = 0xC0000101;
    const uint32_t IA32_KERNEL_GS_BASE = 0xC0000102;

    /* 1. Enable SYSCALL/SYSRET (EFER.SCE = bit 0) */
    uint64_t efer = rdmsr(IA32_EFER);
    efer |= 1;  /* SCE */
    wrmsr(IA32_EFER, efer);

    /* 2. Set LSTAR to syscall entry point */
    uint64_t addr = (uint64_t)(uintptr_t)syscall_entry;
    wrmsr(IA32_LSTAR, addr);

    /* 3. STAR: [63:48] = SYSRET CS (user CS + 16 for compatibility mode)
     *          [47:32] = SYSCALL CS (kernel CS)
     *   Our GDT: kernel CS = 0x08, user CS = 0x1B (0x18 | RPL 3)
     *   SYSRET loads: CS = (STAR[63:48] + 16) | 3 for 64-bit return
     *   So STAR[63:48] = 0x13 (user data selector) → CS = 0x23
     *   Actually: for SYSRET to 64-bit mode, CS = (STAR[63:48]+16) & ~3 | 3
     *   So set STAR[63:48] = 0x13 → SYSRET CS = (0x13+16) | 3 = 0x23
     *   STAR[47:32] = 0x08 (kernel CS)
     */
    uint64_t star = ((uint64_t)0x13 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(IA32_STAR, star);

    /* 4. FMASK: flags to clear on syscall entry.
     *    Bit 9  = IF (disable interrupts during syscall)
     *    Bit 10 = DF (clear direction flag)
     *    Bit 8  = TF (disable single-step during syscall)
     *    Bit 18 = AC (alignment check)
     */
    wrmsr(IA32_FMASK, (1 << 9) | (1 << 10) | (1 << 8) | (1 << 18));

    /* 5. Initialize GS bases for swapgs.
     *    MSR_KERNEL_GS_BASE: base address for kernel GS segment.
     *    Set to 0 for now (no per-CPU data yet).
     *    MSR_GS_BASE: user-space GS base (set to 0).
     *
     *    swapgs exchanges these two MSRs, so they must both be initialized.
     *    Without this, swapgs exchanges undefined values → GS points to
     *    invalid memory, causing crashes on any GS-relative access.
     */
    wrmsr(IA32_KERNEL_GS_BASE, 0);
    wrmsr(IA32_GS_BASE, 0);

    log_printf(LOG_LEVEL_INFO, "syscall: MSRs configured (GS bases set)\n");
}

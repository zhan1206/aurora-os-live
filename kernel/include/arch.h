/*
 * arch.h - Architecture abstraction layer
 *
 * Provides architecture-independent macros and inline functions.
 * x86_64 is the primary build target; multi-architecture code for
 * riscv64, aarch64, and loongarch64 is prepared.
 *
 * The ARCH_* macros are set by the build system (-D flag).  When no
 * architecture is specified, x86_64 is assumed as the default.
 */
#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <stdint.h>

/* ================================================================
 * Architecture selection
 * ================================================================ */
#if !defined(ARCH_X86_64) && !defined(ARCH_RISCV64) && \
    !defined(ARCH_AARCH64) && !defined(ARCH_LOONGARCH64)
#define ARCH_X86_64
#endif

/* ================================================================
 * Memory barrier
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_mfence(void) {
    asm volatile ("mfence" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_mfence(void) {
    asm volatile ("fence iorw, iorw" ::: "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_mfence(void) {
    asm volatile ("dmb ish" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_mfence(void) {
    asm volatile ("dbar 0" ::: "memory");
}
#endif

/* ================================================================
 * Halt instruction
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_halt(void) {
    asm volatile ("hlt" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_halt(void) {
    asm volatile ("wfi" ::: "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_halt(void) {
    asm volatile ("wfi" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_halt(void) {
    asm volatile ("idle 0" ::: "memory");
}
#endif

/* ================================================================
 * Interrupt control
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_disable_irq(void) {
    asm volatile ("cli" ::: "memory");
}
static inline void arch_enable_irq(void) {
    asm volatile ("sti" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_disable_irq(void) {
    asm volatile ("csrc sstatus, %0" : : "i"(1 << 1) : "memory");
}
static inline void arch_enable_irq(void) {
    asm volatile ("csrs sstatus, %0" : : "i"(1 << 1) : "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_disable_irq(void) {
    asm volatile ("msr daifset, #2" ::: "memory");
}
static inline void arch_enable_irq(void) {
    asm volatile ("msr daifclr, #2" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_disable_irq(void) {
    uint64_t val;
    asm volatile ("csrrd %0, 0x0" : "=r"(val));
    val &= ~(1ULL << 2);
    asm volatile ("csrwr %0, 0x0" : : "r"(val) : "memory");
}
static inline void arch_enable_irq(void) {
    uint64_t val;
    asm volatile ("csrrd %0, 0x0" : "=r"(val));
    val |= (1ULL << 2);
    asm volatile ("csrwr %0, 0x0" : : "r"(val) : "memory");
}
#endif

/* ================================================================
 * Get stack pointer
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("mov %%rsp, %0" : "=r"(sp));
    return sp;
}
#elif defined(ARCH_RISCV64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("mv %0, sp" : "=r"(sp));
    return sp;
}
#elif defined(ARCH_AARCH64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("mov %0, sp" : "=r"(sp));
    return sp;
}
#elif defined(ARCH_LOONGARCH64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("or %0, $sp, $zero" : "=r"(sp));
    return sp;
}
#endif

/* ================================================================
 * Cache flush
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_cache_flush(void) {
    asm volatile ("wbinvd" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_cache_flush(void) {
    asm volatile ("fence.i" ::: "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_cache_flush(void) {
    asm volatile ("ic iallu; dsb ish; isb" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_cache_flush(void) {
    asm volatile ("dbar 0; ibar 0" ::: "memory");
}
#endif

#endif /* KERNEL_ARCH_H */
/*
 * arch/loongarch64/csr.h - LoongArch Control and Status Register (CSR) definitions
 *
 * LoongArch CSR registers are accessed via csrrd/csrwr/csrxchg instructions.
 * CSR numbers are 14-bit (0x0000 - 0x3FFF).
 *
 * Reference: LoongArch Reference Manual Volume 1, Chapter 7
 */
#ifndef ARCH_LOONGARCH64_CSR_H
#define ARCH_LOONGARCH64_CSR_H

#include <stdint.h>

/* ================================================================
 * Basic CSR register numbers
 * ================================================================ */

/* CRMD (0x0): Current Mode Register */
#define CSR_CRMD                0x0
#define CRMD_PLV_MASK           0x3         /* Current privilege level */
#define CRMD_PLV_0              0           /* Kernel mode */
#define CRMD_PLV_3              3           /* User mode */
#define CRMD_IE                 (1 << 2)    /* Interrupt enable */
#define CRMD_DA                 (1 << 1)    /* Direct address translation */
#define CRMD_PG                 (1 << 0)    /* Paging enable */

/* PRMD (0x1): Previous Mode Register (saved on exception entry) */
#define CSR_PRMD                0x1
#define PRMD_PPLV_MASK          0x3
#define PRMD_PIE                (1 << 2)

/* EUEN (0x2): Extended Unit Enable Register */
#define CSR_EUEN                0x2
#define EUEN_FPE                (1 << 0)    /* FPU enable */
#define EUEN_SIMD               (1 << 1)    /* SIMD enable */

/* ECFG (0x4): Exception Configuration Register */
#define CSR_ECFG                0x4
#define ECFG_LIE_MASK           (0xFFF << 0)  /* Local interrupt enable bits */

/* ESTAT (0x5): Exception Status Register */
#define CSR_ESTAT               0x5
#define ESTAT_IS_MASK           (0xFFF << 0)  /* Interrupt status bits */
#define ESTAT_IS_IPI            (1 << 11)     /* IPI interrupt status */
#define ESTAT_IS_TIMER          (1 << 12)     /* Timer interrupt status */
#define ESTAT_ECODE_MASK        (0x3F << 16)  /* Exception code */
#define ESTAT_ECODE_SHIFT       16
#define ESTAT_ESUBCODE_MASK     (0xFF << 22)  /* Exception subcode */

/* Exception codes */
#define ECODE_INT               0       /* Interrupt */
#define ECODE_PIL               1       /* Page fault (load) */
#define ECODE_PIS               2       /* Page fault (store) */
#define ECODE_PIF               3       /* Page fault (instruction fetch) */
#define ECODE_ADE               8       /* Address error */
#define ECODE_ALE               9       /* Alignment error */
#define ECODE_SYS               11      /* System call */
#define ECODE_BRK               12      /* Breakpoint */
#define ECODE_INE               13      /* Instruction not exist */
#define ECODE_IPE               14      /* Privileged instruction */
#define ECODE_FPE               15      /* Floating-point exception */
#define ECODE_FPD               23      /* Floating-point disabled */

/* ERA (0x6): Exception Return Address */
#define CSR_ERA                 0x6

/* BADV (0x7): Bad Virtual Address (for page faults / address errors) */
#define CSR_BADV                0x7

/* TCFG (0x41): Timer Configuration Register */
#define CSR_TCFG                0x41
#define TCFG_EN                 (1ULL << 0)    /* Timer enable */
#define TCFG_PERIODIC           (1ULL << 1)    /* Periodic mode */
#define TCFG_INITVAL_MASK       0xFFFFFFFFFFFF0000ULL  /* Initial value */

/* TVAL (0x42): Timer Value Register (remaining ticks) */
#define CSR_TVAL                0x42

/* TICLR (0x44): Timer Interrupt Clear Register */
#define CSR_TICLR               0x44
#define TICLR_CLR               (1 << 0)       /* Clear timer interrupt */

/* ================================================================
 * TLB-related CSRs
 * ================================================================ */

/* TLBIDX (0x10): TLB Index Register */
#define CSR_TLBIDX              0x10
#define TLBIDX_INDEX_MASK       0x1FFFFF       /* TLB entry index */
#define TLBIDX_PS_MASK          (0x3F << 24)   /* Page size */
#define TLBIDX_NE               (1ULL << 31)   /* No entry (invalid) */

/* TLBEHI (0x11): TLB Entry High Register */
#define CSR_TLBEHI              0x11
#define TLBEHI_VPPN_MASK        0xFFFFFFFFFF000000ULL  /* Virtual page number */

/* TLBELO0 (0x12): TLB Entry Low Register 0 (even page) */
#define CSR_TLBELO0             0x12
#define TLBELO_V                (1 << 0)       /* Valid */
#define TLBELO_D                (1 << 1)       /* Dirty */
#define TLBELO_PLV_MASK         (0x3 << 2)     /* Privilege level */
#define TLBELO_MAT_MASK         (0x3 << 4)     /* Memory access type */
#define TLBELO_G                (1 << 6)       /* Global */
#define TLBELO_PPN_MASK         0xFFFFFFFFFFF00000ULL  /* Physical page number */

/* TLBELO1 (0x13): TLB Entry Low Register 1 (odd page) */
#define CSR_TLBELO1             0x13

/* Memory access types (MAT) */
#define TLB_MAT_UNCACHED        0       /* Strongly-ordered uncached */
#define TLB_MAT_CACHED          1       /* Coherent cached */
#define TLB_MAT_WEAK_UNCACHED   2       /* Weakly-ordered uncached */
#define TLB_MAT_WRITETHROUGH    3       /* Write-through */

/* TLB operation instructions */
#define TLBSRCH                 0       /* TLB search */
#define TLBRD                   1       /* TLB read */
#define TLBWR                   2       /* TLB write (random) */
#define TLBWR_S                 3       /* TLB write (indexed) */
#define TLBFILL                 4       /* TLB refill */
#define TLBINV                  5       /* TLB invalidate */
#define TLBINV_ASID             6       /* TLB invalidate by ASID */
#define TLBINV_ALL              7       /* TLB invalidate all */

/* ================================================================
 * CSR helper macros / inline functions
 * ================================================================ */

/* CSR read */
static inline uint64_t csr_read(uint32_t csr_num) {
    uint64_t val;
    asm volatile ("csrrd %0, %1" : "=r"(val) : "i"(csr_num));
    return val;
}

/* CSR write */
static inline void csr_write(uint32_t csr_num, uint64_t val) {
    asm volatile ("csrwr %0, %1" : : "r"(val), "i"(csr_num) : "memory");
}

/* CSR exchange (read-modify-write) */
static inline uint64_t csr_xchg(uint32_t csr_num, uint64_t new_val) {
    uint64_t old_val;
    asm volatile ("csrxchg %0, %1, %2"
        : "+r"(new_val), "=r"(old_val)
        : "i"(csr_num)
        : "memory");
    return old_val;
}

/* TLB invalidation helpers */
static inline void tlb_inv_all(void) {
    asm volatile ("invtlb 0x7, $zero, $zero" ::: "memory");
}

/* STLB refill helper */
static inline void stlb_fill(void) {
    asm volatile ("tlbwr" ::: "memory");
}

/* I/O barrier (dbar) */
static inline void dbar(uint32_t hint) {
    asm volatile ("dbar %0" : : "r"(hint) : "memory");
}

/* Instruction barrier (ibar) */
static inline void ibar(uint32_t hint) {
    asm volatile ("ibar %0" : : "r"(hint) : "memory");
}

#endif /* ARCH_LOONGARCH64_CSR_H */
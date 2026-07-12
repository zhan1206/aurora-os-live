/*
 * arch/aarch64/pagetable.h - ARM AArch64 page table definitions
 *
 * ARMv8-A uses a multi-level translation regime with 4 KiB, 16 KiB, or 64 KiB
 * page granules.  This header assumes 4 KiB granule (standard for ARM Linux).
 *
 * Translation table hierarchy (4 KiB granule, 48-bit VA):
 *   Level 0 (512 entries) → Level 1 (512 entries) → Level 2 (512 entries) → Level 3 (512 entries)
 *   VA[47:39]             → VA[38:30]             → VA[29:21]             → VA[20:12] + offset[11:0]
 *
 * Key system registers:
 *   TTBR0_EL1 - Lower half (user space) translation table base
 *   TTBR1_EL1 - Upper half (kernel space) translation table base
 *   TCR_EL1   - Translation Control Register
 *   MAIR_EL1  - Memory Attribute Indirection Register
 */
#ifndef ARCH_AARCH64_PAGETABLE_H
#define ARCH_AARCH64_PAGETABLE_H

#include <stdint.h>

/* Page size */
#define PAGE_SIZE       4096ULL
#define PAGE_SHIFT      12
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/* Translation table descriptor types */
#define DESC_INVALID            0
#define DESC_BLOCK              (1 << 0)   /* Level 0/1/2 block entry (valid) */
#define DESC_TABLE              (1 << 1)   /* Table descriptor (valid) */
#define DESC_PAGE               (1 << 1)   /* Level 3 page descriptor (valid) */

/* Descriptor attribute bits (lower attributes) */
#define DESC_ATTR_NS            (1ULL << 5)   /* Non-secure */
#define DESC_ATTR_AP_USER       (1ULL << 6)   /* AP[1]: EL0 accessible */
#define DESC_ATTR_AP_RW         (1ULL << 7)   /* AP[2]: Read-Write */
#define DESC_ATTR_SH_NONCACHE   (0ULL << 8)   /* Non-cacheable */
#define DESC_ATTR_SH_OUTER      (2ULL << 8)   /* Outer shareable */
#define DESC_ATTR_SH_INNER      (3ULL << 8)   /* Inner shareable */
#define DESC_ATTR_AF            (1ULL << 10)  /* Access flag */
#define DESC_ATTR_nG            (1ULL << 11)  /* Not global */
#define DESC_ATTR_PXN           (1ULL << 53)  /* Privileged execute-never */
#define DESC_ATTR_UXN           (1ULL << 54)  /* Unprivileged execute-never */
#define DESC_ATTR_CONTIGUOUS    (1ULL << 52)  /* Contiguous hint */

/* Memory attribute indices (MAIR_EL1) */
#define MAIR_DEVICE_nGnRnE      0x00
#define MAIR_DEVICE_nGnRE       0x04
#define MAIR_DEVICE_GRE         0x0C
#define MAIR_NORMAL_NC          0x44
#define MAIR_NORMAL_WT          0xAA
#define MAIR_NORMAL_WB          0xFF

/* Default MAIR_EL1 value */
#define MAIR_DEFAULT            0xFF440C0400ULL

/* ================================================================
 * TTBR0_EL1 / TTBR1_EL1 - Translation Table Base Registers
 * ================================================================ */
#define TTBR_ASID_SHIFT         48
#define TTBR_ASID_MASK          0xFFFF000000000000ULL
#define TTBR_BADDR_MASK         0x0000FFFFFFFFFFFFULL

/* ================================================================
 * TCR_EL1 - Translation Control Register
 * ================================================================ */
#define TCR_T0SZ_SHIFT          0
#define TCR_T1SZ_SHIFT          16
#define TCR_TG0_4K              (0ULL << 14)   /* TTBR0 granule: 4 KiB */
/* FIXED: TG0/TG1 encoding per ARM ARM:
 * TG0[15:14]: 00=4KB, 01=64KB, 10=16KB
 * TG1[31:30]: 10=4KB, 01=16KB, 11=64KB (when TG0=4KB) */
#define TCR_TG0_16K             (2ULL << 14)   /* TTBR0 granule: 16 KiB */
#define TCR_TG0_64K             (1ULL << 14)   /* TTBR0 granule: 64 KiB */
#define TCR_TG1_4K              (2ULL << 30)   /* TTBR1 granule: 4 KiB */
#define TCR_TG1_16K             (1ULL << 30)   /* TTBR1 granule: 16 KiB (when TG0=4KB) */
#define TCR_TG1_64K             (3ULL << 30)   /* TTBR1 granule: 64 KiB */
#define TCR_IRGN0_NC            (0ULL << 8)
#define TCR_IRGN0_WB            (1ULL << 8)
#define TCR_IRGN0_WT            (2ULL << 8)
#define TCR_ORGN0_NC            (0ULL << 10)
#define TCR_ORGN0_WB            (1ULL << 10)
#define TCR_ORGN0_WT            (2ULL << 10)
#define TCR_IRGN1_NC            (0ULL << 24)
#define TCR_IRGN1_WB            (1ULL << 24)
#define TCR_IRGN1_WT            (2ULL << 24)
#define TCR_ORGN1_NC            (0ULL << 26)
#define TCR_ORGN1_WB            (1ULL << 26)
#define TCR_ORGN1_WT            (2ULL << 26)
#define TCR_SH0_INNER           (3ULL << 12)
#define TCR_SH1_INNER           (3ULL << 28)
#define TCR_EPD0                (1ULL << 7)    /* Disable TTBR0 walks */
#define TCR_EPD1                (1ULL << 23)   /* Disable TTBR1 walks */
#define TCR_A1                  (1ULL << 22)   /* ASID in TTBR1 */

/* TCR default configuration: 48-bit VA, 4 KiB granules, Write-Back cacheable */
#define TCR_DEFAULT  ( \
    TCR_TG0_4K | TCR_TG1_4K | \
    TCR_IRGN0_WB | TCR_ORGN0_WB | \
    TCR_IRGN1_WB | TCR_ORGN1_WB | \
    TCR_SH0_INNER | TCR_SH1_INNER | \
    (16ULL << TCR_T0SZ_SHIFT) | (16ULL << TCR_T1SZ_SHIFT) \
)

/* ================================================================
 * SCTLR_EL1 - System Control Register
 * ================================================================ */
#define SCTLR_M                 (1 << 0)    /* MMU enable */
#define SCTLR_A                 (1 << 1)    /* Alignment check */
#define SCTLR_C                 (1 << 2)    /* Data cache enable */
#define SCTLR_SA                (1 << 3)    /* Stack alignment check */
#define SCTLR_I                 (1 << 12)   /* Instruction cache enable */
#define SCTLR_nAA               (1 << 6)    /* Non-aligned access */
#define SCTLR_WXN               (1 << 19)   /* Write implies Execute Never */

/* ================================================================
 * Helper functions to access system registers
 * ================================================================ */

/* Read TTBR0_EL1 */
static inline uint64_t read_ttbr0_el1(void) {
    uint64_t val;
    asm volatile ("mrs %0, ttbr0_el1" : "=r"(val));
    return val;
}

/* Write TTBR0_EL1 */
static inline void write_ttbr0_el1(uint64_t val) {
    asm volatile ("msr ttbr0_el1, %0" : : "r"(val) : "memory");
}

/* Read TTBR1_EL1 */
static inline uint64_t read_ttbr1_el1(void) {
    uint64_t val;
    asm volatile ("mrs %0, ttbr1_el1" : "=r"(val));
    return val;
}

/* Write TTBR1_EL1 */
static inline void write_ttbr1_el1(uint64_t val) {
    asm volatile ("msr ttbr1_el1, %0" : : "r"(val) : "memory");
}

/* Read TCR_EL1 */
static inline uint64_t read_tcr_el1(void) {
    uint64_t val;
    asm volatile ("mrs %0, tcr_el1" : "=r"(val));
    return val;
}

/* Write TCR_EL1 */
static inline void write_tcr_el1(uint64_t val) {
    asm volatile ("msr tcr_el1, %0" : : "r"(val) : "memory");
    asm volatile ("isb" ::: "memory");
}

/* Read MAIR_EL1 */
static inline uint64_t read_mair_el1(void) {
    uint64_t val;
    asm volatile ("mrs %0, mair_el1" : "=r"(val));
    return val;
}

/* Write MAIR_EL1 */
static inline void write_mair_el1(uint64_t val) {
    asm volatile ("msr mair_el1, %0" : : "r"(val) : "memory");
    asm volatile ("isb" ::: "memory");
}

/* TLB invalidation */
static inline void tlbi_vmalle1(void) {
    asm volatile ("tlbi vmalle1" ::: "memory");
    asm volatile ("dsb ish; isb" ::: "memory");
}

static inline void tlbi_vae1(uint64_t va) {
    asm volatile ("tlbi vae1, %0" : : "r"(va >> 12) : "memory");
    asm volatile ("dsb ish; isb" ::: "memory");
}

#endif /* ARCH_AARCH64_PAGETABLE_H */
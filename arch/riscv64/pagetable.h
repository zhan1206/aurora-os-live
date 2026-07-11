/*
 * arch/riscv64/pagetable.h - RISC-V Sv39 page table definitions
 *
 * Sv39 is the RISC-V three-level page table scheme for 39-bit virtual
 * addresses.  It uses 4 KiB pages with a 3-level hierarchy:
 *   VPN[2] (9 bits) → VPN[1] (9 bits) → VPN[0] (9 bits) → offset (12 bits)
 *
 * The SATP CSR controls the root page table and addressing mode.
 */
#ifndef ARCH_RISCV64_PAGETABLE_H
#define ARCH_RISCV64_PAGETABLE_H

/* Page size */
#define PAGE_SIZE       4096ULL
#define PAGE_SHIFT      12
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/* PTE flags (RISC-V privileged spec v1.12) */
#define PTE_V           (1ULL << 0)     /* Valid */
#define PTE_R           (1ULL << 1)     /* Readable */
#define PTE_W           (1ULL << 2)     /* Writable */
#define PTE_X           (1ULL << 3)     /* Executable */
#define PTE_U           (1ULL << 4)     /* User accessible */
#define PTE_G           (1ULL << 5)     /* Global mapping */
#define PTE_A           (1ULL << 6)     /* Accessed */
#define PTE_D           (1ULL << 7)     /* Dirty */

/* PTE compound flags */
#define PTE_RWX         (PTE_R | PTE_W | PTE_X)
#define PTE_RW          (PTE_R | PTE_W)
#define PTE_RX          (PTE_R | PTE_X)
#define PTE_URWX        (PTE_U | PTE_R | PTE_W | PTE_X)

/* PTE physical address mask (bits 53:10) */
#define PTE_ADDR_MASK   0x003FFFFFFFFFFC00ULL

/* SATP CSR (Supervisor Address Translation and Protection) */
#define SATP_MODE_OFF   0ULL
#define SATP_MODE_SV39  (8ULL << 60)    /* Sv39 mode */
#define SATP_MODE_SV48  (9ULL << 60)    /* Sv48 mode */
#define SATP_MODE_SV57  (10ULL << 60)   /* Sv57 mode */

/* SATP register: MODE(4 bits) | ASID(16 bits) | PPN(44 bits) */
#define SATP_PPN_MASK   0x00000FFFFFFFFFFFULL
#define SATP_ASID_SHIFT 44
#define SATP_MODE_SHIFT 60

/* VPN masks for Sv39 (each level is 9 bits) */
#define VPN0_SHIFT      12
#define VPN1_SHIFT      21
#define VPN2_SHIFT      30
#define VPN_MASK        0x1FF

/* Helper: build SATP value from root page table PPN and mode */
#define MAKE_SATP(ppn, mode) \
    ((mode) | ((ppn) & SATP_PPN_MASK))

/* Helper: set SATP register */
static inline void satp_write(uint64_t satp_val) {
    asm volatile ("csrw satp, %0" : : "r"(satp_val) : "memory");
}

/* Helper: read SATP register */
static inline uint64_t satp_read(void) {
    uint64_t val;
    asm volatile ("csrr %0, satp" : "=r"(val));
    return val;
}

/* TLB flush (sfence.vma) */
static inline void sfence_vma(void) {
    asm volatile ("sfence.vma x0, x0" ::: "memory");
}

static inline void sfence_vma_asid(uint64_t asid) {
    asm volatile ("sfence.vma x0, %0" : : "r"(asid) : "memory");
}

#endif /* ARCH_RISCV64_PAGETABLE_H */
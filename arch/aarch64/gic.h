/*
 * arch/aarch64/gic.h - ARM Generic Interrupt Controller (GIC) definitions
 *
 * Defines GIC distributor (GICD) and CPU interface (GICC) register offsets
 * for GICv2 and GICv3.  Used for interrupt handling on ARM platforms.
 *
 * Reference: ARM GIC Architecture Specification v2.0/v3.0
 */
#ifndef ARCH_AARCH64_GIC_H
#define ARCH_AARCH64_GIC_H

#include <stdint.h>

/* ================================================================
 * GIC Distributor (GICD) register offsets
 * ================================================================ */
#define GICD_CTLR               0x0000   /* Distributor Control Register */
#define GICD_TYPER              0x0004   /* Interrupt Controller Type Register */
#define GICD_IIDR               0x0008   /* Distributor Implementer Identification */
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))  /* Interrupt Group Registers */
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))  /* Interrupt Set-Enable Registers */
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))  /* Interrupt Clear-Enable Registers */
#define GICD_ISPENDR(n)         (0x0200 + 4 * (n))  /* Interrupt Set-Pending Registers */
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))  /* Interrupt Clear-Pending Registers */
#define GICD_ISACTIVER(n)       (0x0300 + 4 * (n))  /* Interrupt Set-Active Registers */
#define GICD_ICACTIVER(n)       (0x0380 + 4 * (n))  /* Interrupt Clear-Active Registers */
#define GICD_IPRIORITYR(n)      (0x0400 + 4 * (n))  /* Interrupt Priority Registers */
#define GICD_ITARGETSR(n)       (0x0800 + 4 * (n))  /* Interrupt Processor Targets */
#define GICD_ICFGR(n)           (0x0C00 + 4 * (n))  /* Interrupt Configuration Registers */
#define GICD_SGIR               0x0F00   /* Software Generated Interrupt Register */
#define GICD_CPENDSGIR(n)       (0x0F10 + 4 * (n))  /* SGI Clear-Pending */
#define GICD_SPENDSGIR(n)       (0x0F20 + 4 * (n))  /* SGI Set-Pending */

/* GICD_CTLR bits */
#define GICD_CTLR_ENABLE        (1 << 0)

/* GICD_TYPER bit fields */
#define GICD_TYPER_ITLINESNUM_MASK   0x1F
#define GICD_TYPER_CPU_NUM_SHIFT     5
#define GICD_TYPER_CPU_NUM_MASK      (7 << 5)

/* ================================================================
 * GIC CPU Interface (GICC) register offsets
 * ================================================================ */
#define GICC_CTLR               0x0000   /* CPU Interface Control Register */
#define GICC_PMR                0x0004   /* Interrupt Priority Mask Register */
#define GICC_BPR                0x0008   /* Binary Point Register */
#define GICC_IAR                0x000C   /* Interrupt Acknowledge Register */
#define GICC_EOIR               0x0010   /* End of Interrupt Register */
#define GICC_RPR                0x0014   /* Running Priority Register */
#define GICC_HPPIR              0x0018   /* Highest Priority Pending Interrupt */
#define GICC_IIDR               0x00FC   /* CPU Interface Identification */

/* GICC_CTLR bits */
#define GICC_CTLR_ENABLE        (1 << 0)
#define GICC_CTLR_EOIMODE       (1 << 9)   /* EOImode (GICv2) */

/* GICC_IAR fields */
#define GICC_IAR_INTID_MASK     0x3FF
#define GICC_IAR_CPUID_SHIFT    10
#define GICC_IAR_CPUID_MASK     (7 << 10)

/* GICC_EOIR fields */
#define GICC_EOIR_INTID_MASK    0x3FF

/* ================================================================
 * GICv3 Redistributor (GICR) register offsets
 * ================================================================ */
#define GICR_CTLR               0x0000   /* Redistributor Control */
#define GICR_IIDR               0x0004   /* Implementer Identification */
#define GICR_TYPER              0x0008   /* Redistributor Type */
#define GICR_WAKER              0x0014   /* Redistributor Wake */
#define GICR_IGROUPR0           0x0080   /* Interrupt Group 0 */
#define GICR_ISENABLER0         0x0100   /* Set-Enable */
#define GICR_ICENABLER0         0x0180   /* Clear-Enable */
#define GICR_ISPENDR0           0x0200   /* Set-Pending */
#define GICR_ICPENDR0           0x0280   /* Clear-Pending */
#define GICR_IPRIORITYR(n)      (0x0400 + 4 * (n))  /* Priority */

/* GICR_WAKER bits */
#define GICR_WAKER_PROCESSORSLEEP (1 << 1)
#define GICR_WAKER_CHILDRENASLEEP (1 << 2)

/* ================================================================
 * GICv3 System Register (ICC_) interface
 * Accessed via MRS/MSR instructions
 * ================================================================ */
#define ICC_IAR1_EL1            S3_0_C12_C12_0   /* Interrupt Acknowledge */
#define ICC_EOIR1_EL1           S3_0_C12_C12_1   /* End of Interrupt */
#define ICC_PMR_EL1             S3_0_C4_C6_0     /* Priority Mask */
#define ICC_BPR1_EL1            S3_0_C12_C12_3   /* Binary Point */
#define ICC_CTLR_EL1            S3_0_C12_C12_4   /* Control */
#define ICC_SRE_EL1             S3_0_C12_C12_5   /* System Register Enable */
#define ICC_IGRPEN1_EL1         S3_0_C12_C12_7   /* Group 1 Enable */

/* ICC_SRE_EL1 bits */
#define ICC_SRE_SRE             (1 << 0)   /* System Register Enable */
#define ICC_SRE_DFB             (1 << 1)   /* Disable FIQ Bypass */
#define ICC_SRE_DIB             (1 << 2)   /* Disable IRQ Bypass */

/* Special interrupt IDs */
#define INT_ID_SGI_BASE         0
#define INT_ID_SGI_MAX          15
#define INT_ID_PPI_BASE         16
#define INT_ID_PPI_MAX          31
#define INT_ID_SPI_BASE         32
#define INT_ID_SPI_MAX          1019
#define INT_ID_SPURIOUS         1023

/* ================================================================
 * Helper functions
 * ================================================================ */

/* Write to a GICD register (memory-mapped) */
static inline void gicd_write(uintptr_t base, uint32_t offset, uint32_t val) {
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);
    *reg = val;
}

/* Read from a GICD register (memory-mapped) */
static inline uint32_t gicd_read(uintptr_t base, uint32_t offset) {
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);
    return *reg;
}

/* Write to a GICC register (memory-mapped) */
static inline void gicc_write(uintptr_t base, uint32_t offset, uint32_t val) {
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);
    *reg = val;
}

/* Read from a GICC register (memory-mapped) */
static inline uint32_t gicc_read(uintptr_t base, uint32_t offset) {
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);
    return *reg;
}

#endif /* ARCH_AARCH64_GIC_H */
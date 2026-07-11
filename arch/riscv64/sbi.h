/*
 * arch/riscv64/sbi.h - RISC-V Supervisor Binary Interface (SBI)
 *
 * Provides a C interface to the SBI firmware (OpenSBI, BBL, etc.).
 * SBI calls are made via the ecall instruction with a7 = extension ID,
 * a6 = function ID, and a0-a5 for arguments.
 *
 * Reference: RISC-V SBI Specification v0.2/v0.3
 */
#ifndef ARCH_RISCV64_SBI_H
#define ARCH_RISCV64_SBI_H

#include <stdint.h>

/* SBI Extension IDs (EID) */
#define SBI_EXT_BASE            0x10
#define SBI_EXT_TIME            0x54494D45
#define SBI_EXT_IPI             0x735049
#define SBI_EXT_RFENCE          0x52464E43
#define SBI_EXT_HSM             0x48534D
#define SBI_EXT_SRST            0x53525354

/* SBI Base functions */
#define SBI_BASE_GET_SPEC_VERSION   0
#define SBI_BASE_GET_IMP_ID         1
#define SBI_BASE_GET_IMP_VERSION    2
#define SBI_BASE_PROBE_EXT          3

/* SBI legacy extension IDs */
#define SBI_LEGACY_SET_TIMER        0x00
#define SBI_LEGACY_CONSOLE_PUTCHAR  0x01
#define SBI_LEGACY_CONSOLE_GETCHAR  0x02
#define SBI_LEGACY_CLEAR_IPI        0x03
#define SBI_LEGACY_SEND_IPI         0x04
#define SBI_LEGACY_SHUTDOWN         0x08

/* SBI error codes */
#define SBI_SUCCESS              0
#define SBI_ERR_FAILED          -1
#define SBI_ERR_NOT_SUPPORTED   -2
#define SBI_ERR_INVALID_PARAM   -3

/* ================================================================
 * SBI ecall wrapper
 * ================================================================ */
struct sbiret {
    long error;
    long value;
};

static inline struct sbiret sbi_ecall(long ext, long fid,
                                       long a0, long a1, long a2,
                                       long a3, long a4, long a5) {
    struct sbiret ret;
    register long a7 asm("a7") = ext;
    register long a6 asm("a6") = fid;
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a3 asm("a3") = a3;
    register long r_a4 asm("a4") = a4;
    register long r_a5 asm("a5") = a5;

    asm volatile ("ecall"
        : "+r"(r_a0), "+r"(r_a1)
        : "r"(a7), "r"(a6), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5)
        : "memory");

    ret.error = r_a0;
    ret.value = r_a1;
    return ret;
}

/* ================================================================
 * SBI convenience functions
 * ================================================================ */

/* Output a character to the debug console */
static inline void sbi_putchar(int ch) {
    sbi_ecall(SBI_LEGACY_CONSOLE_PUTCHAR, 0, ch, 0, 0, 0, 0, 0);
}

/* Read a character from the debug console (blocking) */
static inline int sbi_getchar(void) {
    struct sbiret ret = sbi_ecall(SBI_LEGACY_CONSOLE_GETCHAR, 0, 0, 0, 0, 0, 0, 0);
    return (int)ret.error;
}

/* Set the timer for the next interrupt after stime_value ticks */
static inline void sbi_set_timer(uint64_t stime_value) {
    sbi_ecall(SBI_LEGACY_SET_TIMER, 0, (long)stime_value, 0, 0, 0, 0, 0);
}

/* Shutdown the system */
static inline void sbi_shutdown(void) {
    sbi_ecall(SBI_LEGACY_SHUTDOWN, 0, 0, 0, 0, 0, 0, 0);
}

/* Send an inter-processor interrupt to a hart */
static inline void sbi_send_ipi(const unsigned long *hart_mask) {
    sbi_ecall(SBI_LEGACY_SEND_IPI, 0, (long)(uintptr_t)hart_mask, 0, 0, 0, 0, 0);
}

/* Probe if an SBI extension is available */
static inline long sbi_probe_extension(long ext_id) {
    struct sbiret ret = sbi_ecall(SBI_EXT_BASE, SBI_BASE_PROBE_EXT,
                                   ext_id, 0, 0, 0, 0, 0);
    return ret.error ? 0 : ret.value;
}

#endif /* ARCH_RISCV64_SBI_H */
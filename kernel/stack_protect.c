/*
 * stack_protect.c - Stack canary and guard page implementation
 *
 * Provides:
 *   - __stack_chk_guard: A randomized 64-bit canary value placed by GCC
 *     between local variables and the return address.  GCC's
 *     -fstack-protector-strong generates code to check this canary
 *     before each function return.
 *   - __stack_chk_fail: Called when the canary is corrupted (stack
 *     buffer overflow detected).  Prints diagnostic info and panics.
 *
 * The canary is initialized early in boot via stack_protect_init(),
 * using RDTSC for entropy.
 */
#include "stack_protect.h"
#include "include/log.h"
#include <stdint.h>

/* ================================================================
 * Stack canary (global, referenced by GCC-generated code)
 * ================================================================ */

/*
 * __stack_chk_guard: The canary value that GCC places on the stack
 * at function entry and checks before function return.
 *
 * Initialized to a non-zero pattern so that if stack_protect_init()
 * is somehow not called, the canary is still non-trivial (not zero).
 * The real randomization happens in stack_protect_init().
 */
uint64_t __stack_chk_guard = 0xDEADBEEF1BADB002ULL;

/* ================================================================
 * Initialization
 * ================================================================ */

void stack_protect_init(void) {
    uint64_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    uint64_t tsc = (tsc_high << 32) | tsc_low;

    /*
     * Generate a random canary value.  We mix TSC with a fixed
     * constant to ensure adequate entropy.  The canary must be
     * non-zero because the stack protector check uses XOR with
     * the stored canary (a zero canary would mask overflows).
     */
    __stack_chk_guard = tsc ^ 0x9E3779B97F4A7C15ULL;
    __stack_chk_guard ^= __stack_chk_guard >> 33;
    __stack_chk_guard *= 0xFF51AFD7ED558CCDULL;
    __stack_chk_guard ^= __stack_chk_guard >> 33;

    /* Ensure canary is never zero */
    if (__stack_chk_guard == 0) __stack_chk_guard = 0xDEADBEEF1BADB002ULL;

    log_printf(LOG_LEVEL_INFO, "Stack protector initialized\n");
}

/* ================================================================
 * Stack smashing detected
 * ================================================================ */

/*
 * __stack_chk_fail: Called by GCC-generated code when a stack buffer
 * overflow corrupts the canary value.
 *
 * This function is declared noreturn — it will call panic() which
 * halts the system.  We print diagnostic information to help identify
 * which task triggered the fault.
 */
void __stack_chk_fail(void) {
    /*
     * We can't safely use log_printf here because the stack may be
     * corrupted.  Use the low-level panic() which is designed to
     * work in emergency situations.
     */
    extern void panic(const char *fmt, ...);
    panic("Stack smashing detected! Stack canary has been corrupted.\n"
          "This indicates a buffer overflow in a kernel function.\n"
          "Check function local arrays for out-of-bounds writes.");
    __builtin_unreachable();
}
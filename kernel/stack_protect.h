/*
 * stack_protect.h - Stack canary and guard page protection
 *
 * Stack canary: A random 64-bit value placed between the return address
 * and local variables.  GCC's -fstack-protector-strong inserts checks
 * that compare the canary before function return.  If the canary is
 * corrupted, __stack_chk_fail() is called to panic.
 *
 * Guard pages: A non-present page mapped at the bottom of each user
 * stack.  Any stack overflow into this page triggers a page fault,
 * which the kernel converts to a SIGSEGV delivered to the process.
 */
#ifndef STACK_PROTECT_H
#define STACK_PROTECT_H

#include <stdint.h>

/* ================================================================
 * Stack canary
 * ================================================================ */

/*
 * __stack_chk_guard: Global stack canary value, randomized at boot.
 * GCC references this symbol when -fstack-protector-strong is enabled.
 * Must be defined in the kernel binary (NOT in a shared library).
 */
extern uint64_t __stack_chk_guard;

/*
 * stack_protect_init: Initialize the stack canary with a random value.
 * Must be called early in boot, before any C code that uses stack
 * protection (i.e., before any function with local arrays).
 */
void stack_protect_init(void);

/*
 * __stack_chk_fail: Called by GCC-generated code when stack smashing
 * is detected.  Prints an error message and calls panic().
 * noreturn attribute ensures the compiler knows this never returns.
 */
void __stack_chk_fail(void) __attribute__((noreturn));

/* ================================================================
 * Guard page constants
 * ================================================================ */

/*
 * GUARD_PAGE_SIZE: Size of a single guard page (4KB).
 * One non-present page is placed at the bottom of each user stack.
 */
#define GUARD_PAGE_SIZE  4096ULL

/*
 * GUARD_PAGE_FLAGS: Page table flags for the guard page.
 * A non-present page (PTE_PRESENT cleared) with user bit set
 * so that user-mode access triggers a page fault.
 */
#define GUARD_PAGE_FLAGS 0ULL  /* non-present, no flags */

#endif /* STACK_PROTECT_H */
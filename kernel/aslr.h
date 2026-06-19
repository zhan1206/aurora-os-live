/*
 * aslr.h - Address Space Layout Randomization definitions
 *
 * Provides randomized base addresses for mmap, stack, and other
 * user-space memory regions to mitigate memory corruption exploits.
 *
 * Randomization uses a simple xorshift64 PRNG seeded at boot time
 * via RDTSC or PIT tick counter.
 */
#ifndef ASLR_H
#define ASLR_H

#include <stdint.h>

/* ================================================================
 * ASLR constants
 * ================================================================ */

/* Base address for mmap allocations (0x70000000000 = 7TB) */
#define ASLR_MMAP_BASE    0x70000000000ULL

/* Base address for user stack (0x7FFFF0000000 = 128TB - 1GB) */
#define ASLR_STACK_BASE   0x7FFFF0000000ULL

/* Maximum random shift: 1GB (0x40000000) */
#define ASLR_MAX_SHIFT    0x40000000ULL

/* ================================================================
 * API
 * ================================================================ */

/*
 * aslr_init: Seed the PRNG using RDTSC or PIT ticks.
 * Must be called early in boot, after memory init.
 */
void aslr_init(void);

/*
 * aslr_randomize_base: Add a random offset to a base address.
 * @base:       Base address to randomize.
 * @max_shift:  Maximum random offset (must be page-aligned).
 * Returns:     Randomized address (page-aligned).
 */
uint64_t aslr_randomize_base(uint64_t base, uint64_t max_shift);

/*
 * aslr_randomize_stack: Apply ASLR to the user stack base.
 * Should be called during user task creation.
 * Returns the randomized stack top address.
 */
uint64_t aslr_randomize_stack(void);

/*
 * aslr_randomize_mmap: Return a randomized mmap base address.
 * Should be called by sys_mmap for anonymous mappings.
 * Returns a page-aligned address within the mmap region.
 */
uint64_t aslr_randomize_mmap(void);

#endif /* ASLR_H */
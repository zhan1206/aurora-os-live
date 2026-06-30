/*
 * aslr.c - Address Space Layout Randomization implementation
 *
 * Uses a xorshift64 PRNG to generate random offsets for:
 *   - User stack base (within 1GB range)
 *   - mmap base (within 1GB range)
 *
 * The PRNG is seeded at boot time via RDTSC for entropy.
 */
#include "aslr.h"
#include "pagetable.h"
#include "include/log.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * xorshift64 PRNG
 * ================================================================ */

static uint64_t aslr_state = 0xDEADBEEFCAFEBABEULL;

/*
 * xorshift64_next: Generate the next pseudo-random 64-bit value.
 * Marsaglia's xorshift64* algorithm — fast and decent quality.
 */
static uint64_t xorshift64_next(void) {
    aslr_state ^= aslr_state << 13;
    aslr_state ^= aslr_state >> 7;
    aslr_state ^= aslr_state << 17;
    return aslr_state;
}

/* ================================================================
 * Initialization
 * ================================================================ */

void aslr_init(void) {
    uint64_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    uint64_t tsc = (tsc_high << 32) | tsc_low;

    /*
     * Mix TSC with a fixed seed to ensure we never start from zero.
     * TSC alone can be somewhat predictable at boot, but on real
     * hardware it provides enough entropy for our purposes.
     */
    aslr_state = tsc ^ 0x9E3779B97F4A7C15ULL;  /* golden ratio */
    aslr_state ^= aslr_state >> 33;
    aslr_state *= 0xFF51AFD7ED558CCDULL;
    aslr_state ^= aslr_state >> 33;

    /* Ensure state is non-zero (xorshift requires non-zero state) */
    if (aslr_state == 0) aslr_state = 1;

    log_printf(LOG_LEVEL_INFO, "ASLR initialized (seed=%p)\n",
               (void *)(uintptr_t)aslr_state);
}

/* ================================================================
 * Randomization functions
 * ================================================================ */

uint64_t aslr_randomize_base(uint64_t base, uint64_t max_shift) {
    if (max_shift == 0) return base;

    /*
     * Generate a page-aligned random offset.
     * Shift right by PAGE_SHIFT (12) to get a page number,
     * take modulo max_shift (in pages), then shift back.
     */
    uint64_t pages = max_shift / PAGE_SIZE;
    /* Guard against division by zero: if max_shift < PAGE_SIZE,
     * pages == 0 and xorshift64_next() % pages would #DE. */
    if (pages == 0) return base;
    uint64_t offset_pages = xorshift64_next() % pages;
    uint64_t offset = offset_pages * PAGE_SIZE;

    return base + offset;
}

uint64_t aslr_randomize_stack(void) {
    return aslr_randomize_base(ASLR_STACK_BASE, ASLR_MAX_SHIFT);
}

uint64_t aslr_randomize_mmap(void) {
    return aslr_randomize_base(ASLR_MMAP_BASE, ASLR_MAX_SHIFT);
}
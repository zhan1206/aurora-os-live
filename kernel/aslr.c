/*
 * aslr.c - Address Space Layout Randomization implementation
 *
 * Uses a xorshift64* PRNG to generate random offsets for:
 *   - User stack base (within 1GB range)
 *   - mmap base (within 1GB range)
 *
 * The PRNG is seeded at boot time from multiple entropy sources
 * (TSC + RDRAND if available) and run through mixing rounds.
 *
 * FIXED (v4.0.6):
 *   - Added multi-source entropy mixing (TSC + RDRAND)
 *   - Added additional mixing rounds to strengthen seed
 *   - Warning: xorshift64 is NOT cryptographically secure;
 *     for production, replace with ChaCha20 or similar CSPRNG.
 */
#include "aslr.h"
#include "pagetable.h"
#include "include/log.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * xorshift64* PRNG
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
 * Entropy mixing
 * ================================================================ */

static uint64_t mix_entropy(uint64_t a, uint64_t b) {
    uint64_t result = a ^ b;
    /* SplitMix64-style finalizer for better avalanche */
    result = (result ^ (result >> 30)) * 0xBF58476D1CE4E5B9ULL;
    result = (result ^ (result >> 27)) * 0x94D049BB133111EBULL;
    result = result ^ (result >> 31);
    return result;
}

/* ================================================================
 * Initialization
 * ================================================================ */

void aslr_init(void) {
    uint64_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    uint64_t tsc = (tsc_high << 32) | tsc_low;

    /* Try RDRAND for additional entropy (may not be available) */
    uint64_t rdrand_val = 0;
    int rdrand_ok = 0;
    asm volatile (
        "rdrand %0\n\t"
        "setc %1"
        : "=r"(rdrand_val), "=r"(rdrand_ok)
        :
        : "cc"
    );

    /*
     * Mix multiple entropy sources:
     *   - TSC (always available, somewhat predictable at boot)
     *   - RDRAND (hardware RNG, may not be available in VMs)
     *   - Fixed constants as fallback
     *
     * Multiple mixing rounds ensure good distribution even if
     * some sources are weak or unavailable.
     */
    uint64_t seed = mix_entropy(tsc, 0x9E3779B97F4A7C15ULL);  /* golden ratio */
    if (rdrand_ok) {
        seed = mix_entropy(seed, rdrand_val);
    }

    /* Run several mixing rounds to strengthen the seed */
    aslr_state = seed;
    for (int i = 0; i < 8; i++) {
        aslr_state = mix_entropy(aslr_state, xorshift64_next());
    }

    /* Ensure state is non-zero (xorshift requires non-zero state) */
    if (aslr_state == 0) aslr_state = 1;

    log_printf(LOG_LEVEL_INFO, "ASLR initialized (entropy: TSC%s)\n",
               rdrand_ok ? "+RDRAND" : " only");
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
/*
 * aslr.c - Address Space Layout Randomization implementation
 *
 * Uses a ChaCha20-based CSPRNG to generate random offsets for:
 *   - User stack base (within 1GB range)
 *   - mmap base (within 1GB range)
 *
 * The PRNG is seeded at boot time from multiple entropy sources
 * (TSC + RDRAND if available). The ChaCha20 key is 256 bits (32 bytes)
 * derived from the entropy, and a 96-bit (12-byte) nonce is
 * incremented for each call to chacha20_random().
 *
 * v4.0.7: Replaced xorshift64 with ChaCha20 CSPRNG for
 *         cryptographically secure randomization.
 */
#include "aslr.h"
#include "pagetable.h"
#include "include/log.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * ChaCha20 CSPRNG
 * ================================================================ */

static const uint32_t chacha20_const[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
/* "expand 32-byte k" */

/* Internal state: const[4] + key[8] + counter[1] + nonce[3] = 16 words */
static uint32_t chacha_state[16];

/*
 * chacha20_quarter_round: Perform the ChaCha20 quarter round operation
 * on four 32-bit words. This is the core diffusion primitive.
 */
static inline void chacha20_quarter_round(uint32_t *a, uint32_t *b,
                                           uint32_t *c, uint32_t *d) {
    *a += *b; *d ^= *a; *d = (*d << 16) | (*d >> 16);
    *c += *d; *b ^= *c; *b = (*b << 12) | (*b >> 20);
    *a += *b; *d ^= *a; *d = (*d <<  8) | (*d >> 24);
    *c += *d; *b ^= *c; *b = (*b <<  7) | (*b >> 25);
}

/*
 * chacha20_block: Generate one 64-byte ChaCha20 keystream block.
 * The input state is preserved; output is written to @out (must be 64 bytes).
 * The counter in chacha_state[12] is NOT incremented; the caller does that.
 */
static void chacha20_block(uint32_t *out) {
    uint32_t x[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = chacha_state[i];
    }

    /* 20 rounds: 10 double rounds */
    for (i = 0; i < 10; i++) {
        /* Column rounds */
        chacha20_quarter_round(&x[0], &x[4], &x[8],  &x[12]);
        chacha20_quarter_round(&x[1], &x[5], &x[9],  &x[13]);
        chacha20_quarter_round(&x[2], &x[6], &x[10], &x[14]);
        chacha20_quarter_round(&x[3], &x[7], &x[11], &x[15]);

        /* Diagonal rounds */
        chacha20_quarter_round(&x[0], &x[5], &x[10], &x[15]);
        chacha20_quarter_round(&x[1], &x[6], &x[11], &x[12]);
        chacha20_quarter_round(&x[2], &x[7], &x[8],  &x[13]);
        chacha20_quarter_round(&x[3], &x[4], &x[9],  &x[14]);
    }

    /* Add original state to the working state (finalize) */
    for (i = 0; i < 16; i++) {
        x[i] += chacha_state[i];
    }

    /* Serialize to 64 bytes (little-endian) */
    for (i = 0; i < 16; i++) {
        out[i] = x[i];
    }
}

/*
 * chacha20_encrypt: Encrypt/decrypt @len bytes at @in into @out using
 * ChaCha20 keystream starting from the current counter and nonce.
 * The state counter is advanced by the number of blocks consumed.
 * For our CSPRNG, @in and @out can be the same buffer (XOR with zero
 * gives the raw keystream).
 */
static void chacha20_encrypt(uint8_t *out, const uint8_t *in, size_t len) {
    uint32_t block[16];
    size_t i;

    while (len > 0) {
        chacha20_block(block);
        uint8_t *keystream = (uint8_t *)block;
        size_t chunk = (len < 64) ? len : 64;
        for (i = 0; i < chunk; i++) {
            out[i] = in[i] ^ keystream[i];
        }
        out += chunk;
        in  += chunk;
        len -= chunk;
        /* Increment counter (32-bit, wraps naturally) */
        chacha_state[12]++;
    }
}

/*
 * chacha20_random: Generate 64 random bytes (2 ChaCha20 blocks).
 * Returns the number of bytes written (always 64).
 */
static int chacha20_random(uint8_t *buf) {
    uint32_t block[16];

    /* First block */
    chacha20_block(block);
    chacha_state[12]++;
    for (int i = 0; i < 16; i++) {
        ((uint32_t *)buf)[i] = block[i];
    }

    /* Second block */
    chacha20_block(block);
    chacha_state[12]++;
    for (int i = 0; i < 16; i++) {
        ((uint32_t *)(buf + 32))[i] = block[i];
    }

    return 64;
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
    unsigned char rdrand_ok = 0;
    asm volatile (
        "rdrand %0\n\t"
        "setc %1"
        : "=r"(rdrand_val), "=qm"(rdrand_ok)
        :
        : "cc"
    );

    /*
     * Derive a 256-bit key from entropy sources:
     *   - TSC (always available, somewhat predictable at boot)
     *   - RDRAND (hardware RNG, may not be available in VMs)
     *
     * We generate 4 x 64-bit values via mixing, then pack them
     * into the 8 x 32-bit key words.
     */
    uint64_t e0 = mix_entropy(tsc, 0x9E3779B97F4A7C15ULL);  /* golden ratio */
    uint64_t e1 = mix_entropy(tsc ^ 0xAAAAAAAAAAAAAAAAULL, 0xBF58476D1CE4E5B9ULL);
    uint64_t e2 = mix_entropy(tsc ^ 0x5555555555555555ULL, 0x94D049BB133111EBULL);
    uint64_t e3 = mix_entropy(tsc ^ 0x3333333333333333ULL, 0xFF51AFD7ED558CCDULL);

    if (rdrand_ok) {
        e0 = mix_entropy(e0, rdrand_val);
        e1 = mix_entropy(e1, rdrand_val ^ 0xFFFFFFFFFFFFFFFFULL);
        e2 = mix_entropy(e2, ~rdrand_val);
        e3 = mix_entropy(e3, rdrand_val << 17);
    }

    /* Set up ChaCha20 state: const[4] + key[8] + counter[1] + nonce[3] */
    for (int i = 0; i < 4; i++) {
        chacha_state[i] = chacha20_const[i];
    }

    /* Key: 256 bits (8 x 32-bit words) */
    chacha_state[4]  = (uint32_t)(e0 & 0xFFFFFFFF);
    chacha_state[5]  = (uint32_t)(e0 >> 32);
    chacha_state[6]  = (uint32_t)(e1 & 0xFFFFFFFF);
    chacha_state[7]  = (uint32_t)(e1 >> 32);
    chacha_state[8]  = (uint32_t)(e2 & 0xFFFFFFFF);
    chacha_state[9]  = (uint32_t)(e2 >> 32);
    chacha_state[10] = (uint32_t)(e3 & 0xFFFFFFFF);
    chacha_state[11] = (uint32_t)(e3 >> 32);

    /* Counter starts at 0 */
    chacha_state[12] = 0;

    /* Nonce: 96 bits (12 bytes), derived from remaining entropy */
    chacha_state[13] = (uint32_t)(tsc & 0xFFFFFFFF);
    chacha_state[14] = (uint32_t)(tsc >> 32);
    chacha_state[15] = (uint32_t)(rdrand_ok ? rdrand_val : (tsc >> 16));

    /* Run a few blocks to mix the initial state */
    chacha_state[12] = 0;
    uint8_t discard[64];
    for (int i = 0; i < 8; i++) {
        chacha20_random(discard);
    }

    log_printf(LOG_LEVEL_INFO, "ASLR initialized (ChaCha20 CSPRNG, entropy: TSC%s)\n",
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
     * pages == 0 and modulo would #DE. */
    if (pages == 0) return base;

    uint8_t rnd[64];
    chacha20_random(rnd);
    uint64_t rand_val = *(uint64_t *)rnd;

    uint64_t offset_pages = rand_val % pages;
    uint64_t offset = offset_pages * PAGE_SIZE;

    return base + offset;
}

uint64_t aslr_randomize_stack(void) {
    return aslr_randomize_base(ASLR_STACK_BASE, ASLR_MAX_SHIFT);
}

uint64_t aslr_randomize_mmap(void) {
    return aslr_randomize_base(ASLR_MMAP_BASE, ASLR_MAX_SHIFT);
}

/*
 * NOTE: Shared library (DSO) load address randomization is not yet
 * implemented.  Currently, all shared libraries loaded via the dynamic
 * linker are placed at fixed addresses.  A future implementation should
 * randomize the mmap base for each library independently, using the
 * same ChaCha20 CSPRNG, to mitigate return-to-libc and similar attacks.
 */
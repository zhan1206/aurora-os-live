/*
 * module_sign.c - Kernel module signature verification
 *
 * Inspired by CoolPotOS's ECC module key verification mechanism
 * (CP_Kernel-x86_64-v0.4.39+).
 *
 * Uses SHA-256 for module content hashing. The signature verification
 * scheme is HMAC-based: hash = SHA-256(module_data), signature = hash ^ key.
 * This is a simplified scheme; production systems should use ECDSA or RSA.
 *
 * FIXED (v4.0.6):
 *   - Replaced XOR rolling hash with SHA-256
 *   - Fixed signature comparison to use full sig_size (64 bytes)
 *   - Added constant-time comparison to prevent timing side-channels
 *   - Integrated into module_load() entry point
 *
 * The public key is embedded at compile time. For production, the key
 * should be generated at build time and injected by the build system.
 */

#include "module.h"
#include "include/log.h"
#include "include/kstdio.h"
#include <string.h>

/* ================================================================
 * SHA-256 implementation
 * ================================================================ */

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;

    /* Prepare message schedule */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    /* Initialize working variables */
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    /* 64 rounds */
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t digest[32]) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t block[64];
    size_t block_idx = 0;
    uint64_t bit_len = (uint64_t)len * 8;

    /* Process full blocks */
    for (size_t i = 0; i < len; i++) {
        block[block_idx++] = data[i];
        if (block_idx == 64) {
            sha256_transform(state, block);
            block_idx = 0;
        }
    }

    /* Padding */
    block[block_idx++] = 0x80;
    if (block_idx > 56) {
        memset(block + block_idx, 0, 64 - block_idx);
        sha256_transform(state, block);
        block_idx = 0;
    }
    memset(block + block_idx, 0, 56 - block_idx);

    /* Append bit length (big-endian) */
    for (int i = 0; i < 8; i++) {
        block[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    sha256_transform(state, block);

    /* Output digest (big-endian) */
    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(state[i]);
    }
}

/* ================================================================
 * Constant-time memory comparison
 *
 * Prevents timing side-channel attacks on signature verification.
 * Returns 0 if buffers are equal, non-zero otherwise.
 * ================================================================ */
static int constant_time_memcmp(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return result;
}

/* ================================================================
 * Module signature header (appended to .ko file)
 * ================================================================ */
#define MODULE_SIGN_MAGIC   0x434F4F4C504F5431ULL  /* "COOLPOT1" */
#define MODULE_SIGN_VERSION 1
#define MODULE_SIGN_SIZE    64  /* 512-bit signature (SHA-256 hash XOR'd with key) */

struct module_sign_header {
    uint64_t magic;          /* MODULE_SIGN_MAGIC */
    uint32_t version;        /* 1 */
    uint32_t sig_size;       /* MODULE_SIGN_SIZE */
    uint8_t  signature[MODULE_SIGN_SIZE];
    uint8_t  reserved[32];
} __attribute__((packed));

/* ================================================================
 * Embedded public key (compile-time constant)
 *
 * In production, this should be generated at build time and
 * injected by the build system. The current key is a placeholder
 * and must be replaced for any real security use.
 * ================================================================ */
static const uint8_t public_key[MODULE_SIGN_SIZE] = {
    0x41, 0x75, 0x72, 0x6f, 0x72, 0x61, 0x4f, 0x53,
    0x5f, 0x4d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x4b,
    0x65, 0x79, 0x5f, 0x32, 0x30, 0x32, 0x36, 0x30,
    0x36, 0x31, 0x39, 0x5f, 0x76, 0x31, 0x2e, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ================================================================
 * module_sign_verify: Verify a module's signature using SHA-256.
 *
 * @module_data:  pointer to the module binary in memory
 * @module_size:  size of the module binary (including signature)
 *
 * Returns 0 on success, -1 on failure.
 *
 * The module binary layout:
 *   [module ELF data] [module_sign_header]
 *
 * The signature covers only the module ELF data, not the header.
 * Scheme: hash = SHA-256(module_data), signature = hash ^ key.
 * ================================================================ */
int module_sign_verify(const uint8_t *module_data, size_t module_size) {
    if (!module_data || module_size < sizeof(struct module_sign_header)) {
        log_printf(LOG_LEVEL_ERR, "module_sign: module too small for signature\n");
        return -1;
    }

    /* Locate the signature header at the end of the module */
    const struct module_sign_header *hdr =
        (const struct module_sign_header *)(module_data + module_size - sizeof(*hdr));

    /* Verify magic */
    if (hdr->magic != MODULE_SIGN_MAGIC) {
        log_printf(LOG_LEVEL_ERR, "module_sign: bad magic 0x%lx\n",
                   (unsigned long)hdr->magic);
        return -1;
    }

    /* Verify version */
    if (hdr->version != MODULE_SIGN_VERSION) {
        log_printf(LOG_LEVEL_ERR, "module_sign: unsupported version %d\n",
                   (int)hdr->version);
        return -1;
    }

    /* Verify signature size */
    if (hdr->sig_size != MODULE_SIGN_SIZE) {
        log_printf(LOG_LEVEL_ERR, "module_sign: bad signature size %d\n",
                   (int)hdr->sig_size);
        return -1;
    }

    /* Compute SHA-256 of the module data (excluding the signature header) */
    size_t data_size = module_size - sizeof(*hdr);
    uint8_t computed_hash[32];
    sha256(module_data, data_size, computed_hash);

    /*
     * Verify the signature using HMAC-like scheme:
     *   expected_signature = SHA-256(module_data) ^ public_key
     * Compare with constant-time memcmp using the full sig_size.
     */
    uint8_t expected[MODULE_SIGN_SIZE];
    memset(expected, 0, sizeof(expected));

    /* XOR the 32-byte hash with the key to produce the expected 64-byte signature */
    for (int i = 0; i < 32; i++) {
        expected[i] = computed_hash[i] ^ public_key[i];
    }
    /* The upper 32 bytes are XOR'd with the upper half of the key */
    for (int i = 32; i < MODULE_SIGN_SIZE; i++) {
        expected[i] = public_key[i];
    }

    /* Constant-time comparison of full signature */
    if (constant_time_memcmp(expected, hdr->signature, MODULE_SIGN_SIZE) != 0) {
        log_printf(LOG_LEVEL_ERR, "module_sign: signature verification failed\n");
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "module_sign: signature verified (SHA-256)\n");
    return 0;
}

/*
 * module_sign_is_enabled: Check if module signature verification is enabled.
 *
 * Controlled by the MODULE_SIGN_CHECK compile-time flag.
 * When enabled, all modules must pass signature verification.
 * When disabled, modules are loaded without verification (development mode).
 */
int module_sign_is_enabled(void) {
#ifdef MODULE_SIGN_CHECK
    return 1;
#else
    return 0;
#endif
}
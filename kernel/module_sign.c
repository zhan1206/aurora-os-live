/*
 * module_sign.c - Kernel module signature verification (DEMONSTRATION)
 *
 * Inspired by CoolPotOS's ECC module key verification mechanism
 * (CP_Kernel-x86_64-v0.4.39+).
 *
 * IMPORTANT: This is a DEMONSTRATION / PLACEHOLDER implementation.
 *   - Uses a simple XOR-based rolling hash, NOT SHA-256
 *   - Uses a hardcoded ASCII placeholder key, NOT a real cryptographic key
 *   - The MODULE_SIGN_CHECK macro is not defined in any build configuration
 *   - module_sign_verify() is not called from module_load()
 *   - In its current state, this code provides ZERO security benefit
 *
 * For production use, this would need:
 *   1. A real cryptographic hash (SHA-256 or SHA-512)
 *   2. A proper key pair (RSA or ECDSA) with secure key management
 *   3. Integration into the module_load() path
 *   4. A build system step to sign modules
 *
 * The module signing process (as designed):
 *   1. Build system computes SHA-256 of the module binary
 *   2. The hash is signed with a private key (RSA or ECDSA)
 *   3. The signature is appended to the module binary
 *   4. During module_load(), the kernel verifies the signature
 *      using the embedded public key.
 */

#include "module.h"
#include "include/log.h"
#include "include/kstdio.h"
#include <string.h>

/* ================================================================
 * Module signature header (appended to .ko file)
 * ================================================================ */
#define MODULE_SIGN_MAGIC   0x434F4F4C504F5431ULL  /* "COOLPOT1" */
#define MODULE_SIGN_VERSION 1
#define MODULE_SIGN_SIZE    64  /* 512-bit signature */

struct module_sign_header {
    uint64_t magic;          /* MODULE_SIGN_MAGIC */
    uint32_t version;        /* 1 */
    uint32_t sig_size;       /* MODULE_SIGN_SIZE */
    uint8_t  signature[MODULE_SIGN_SIZE];
    uint8_t  reserved[32];
} __attribute__((packed));

/* ================================================================
 * Embedded public key (compile-time constant)
 * In production, this would be generated at build time and
 * embedded by the build system.
 * ================================================================ */
static const uint8_t public_key[32] = {
    0x41, 0x75, 0x72, 0x6f, 0x72, 0x61, 0x4f, 0x53,
    0x5f, 0x4d, 0x6f, 0x64, 0x75, 0x6c, 0x65, 0x4b,
    0x65, 0x79, 0x5f, 0x32, 0x30, 0x32, 0x36, 0x30,
    0x36, 0x31, 0x39, 0x5f, 0x76, 0x31, 0x2e, 0x30
};

/* ================================================================
 * Simple HMAC-SHA256-like verification
 *
 * For a real implementation, use a proper cryptographic library
 * such as tinycrypt (used by CoolPotOS) or OpenSSL.
 * This simplified version uses XOR-based "signature" verification
 * for demonstration purposes.
 * ================================================================ */

/*
 * module_sign_verify: Verify a module's signature.
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
 */
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

    /*
     * Compute a simple "hash" of the module data (excluding the signature header).
     * In production, use SHA-256 or SHA-512.
     */
    size_t data_size = module_size - sizeof(*hdr);
    uint8_t computed_hash[32];
    memset(computed_hash, 0, sizeof(computed_hash));

    /* Simple rolling hash (not cryptographically secure, for demonstration) */
    for (size_t i = 0; i < data_size; i++) {
        computed_hash[i % 32] ^= module_data[i];
        computed_hash[(i * 7 + 13) % 32] += module_data[i];
    }

    /*
     * Verify the signature: XOR the computed hash with the public key,
     * then compare with the stored signature (first 32 bytes).
     * In production, use proper ECDSA/RSA verification.
     */
    uint8_t expected[32];
    for (int i = 0; i < 32; i++) {
        expected[i] = computed_hash[i] ^ public_key[i];
    }

    if (memcmp(expected, hdr->signature, 32) != 0) {
        log_printf(LOG_LEVEL_ERR, "module_sign: signature verification failed\n");
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "module_sign: signature verified successfully\n");
    return 0;
}

/*
 * module_sign_is_enabled: Check if module signature verification is enabled.
 *
 * Controlled by the MODULE_SIGN_CHECK compile-time flag.
 * When disabled, all modules are loaded without verification.
 */
int module_sign_is_enabled(void) {
#ifdef MODULE_SIGN_CHECK
    return 1;
#else
    return 0;
#endif
}
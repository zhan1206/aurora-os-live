/*
 * cmdline.c - Kernel command line implementation
 *
 * Stores and provides access to the kernel command line
 * parsed from the bootloader.
 */
#include "cmdline.h"
#include <string.h>

static char g_cmdline[CMDLINE_MAX_LEN] = "auroraos console=tty0 root=/dev/ram0 quiet";

const char *cmdline_get(void) {
    return g_cmdline;
}

void cmdline_set(const char *cmdline) {
    if (!cmdline) return;
    size_t len = strlen(cmdline);
    if (len >= CMDLINE_MAX_LEN) len = CMDLINE_MAX_LEN - 1;
    memcpy(g_cmdline, cmdline, len);
    g_cmdline[len] = '\0';
}

void cmdline_init(const char *default_cmdline) {
    if (default_cmdline) {
        cmdline_set(default_cmdline);
    }
}

int cmdline_has_flag(const char *flag) {
    if (!flag) return 0;
    const char *p = g_cmdline;
    size_t flen = strlen(flag);

    while (*p) {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (*p == '\0') break;

        /* Check if this word matches the flag */
        const char *end = p;
        while (*end && *end != ' ') end++;
        size_t wlen = (size_t)(end - p);

        if (wlen == flen && strncmp(p, flag, flen) == 0) {
            return 1;
        }

        p = end;
        if (*p == ' ') p++;
    }
    return 0;
}

const char *cmdline_get_option(const char *key) {
    if (!key) return NULL;
    const char *p = g_cmdline;
    size_t klen = strlen(key);

    while (*p) {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (*p == '\0') break;

        /* Find end of this word */
        const char *end = p;
        while (*end && *end != ' ') end++;

        /* Check if this word starts with key= */
        if ((size_t)(end - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return p + klen + 1; /* Return the value part */
        }

        p = end;
        if (*p == ' ') p++;
    }
    return NULL;
}
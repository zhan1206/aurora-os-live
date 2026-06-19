/*
 * sysctl.h - System control interface for AuroraOS kernel
 *
 * Provides a /proc-like interface for reading kernel statistics
 * and tuning parameters. Entries are registered at boot time
 * and can be queried by name.
 */

#ifndef SYSCTL_H
#define SYSCTL_H

#include <stdint.h>

/* ================================================================
 * Sysctl entry structure
 * ================================================================ */
struct sysctl_entry {
    const char *name;
    uint64_t (*read)(void);
    const char *desc;
};

/* ================================================================
 * Public API
 * ================================================================ */
int  sysctl_register(struct sysctl_entry *entry);
uint64_t sysctl_read(const char *name);
void sysctl_list(void (*callback)(const char *name, uint64_t value, const char *desc));
void sysctl_init(void);

#endif /* SYSCTL_H */
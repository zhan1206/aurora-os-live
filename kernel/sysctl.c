/*
 * sysctl.c - System control interface implementation
 *
 * Maintains a linked list of registered sysctl entries.
 * Provides name-based lookup and enumeration.
 * Entries are populated from the perf subsystem and other
 * kernel statistics providers.
 */

#include "sysctl.h"
#include "perf.h"
#include "mem.h"
#include "include/log.h"
#include <string.h>
#include <stddef.h>

/* ================================================================
 * Linked list of sysctl entries
 * ================================================================ */
#define SYSCTL_MAX_ENTRIES 64

static struct sysctl_entry *entries[SYSCTL_MAX_ENTRIES];
static int entry_count = 0;

/* ================================================================
 * Built-in read functions for perf counters
 * ================================================================ */
static uint64_t read_ctx_switches(void) {
    return perf.counters[PERF_CTX_SWITCHES].count;
}

static uint64_t read_syscall_count(void) {
    return perf.counters[PERF_SYSCALL_COUNT].count;
}

static uint64_t read_syscall_latency(void) {
    if (perf.counters[PERF_SYSCALL_LATENCY].count == 0) return 0;
    return perf.counters[PERF_SYSCALL_LATENCY].total_latency /
           perf.counters[PERF_SYSCALL_LATENCY].count;
}

static uint64_t read_page_faults(void) {
    return perf.counters[PERF_PAGE_FAULTS].count;
}

static uint64_t read_cow_count(void) {
    return perf.counters[PERF_COW_COUNT].count;
}

static uint64_t read_malloc_count(void) {
    return perf.counters[PERF_MALLOC_COUNT].count;
}

static uint64_t read_free_count(void) {
    return perf.counters[PERF_FREE_COUNT].count;
}

static uint64_t read_irq_count(void) {
    return perf.counters[PERF_IRQ_COUNT].count;
}

static uint64_t read_uptime(void) {
    return perf_ticks_to_ns(perf.uptime_ticks) / 1000000000ULL;  /* seconds */
}

static uint64_t read_mem_total(void) {
    uint64_t total, fre, used;
    mem_get_stats(&total, &fre, &used);
    return total;
}

static uint64_t read_mem_used(void) {
    uint64_t total, fre, used;
    mem_get_stats(&total, &fre, &used);
    return used;
}

static uint64_t read_mem_free(void) {
    uint64_t total, fre, used;
    mem_get_stats(&total, &fre, &used);
    return fre;
}

/* ================================================================
 * Built-in entry definitions
 * ================================================================ */
static struct sysctl_entry builtin_entries[] = {
    { "ctx_switches",    read_ctx_switches,    "Context switches since boot" },
    { "syscall_count",   read_syscall_count,   "System calls since boot" },
    { "syscall_latency", read_syscall_latency, "Average syscall latency (ns)" },
    { "page_faults",     read_page_faults,     "Page faults since boot" },
    { "cow_count",       read_cow_count,       "Copy-on-write faults since boot" },
    { "malloc_count",    read_malloc_count,    "kmalloc() calls since boot" },
    { "free_count",      read_free_count,      "kfree() calls since boot" },
    { "irq_count",       read_irq_count,       "Interrupts since boot" },
    { "uptime",          read_uptime,          "System uptime (seconds)" },
    { "mem_total",       read_mem_total,       "Total physical memory (bytes)" },
    { "mem_used",        read_mem_used,        "Used physical memory (bytes)" },
    { "mem_free",        read_mem_free,        "Free physical memory (bytes)" },
};

#define BUILTIN_COUNT (sizeof(builtin_entries) / sizeof(builtin_entries[0]))

/* ================================================================
 * sysctl_register: Register a sysctl entry
 * ================================================================ */
int sysctl_register(struct sysctl_entry *entry) {
    if (!entry || !entry->name || !entry->read) return -1;
    if (entry_count >= SYSCTL_MAX_ENTRIES) return -1;
    entries[entry_count++] = entry;
    return 0;
}

/* ================================================================
 * sysctl_read: Read a statistic by name
 * ================================================================ */
uint64_t sysctl_read(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i]->name, name) == 0) {
            return entries[i]->read();
        }
    }
    return 0; /* not found */
}

/* ================================================================
 * sysctl_list: List all available statistics
 * ================================================================ */
void sysctl_list(void (*callback)(const char *name, uint64_t value, const char *desc)) {
    if (!callback) return;
    for (int i = 0; i < entry_count; i++) {
        uint64_t val = entries[i]->read();
        callback(entries[i]->name, val, entries[i]->desc);
    }
}

/* ================================================================
 * sysctl_init: Register all built-in entries
 * ================================================================ */
void sysctl_init(void) {
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        sysctl_register(&builtin_entries[i]);
    }
    log_printf(LOG_LEVEL_INFO, "sysctl: %d entries registered\n", (int)BUILTIN_COUNT);
}
/*
 * procfs.c - /proc filesystem implementation for AuroraOS
 *
 * Provides virtual files that expose kernel information:
 *   /proc/cpuinfo    - CPU vendor and feature info (CPUID)
 *   /proc/meminfo    - Memory usage statistics
 *   /proc/uptime     - System uptime in seconds
 *   /proc/version    - Kernel version string
 *   /proc/mounts     - List of mounted filesystems
 *   /proc/self/stat  - Current process PID and state
 *   /proc/self/maps  - Current process memory map (CoolPotOS-inspired)
 *   /proc/self/cmdline - Current process command line (CoolPotOS-inspired)
 *   /proc/interrupts - IRQ vector counts per CPU (CoolPotOS-inspired)
 *   /proc/filesystems- Supported filesystem types
 *   /proc/cmdline    - Kernel command line
 *   /proc/kmsg       - Kernel log ring buffer (CoolPotOS-inspired)
 *
 * Each file is backed by a function that generates its content on read.
 * Uses the existing VFS infrastructure (struct file_ops, struct inode,
 * struct super_block, struct dentry).
 */

#include "procfs.h"
#include "fs.h"
#include "vfs.h"
#include "sched.h"
#include "perf.h"
#include "mem.h"
#include "pagetable.h"
#include "cmdline.h"
#include "include/log.h"
#include "include/kstdio.h"
#include <string.h>
#include <stdint.h>

/* ================================================================
 * Procfs inode private data
 * ================================================================ */
#define PROC_INODE_FILE    0
#define PROC_INODE_ROOT    1
#define PROC_INODE_SELF    2

struct procfs_inode_data {
    struct procfs_entry *entry;  /* pointer to the procfs entry */
    int type;                    /* PROC_INODE_* */
};

/* ================================================================
 * Forward declarations
 * ================================================================ */
static struct file_ops procfs_file_ops;
static struct file_ops procfs_dir_ops;
static int procfs_lookup(struct inode *dir, struct dentry *dentry);
static int procfs_self_lookup(struct inode *dir, struct dentry *dentry);
static int read_self_maps(char *buf, size_t size);
static int read_self_stat(char *buf, size_t size);
static int read_self_cmdline(char *buf, size_t size);
static int output_region(char *buf, size_t size, uint64_t start, uint64_t end, int flags);
static int u64toa_hex_append(char *buf, size_t size, uint64_t val);

/* ================================================================
 * Helper: append functions for building strings
 * ================================================================ */

static int append_str(char *buf, size_t size, const char *s) {
    int n = 0;
    for (const char *p = s; *p && n < (int)size - 1; p++, n++)
        buf[n] = *p;
    return n;
}

static int u64toa_append(char *buf, size_t size, uint64_t val) {
    char tmp[21];
    int tn = 0;
    if (val == 0) tmp[tn++] = '0';
    while (val > 0 && tn < 20) { tmp[tn++] = '0' + (int)(val % 10); val /= 10; }
    int n = 0;
    for (int i = tn - 1; i >= 0 && n < (int)size - 1; i--)
        buf[n++] = tmp[i];
    return n;
}

static int itoa_append(char *buf, size_t size, int val) {
    int n = 0;
    if (val < 0) {
        if (n < (int)size - 1) buf[n++] = '-';
        unsigned int uv = (unsigned int)(-(val + 1)) + 1U;
        n += u64toa_append(buf + n, size - (size_t)n, uv);
    } else {
        n += u64toa_append(buf + n, size - (size_t)n, (uint64_t)val);
    }
    return n;
}

/* ================================================================
 * Read functions for each /proc file
 * ================================================================ */

/*
 * read_cpuinfo: Returns CPU vendor string and feature info from CPUID.
 */
static int read_cpuinfo(char *buf, size_t size) {
    if (size < 256) return -1;
    int len = 0;

    /* Read CPUID leaf 0: vendor string in EBX:EDX:ECX */
    uint32_t ebx, ecx, edx;
    uint32_t max_leaf;

    asm volatile ("cpuid"
        : "=a"(max_leaf), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0));

    char vendor[13];
    ((uint32_t *)vendor)[0] = ebx;
    ((uint32_t *)vendor)[1] = edx;
    ((uint32_t *)vendor)[2] = ecx;
    vendor[12] = '\0';

    len += append_str(buf + len, size - (size_t)len, "vendor_id       : ");
    len += append_str(buf + len, size - (size_t)len, vendor);
    len += append_str(buf + len, size - (size_t)len, "\n");

    /* CPUID leaf 1: feature info */
    if (max_leaf >= 1) {
        uint32_t eax;
        asm volatile ("cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(1), "c"(0));

        uint32_t family = (eax >> 8) & 0xF;
        uint32_t model  = (eax >> 4) & 0xF;
        if (family == 0xF) family += (eax >> 20) & 0xFF;
        if (family == 0x6 || family == 0xF) model |= ((eax >> 16) & 0xF) << 4;

        len += append_str(buf + len, size - (size_t)len, "cpu family      : ");
        len += itoa_append(buf + len, size - (size_t)len, (int)family);
        len += append_str(buf + len, size - (size_t)len, "\n");
        len += append_str(buf + len, size - (size_t)len, "model           : ");
        len += itoa_append(buf + len, size - (size_t)len, (int)model);
        len += append_str(buf + len, size - (size_t)len, "\n");

        /* Feature flags */
        len += append_str(buf + len, size - (size_t)len, "flags           :");
        if (edx & (1 << 0))  len += append_str(buf + len, size - (size_t)len, " fpu");
        if (edx & (1 << 4))  len += append_str(buf + len, size - (size_t)len, " tsc");
        if (edx & (1 << 5))  len += append_str(buf + len, size - (size_t)len, " msr");
        if (edx & (1 << 8))  len += append_str(buf + len, size - (size_t)len, " cx8");
        if (edx & (1 << 11)) len += append_str(buf + len, size - (size_t)len, " sep");
        if (edx & (1 << 23)) len += append_str(buf + len, size - (size_t)len, " mmx");
        if (edx & (1 << 25)) len += append_str(buf + len, size - (size_t)len, " sse");
        if (edx & (1 << 26)) len += append_str(buf + len, size - (size_t)len, " sse2");
        if (ecx & (1 << 0))  len += append_str(buf + len, size - (size_t)len, " sse3");
        if (ecx & (1 << 9))  len += append_str(buf + len, size - (size_t)len, " ssse3");
        if (ecx & (1 << 19)) len += append_str(buf + len, size - (size_t)len, " sse4.1");
        if (ecx & (1 << 20)) len += append_str(buf + len, size - (size_t)len, " sse4.2");
        if (ecx & (1 << 28)) len += append_str(buf + len, size - (size_t)len, " avx");
        if (edx & (1 << 28)) len += append_str(buf + len, size - (size_t)len, " htt");
        if (edx & (1 << 29)) len += append_str(buf + len, size - (size_t)len, " tm");
        if (ecx & (1 << 30)) len += append_str(buf + len, size - (size_t)len, " rdrand");
        len += append_str(buf + len, size - (size_t)len, "\n");
    }

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_meminfo: Returns memory usage statistics.
 */
static int read_meminfo(char *buf, size_t size) {
    if (size < 256) return -1;
    uint64_t total, fre, used;
    mem_get_stats(&total, &fre, &used);

    int len = 0;
    len += append_str(buf + len, size - (size_t)len, "MemTotal:        ");
    len += u64toa_append(buf + len, size - (size_t)len, total / 1024);
    len += append_str(buf + len, size - (size_t)len, " kB\n");
    len += append_str(buf + len, size - (size_t)len, "MemFree:         ");
    len += u64toa_append(buf + len, size - (size_t)len, fre / 1024);
    len += append_str(buf + len, size - (size_t)len, " kB\n");
    len += append_str(buf + len, size - (size_t)len, "MemUsed:         ");
    len += u64toa_append(buf + len, size - (size_t)len, used / 1024);
    len += append_str(buf + len, size - (size_t)len, " kB\n");

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_uptime: Returns system uptime in seconds.
 */
static int read_uptime(char *buf, size_t size) {
    if (size < 32) return -1;
    uint64_t ticks = perf_get_ticks();
    uint64_t ns = perf_ticks_to_ns(ticks);
    uint64_t sec = ns / 1000000000ULL;

    int len = 0;
    len += u64toa_append(buf + len, size - (size_t)len, sec);
    len += append_str(buf + len, size - (size_t)len, ".00\n");

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_version: Returns kernel version string.
 */
static int read_version(char *buf, size_t size) {
    const char *version = "AuroraOS v3.0.2\n";
    int len = 0;
    for (const char *p = version; *p && len < (int)size - 1; p++)
        buf[len++] = *p;
    buf[len] = '\0';
    return len;
}

/*
 * read_mounts: Returns list of mounted filesystems.
 */
static int read_mounts(char *buf, size_t size) {
    if (size < 256) return -1;
    int len = 0;

    /* Get the root filesystem */
    struct super_block *root_sb = vfs_get_root_sb();
    if (root_sb) {
        len += append_str(buf + len, size - (size_t)len, root_sb->fs_name);
        len += append_str(buf + len, size - (size_t)len, " / ");
        len += append_str(buf + len, size - (size_t)len, root_sb->fs_name);
        len += append_str(buf + len, size - (size_t)len, " rw 0 0\n");
    }

    /* procfs itself */
    len += append_str(buf + len, size - (size_t)len, "proc /proc proc rw 0 0\n");

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_self_cmdline: Returns the command line of the current process.
 * Inspired by CoolPotOS's /proc/<pid>/cmdline.
 */
static int read_self_cmdline(char *buf, size_t size) {
    if (size < 64) return -1;
    if (!current) return -1;

    int len = 0;
    /* Use the process name as the command line for now.
     * In a full implementation, this would return the argv array
     * passed to execve(). */
    for (char *p = current->name; *p && len < (int)size - 1; p++)
        buf[len++] = *p;
    if (len < (int)size - 1) buf[len++] = '\n';

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_self_maps: Returns the memory map of the current process.
 * Walks the page table to find all mapped regions and their permissions.
 * Inspired by CoolPotOS's /proc/<pid>/maps.
 */
static int read_self_maps(char *buf, size_t size) {
    if (size < 512) return -1;
    if (!current) return -1;

    int len = 0;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)current->cr3;

    /* Walk the page table to find contiguous mapped regions */
    uint64_t region_start = 0;
    uint64_t region_end = 0;
    int region_flags = -1;
    int in_region = 0;

    for (uint64_t va = 0; va < 0x0000800000000000ULL; va += PAGE_SIZE) {
        /* Skip kernel space */
        if (va >= 0x0000800000000000ULL) break;

        uint64_t pml4_idx = (va >> 39) & 0x1FF;
        if (!(pml4[pml4_idx] & PTE_PRESENT)) {
            /* Skip the entire 512GB region */
            va += (1ULL << 39) - PAGE_SIZE;
            continue;
        }

        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & PTE_ADDR_MASK);
        uint64_t pdpt_idx = (va >> 30) & 0x1FF;
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
            va += (1ULL << 30) - PAGE_SIZE;
            continue;
        }

        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        uint64_t pd_idx = (va >> 21) & 0x1FF;
        if (!(pd[pd_idx] & PTE_PRESENT)) {
            va += (1ULL << 21) - PAGE_SIZE;
            continue;
        }

        /* Check if this is a huge page (2MB) */
        if (pd[pd_idx] & PTE_PS) {
            uint64_t entry = pd[pd_idx];
            /* This is a 2MB huge page */
            if (entry & PTE_PRESENT) {
                int flags = 4; /* r-- */
                if (entry & PTE_RW) flags |= 2; /* rw- */
                if (!(entry & PTE_NX)) flags |= 1; /* rwx */

                if (in_region && flags == region_flags &&
                    va == region_end) {
                    region_end = va + (1ULL << 21);
                } else {
                    if (in_region) {
                        /* Output previous region */
                        len += output_region(buf + len, size - (size_t)len,
                            region_start, region_end, region_flags);
                        if (len >= (int)size - 1) goto done;
                    }
                    region_start = va;
                    region_end = va + (1ULL << 21);
                    region_flags = flags;
                    in_region = 1;
                }
            }
            va += (1ULL << 21) - PAGE_SIZE;
            continue;
        }

        uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & PTE_ADDR_MASK);
        uint64_t pt_idx = (va >> 12) & 0x1FF;

        if (pt[pt_idx] & PTE_PRESENT) {
            uint64_t entry = pt[pt_idx];
            int flags = 4; /* r-- */
            if (entry & PTE_RW) flags |= 2; /* rw- */
            if (!(entry & PTE_NX)) flags |= 1; /* rwx */

            if (in_region && flags == region_flags &&
                va == region_end) {
                /* Extend current region */
                region_end = va + PAGE_SIZE;
            } else {
                if (in_region) {
                    /* Output previous region */
                    len += output_region(buf + len, size - (size_t)len,
                        region_start, region_end, region_flags);
                    if (len >= (int)size - 1) goto done;
                }
                region_start = va;
                region_end = va + PAGE_SIZE;
                region_flags = flags;
                in_region = 1;
            }
        }
    }

    /* Output the last region */
    if (in_region) {
        len += output_region(buf + len, size - (size_t)len,
            region_start, region_end, region_flags);
    }

done:
    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * Helper: Output a single memory region line in /proc/self/maps format.
 * Format: "start-end flags offset dev inode path"
 */
static int output_region(char *buf, size_t size,
                         uint64_t start, uint64_t end, int flags) {
    if (size < 96) return 0;
    int len = 0;

    /* Start address */
    len += u64toa_hex_append(buf + len, size - (size_t)len, start);
    len += append_str(buf + len, size - (size_t)len, "-");
    /* End address */
    len += u64toa_hex_append(buf + len, size - (size_t)len, end);
    len += append_str(buf + len, size - (size_t)len, " ");

    /* Permissions: rwxp */
    buf[len++] = (flags & 4) ? 'r' : '-';
    buf[len++] = (flags & 2) ? 'w' : '-';
    buf[len++] = (flags & 1) ? 'x' : '-';
    buf[len++] = 'p';
    len += append_str(buf + len, size - (size_t)len, " 00000000 00:00 0");

    /* Path hint */
    if (start >= 0x7FFF00000000ULL) {
        len += append_str(buf + len, size - (size_t)len, "          [stack]");
    } else if (start >= 0x60000000ULL && start < 0x70000000ULL) {
        len += append_str(buf + len, size - (size_t)len, "          [heap]");
    } else if (start < 0x200000ULL) {
        len += append_str(buf + len, size - (size_t)len, "          [text]");
    }

    if (len < (int)size - 1) buf[len++] = '\n';
    return len;
}

/*
 * Helper: Convert uint64_t to hex string and append to buffer.
 */
static int u64toa_hex_append(char *buf, size_t size, uint64_t val) {
    char tmp[17];
    int tn = 0;
    if (val == 0) tmp[tn++] = '0';
    while (val > 0 && tn < 16) {
        int d = (int)(val & 0xF);
        tmp[tn++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        val >>= 4;
    }
    /* Pad to 16 hex digits */
    int n = 0;
    while (tn < 16 && n < (int)size - 1) { buf[n++] = '0'; tn++; }
    for (int i = tn - 1; i >= 0 && n < (int)size - 1; i--)
        buf[n++] = tmp[i];
    return n;
}

/*
 * read_self_stat: Returns current process PID and state.
 */
static int read_self_stat(char *buf, size_t size) {
    if (size < 128) return -1;
    int len = 0;

    if (current) {
        len += itoa_append(buf + len, size - (size_t)len, current->pid);
        len += append_str(buf + len, size - (size_t)len, " (");
        /* Process name */
        for (char *p = current->name; *p && len < (int)size - 1; p++)
            buf[len++] = *p;
        len += append_str(buf + len, size - (size_t)len, ") ");

        /* State */
        char state_char = 'R';
        switch (current->state) {
            case TASK_RUNNING: state_char = 'R'; break;
            case TASK_READY:   state_char = 'R'; break;
            case TASK_BLOCKED: state_char = 'S'; break;
            case TASK_ZOMBIE:  state_char = 'Z'; break;
            case TASK_DEAD:    state_char = 'X'; break;
        }
        if (len < (int)size - 1) buf[len++] = state_char;
        if (len < (int)size - 1) buf[len++] = '\n';
    }

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_interrupts: Returns IRQ counts per CPU (CoolPotOS-inspired).
 */
static int read_interrupts(char *buf, size_t size) {
    if (size < 512) return -1;
    int len = 0;

    len += append_str(buf + len, size - (size_t)len, "           CPU0       \n");

    for (int i = 0; i < 256; i++) {
        if (perf.irq_counts[i].count > 0 || perf.irq_counts[i].name) {
            /* Vector number */
            len += append_str(buf + len, size - (size_t)len, "  ");
            len += itoa_append(buf + len, size - (size_t)len, i);
            len += append_str(buf + len, size - (size_t)len, ": ");

            /* IRQ name */
            const char *irq_name = perf.irq_counts[i].name;
            if (irq_name) {
                len += append_str(buf + len, size - (size_t)len, irq_name);
            } else {
                len += append_str(buf + len, size - (size_t)len, "unknown");
            }

            /* Padding */
            int name_len = 0;
            if (irq_name) for (const char *p = irq_name; *p; p++) name_len++;
            else name_len = 7;
            for (int j = name_len; j < 16; j++)
                if (len < (int)size - 1) buf[len++] = ' ';

            /* Count */
            len += u64toa_append(buf + len, size - (size_t)len, perf.irq_counts[i].count);
            if (len < (int)size - 1) buf[len++] = '\n';
        }
    }

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_filesystems: Returns supported filesystem types.
 */
static int read_filesystems(char *buf, size_t size) {
    if (size < 128) return -1;
    int len = 0;

    len += append_str(buf + len, size - (size_t)len, "nodev   proc\n");
    len += append_str(buf + len, size - (size_t)len, "        ramfs\n");
    len += append_str(buf + len, size - (size_t)len, "        ext2\n");

    if (len >= (int)size) len = (int)size - 1;
    buf[len] = '\0';
    return len;
}

/*
 * read_cmdline: Returns kernel command line (CoolPotOS-inspired).
 */
static int read_cmdline(char *buf, size_t size) {
    if (size < 64) return -1;
    const char *cmdline = cmdline_get();
    int len = 0;
    for (const char *p = cmdline; *p && len < (int)size - 1; p++)
        buf[len++] = *p;
    if (len < (int)size - 1) buf[len++] = '\n';
    buf[len] = '\0';
    return len;
}

/*
 * read_kmsg: Returns kernel log ring buffer (CoolPotOS-inspired).
 */
static int read_kmsg(char *buf, size_t size) {
    if (size < 256) return -1;
    return log_ring_read(buf, size);
}

/* ================================================================
 * Procfs entry table
 * ================================================================ */
static struct procfs_entry procfs_entries[] = {
    { "cpuinfo",     read_cpuinfo,     0 },
    { "meminfo",     read_meminfo,     0 },
    { "uptime",      read_uptime,      0 },
    { "version",     read_version,     0 },
    { "mounts",      read_mounts,      0 },
    { "interrupts",  read_interrupts,  0 },
    { "filesystems", read_filesystems, 0 },
    { "cmdline",     read_cmdline,     0 },
    { "kmsg",        read_kmsg,        0 },
    { "self",        NULL,             1 },  /* directory for /proc/self */
    { NULL,          NULL,             0 },  /* sentinel */
};

/* ================================================================
 * Procfs file operations
 * ================================================================ */

static int procfs_open(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

static ssize_t procfs_read(struct file *filp, void *buf, size_t count,
                           off_t *offset) {
    if (!filp || !filp->inode || !buf) return -1;

    struct procfs_inode_data *data =
        (struct procfs_inode_data *)filp->inode->priv;
    if (!data) return -1;

    /* Only support reading from the beginning */
    if (*offset > 0) return 0;

    /* Generate content into a temporary buffer, then copy to user buffer */
    char tmp[1024];
    int len = 0;

    if (data->type == PROC_INODE_FILE && data->entry && data->entry->read_func) {
        len = data->entry->read_func(tmp, sizeof(tmp));
    } else if (data->type == PROC_INODE_SELF) {
        /* /proc/self/<name> */
        if (filp->inode->name && strcmp(filp->inode->name, "maps") == 0) {
            len = read_self_maps(tmp, sizeof(tmp));
        } else if (filp->inode->name && strcmp(filp->inode->name, "cmdline") == 0) {
            len = read_self_cmdline(tmp, sizeof(tmp));
        } else {
            /* Default: /proc/self/stat */
            len = read_self_stat(tmp, sizeof(tmp));
        }
    } else {
        return 0;
    }

    if (len < 0) return -1;
    if (len == 0) return 0;

    size_t to_copy = (size_t)len;
    if (to_copy > count) to_copy = count;
    memcpy(buf, tmp, to_copy);
    *offset += (off_t)to_copy;

    return (ssize_t)to_copy;
}

static int procfs_close(struct inode *inode, struct file *filp) {
    (void)inode; (void)filp;
    return 0;
}

/* ================================================================
 * File operations tables (must be defined before lookup functions)
 * ================================================================ */

static struct file_ops procfs_file_ops = {
    .open   = procfs_open,
    .read   = procfs_read,
    .write  = NULL,
    .close  = procfs_close,
    .lookup = NULL,
};

static struct file_ops procfs_dir_ops = {
    .open   = procfs_open,
    .read   = NULL,
    .write  = NULL,
    .close  = procfs_close,
    .lookup = procfs_lookup,
};

/* ================================================================
 * lookup: Resolve a name within the procfs root directory
 * ================================================================ */

static int procfs_lookup(struct inode *dir, struct dentry *dentry) {
    if (!dir || !dentry || !dentry->name) return -1;

    struct procfs_inode_data *dir_data =
        (struct procfs_inode_data *)dir->priv;

    /* If we're in the "self" directory, use its own lookup */
    if (dir_data && dir_data->type == PROC_INODE_SELF) {
        return procfs_self_lookup(dir, dentry);
    }

    /* Search the procfs entry table */
    for (int i = 0; procfs_entries[i].name != NULL; i++) {
        struct procfs_entry *e = &procfs_entries[i];
        if (strcmp(e->name, dentry->name) == 0) {
            /* Create an inode for this entry */
            struct inode *inode = (struct inode *)kmalloc(sizeof(*inode));
            if (!inode) return -1;
            memset(inode, 0, sizeof(*inode));

            struct procfs_inode_data *data =
                (struct procfs_inode_data *)kmalloc(sizeof(*data));
            if (!data) { kfree(inode); return -1; }
            memset(data, 0, sizeof(*data));

            data->entry = e;
            if (e->is_dir) {
                /* Self directory */
                data->type = PROC_INODE_SELF;
                inode->is_dir = 1;
                inode->ops = &procfs_dir_ops;
            } else {
                /* Regular file */
                data->type = PROC_INODE_FILE;
                inode->is_dir = 0;
                inode->ops = &procfs_file_ops;
            }
            inode->name = e->name;
            inode->priv = data;
            inode->dentry = dentry;
            dentry->inode = inode;
            return 0;
        }
    }

    /* Not found in procfs entries */
    return -1;
}

/*
 * procfs_self_lookup: Resolve entries within /proc/self
 */
static int procfs_self_lookup(struct inode *dir, struct dentry *dentry) {
    (void)dir;
    if (!dentry || !dentry->name) return -1;

    /* Helper to create a virtual inode for /proc/self/<name> */
    #define CREATE_SELF_INODE(fname) do { \
        struct inode *inode = (struct inode *)kmalloc(sizeof(*inode)); \
        if (!inode) return -1; \
        memset(inode, 0, sizeof(*inode)); \
        struct procfs_inode_data *data = \
            (struct procfs_inode_data *)kmalloc(sizeof(*data)); \
        if (!data) { kfree(inode); return -1; } \
        memset(data, 0, sizeof(*data)); \
        data->type = PROC_INODE_SELF; \
        data->entry = NULL; \
        inode->name = fname; \
        inode->priv = data; \
        inode->is_dir = 0; \
        inode->ops = &procfs_file_ops; \
        inode->dentry = dentry; \
        dentry->inode = inode; \
        return 0; \
    } while (0)

    if (strcmp(dentry->name, "stat") == 0) {
        CREATE_SELF_INODE("stat");
    }

    if (strcmp(dentry->name, "maps") == 0) {
        CREATE_SELF_INODE("maps");
    }

    if (strcmp(dentry->name, "cmdline") == 0) {
        CREATE_SELF_INODE("cmdline");
    }

    #undef CREATE_SELF_INODE
    return -1;
}

/* ================================================================
 * procfs_create: Create the procfs filesystem super block
 * ================================================================ */

struct super_block *procfs_create(void) {
    /* Create the root inode for procfs */
    struct inode *root_inode = (struct inode *)kmalloc(sizeof(*root_inode));
    if (!root_inode) return NULL;
    memset(root_inode, 0, sizeof(*root_inode));

    struct procfs_inode_data *root_data =
        (struct procfs_inode_data *)kmalloc(sizeof(*root_data));
    if (!root_data) { kfree(root_inode); return NULL; }
    memset(root_data, 0, sizeof(*root_data));

    root_data->type = PROC_INODE_ROOT;
    root_data->entry = NULL;

    root_inode->name = "";
    root_inode->priv = root_data;
    root_inode->is_dir = 1;
    root_inode->ops = &procfs_dir_ops;

    /* Create the super block */
    struct super_block *sb = (struct super_block *)kmalloc(sizeof(*sb));
    if (!sb) { kfree(root_data); kfree(root_inode); return NULL; }
    memset(sb, 0, sizeof(*sb));

    sb->fs_name = "proc";
    sb->root = root_inode;
    sb->sb_data = NULL;

    return sb;
}

/* ================================================================
 * procfs_init: Create procfs and mount it at /proc
 * ================================================================ */

void procfs_init(void) {
    struct super_block *proc_sb = procfs_create();
    if (!proc_sb) {
        log_printf(LOG_LEVEL_ERR, "procfs: failed to create super block\n");
        return;
    }

    if (vfs_mount("/proc", proc_sb) < 0) {
        log_printf(LOG_LEVEL_ERR, "procfs: failed to mount at /proc\n");
        return;
    }

    log_printf(LOG_LEVEL_INFO, "procfs: mounted at /proc\n");
}
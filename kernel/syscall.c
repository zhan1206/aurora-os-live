/*
 * syscall.c - System call dispatcher (Phase 1: complete I/O syscalls)
 */
#include "syscall.h"
#include "include/log.h"
#include "include/idt.h"
#include "include/userspace.h"
#include "include/trapframe.h"
#include "include/errno.h"
#include "sched.h"
#include "signal.h"
#include "vfs.h"
#include "fs.h"
#include "pagetable.h"
#include "mem.h"
#include "capability.h"
#include "perf.h"
#include "seccomp.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Trapframe pointer for signal delivery */
struct trapframe *current_tf = NULL;

/* ================================================================
 * I/O syscalls
 * ================================================================ */

/* Simple stat structure for fstat syscall */
struct kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
};

static long sys_read(int fd, void *buf, size_t count) {
    if (!buf || count == 0) return 0;
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!user_addr_range_ok(buf, count)) { current->t_errno = EFAULT; return -1; }

    /* fd 0 (stdin): read from console line buffer */
    if (fd == 0) {
        extern int console_getline(char *buf, size_t buflen);
        extern void console_write(const char *s);
        extern void yield(void);

        /* Block until a line is available */
        int len;
        while ((len = console_getline((char *)buf, count)) == 0) {
            current->state = TASK_BLOCKED;
            schedule();
            /* Check for pending signals */
            if (current->sig && current->sig->pending) { current->t_errno = EINTR; return -1; }
        }
        return len;
    }

    /* Other fds: use fd_table */
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }
    return vfs_read(filp, buf, count);
}

static long sys_write(int fd, const void *buf, size_t count) {
    if (!buf || count == 0) return 0;
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!user_addr_range_ok(buf, count)) { current->t_errno = EFAULT; return -1; }

    /* fd 1/2 (stdout/stderr): batch output to VGA + serial */
    if (fd == 1 || fd == 2) {
        extern void printk(const char *str);
        /*
         * Batch output: copy up to 256 chars at a time to a kernel
         * buffer and call printk once, instead of per-character.
         * Uses memcpy for efficient bulk copy.
         */
        size_t remaining = count;
        const char *s = (const char *)buf;
        while (remaining > 0) {
            size_t chunk = (remaining > 256) ? 256 : remaining;
            char tmp[257];
            if (copy_from_user(tmp, s, chunk) != 0) {
                return -1;
            }
            tmp[chunk] = '\0';
            printk(tmp);
            s += chunk;
            remaining -= chunk;
        }
        return (long)count;
    }

    /* Other fds: use fd_table */
    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }
    return vfs_write(filp, buf, count);
}

static long sys_open(const char *path, int flags) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';  /* ensure null termination */

    struct file *filp = vfs_open(kpath, flags);
    if (!filp) { current->t_errno = ENOENT; return -1; }

    int fd = fd_alloc(current, filp);
    if (fd < 0) {
        vfs_close(filp);
        current->t_errno = ENFILE;
        return -1;
    }
    return fd;
}

static long sys_close(int fd) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    int ret = fd_close(current, fd);
    if (ret < 0) current->t_errno = EBADF;
    return ret;
}

static long sys_dup(int oldfd) {
    if (oldfd < 0 || oldfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    int ret = fd_dup(current, oldfd);
    if (ret < 0) current->t_errno = EBADF;
    return ret;
}

static long sys_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (newfd < 0 || newfd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    int ret = fd_dup2(current, oldfd, newfd);
    if (ret < 0) current->t_errno = EBADF;
    return ret;
}

static long sys_getdents(int fd, void *dirp, size_t count) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!dirp || count == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(dirp, count)) { current->t_errno = EFAULT; return -1; }

    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }
    if (!filp->inode || !filp->inode->ops || !filp->inode->ops->read) {
        current->t_errno = ENOTDIR;
        return -1;
    }

    /* For now, use read to get directory listing */
    return vfs_read(filp, dirp, count);
}

/* ================================================================
 * lseek — reposition file offset
 * ================================================================ */
static long sys_lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }

    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    /* Determine file size from the inode's private data if available */
    uint64_t file_size = 0;
    if (filp->inode && filp->inode->priv) {
        /* ramfs_node: size is at offset sizeof(struct inode), but we can't
         * safely dereference it without knowing the type.  As a safe default,
         * allow seeking to any non-negative offset. */
        file_size = UINT64_MAX; /* allow any seek for ramfs */
    }

    off_t new_offset;
    switch (whence) {
        case 0: /* SEEK_SET */ new_offset = offset; break;
        case 1: /* SEEK_CUR */ new_offset = filp->offset + offset; break;
        case 2: /* SEEK_END */
            if (file_size == UINT64_MAX) {
                /* SEEK_END not supported for unknown file sizes */
                current->t_errno = EINVAL; return -1;
            }
            new_offset = (off_t)file_size + offset;
            break;
        default: current->t_errno = EINVAL; return -1;
    }
    if (new_offset < 0) { current->t_errno = EINVAL; return -1; }

    filp->offset = new_offset;
    return (long)new_offset;
}

/* ================================================================
 * fstat — get file status
 * ================================================================ */
static long sys_fstat(int fd, struct kstat *statbuf) {
    if (fd < 0 || fd >= MAX_FDS) { current->t_errno = EBADF; return -1; }
    if (!statbuf || !user_addr_range_ok(statbuf, sizeof(struct kstat))) {
        current->t_errno = EFAULT; return -1;
    }

    struct file *filp = (struct file *)fd_get(current, fd);
    if (!filp) { current->t_errno = EBADF; return -1; }

    struct kstat ks;
    memset(&ks, 0, sizeof(ks));
    ks.st_dev = 0;
    ks.st_ino = (uint64_t)(uintptr_t)filp->inode;
    ks.st_mode = filp->inode && filp->inode->is_dir ? 0040755 : 0100755;
    ks.st_nlink = 1;
    ks.st_uid = 0;
    ks.st_gid = 0;
    ks.st_size = 0; /* size not tracked in generic inode yet */
    ks.st_blksize = 4096;
    ks.st_blocks = 0;

    if (copy_to_user(statbuf, &ks, sizeof(ks)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * mmap — map memory region (basic anonymous mapping)
 *
 * Allocates physical pages and maps them into the process address space
 * starting at a fixed region (0x60000000). Each page is zero-initialized.
 *
 * LIMITATION: Uses a fixed mapping region at 0x60000000. Consecutive
 * mmap calls will overwrite previous mappings. A proper implementation
 * would track allocated regions and find free address space.
 *
 * PROT flags: PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4
 * MAP_ANONYMOUS=0x20 (only anonymous mapping is supported in this phase)
 * ================================================================ */
static long sys_mmap(void *addr, size_t length, int prot, int flags,
                     int fd, off_t offset) {
    (void)addr; (void)fd; (void)offset;

    /* Anonymous mapping only in this phase */
    if (!(flags & 0x20)) { /* MAP_ANONYMOUS = 0x20 */
        current->t_errno = ENOSYS;
        return -1;
    }
    if (length == 0) { current->t_errno = EINVAL; return -1; }

    /* Align length to page size */
    size_t num_pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t map_va = 0x60000000ULL; /* Fixed mapping region for now */

    uint64_t pte_flags = PTE_USER;
    if (prot & 2) pte_flags |= PTE_RW;      /* PROT_WRITE */
    if (!(prot & 4)) pte_flags |= PTE_NX;    /* PROT_EXEC */

    for (size_t i = 0; i < num_pages; i++) {
        void *phys = alloc_page();
        if (!phys) {
            /* Cleanup previously mapped pages on allocation failure */
            for (size_t j = 0; j < i; j++) {
                uint64_t va = map_va + j * PAGE_SIZE;
                unmap_page(current->cr3, va);
                /* Note: physical page cannot be easily recovered after unmap
                 * without tracking it. This is a known limitation of the
                 * fixed-region approach. */
            }
            current->t_errno = ENOMEM;
            return -1;
        }
        memset(phys, 0, PAGE_SIZE);
        if (map_page(current->cr3, map_va + i * PAGE_SIZE,
                     (uint64_t)(uintptr_t)phys, pte_flags) != 0) {
            free_page(phys);
            /* Cleanup previously mapped pages on map failure */
            for (size_t j = 0; j < i; j++) {
                uint64_t va = map_va + j * PAGE_SIZE;
                unmap_page(current->cr3, va);
            }
            current->t_errno = ENOMEM;
            return -1;
        }
    }

    return (long)map_va;
}

/* ================================================================
 * mprotect — change memory protection
 *
 * Walks the 4-level page table (PML4→PDPT→PD→PT) for each page in the
 * given range. Updates the PTE protection flags while preserving the
 * physical address. Skips non-present pages and huge (2MB) pages.
 *
 * Uses INVLPG to flush stale TLB entries for each modified page.
 * ================================================================ */
static long sys_mprotect(void *addr, size_t length, int prot) {
    if (!addr || length == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(addr, length)) { current->t_errno = EFAULT; return -1; }

    uint64_t va = (uint64_t)(uintptr_t)addr;
    uint64_t va_end = va + length;
    uint64_t va_page = va & ~0xFFFULL;

    uint64_t new_flags = PTE_USER | PTE_PRESENT;
    if (prot & 2) new_flags |= PTE_RW;      /* PROT_WRITE */
    if (!(prot & 4)) new_flags |= PTE_NX;    /* PROT_EXEC */

    /* Walk page table for each page in the range */
    while (va_page < va_end) {
        uint64_t pml4_idx = (va_page >> 39) & 0x1FF;
        uint64_t pdpt_idx = (va_page >> 30) & 0x1FF;
        uint64_t pd_idx   = (va_page >> 21) & 0x1FF;
        uint64_t pt_idx   = (va_page >> 12) & 0x1FF;

        uint64_t *pml4 = (uint64_t *)phys_to_virt(current->cr3);
        if (!(pml4[pml4_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }
        if (pd[pd_idx] & PTE_PS) { va_page += PAGE_SIZE; continue; } /* skip huge pages */
        uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & PTE_ADDR_MASK);
        if (!(pt[pt_idx] & PTE_PRESENT)) { va_page += PAGE_SIZE; continue; }

        /* Update protection flags, preserving the physical address */
        pt[pt_idx] = (pt[pt_idx] & PTE_ADDR_MASK) | new_flags;
        asm volatile ("invlpg (%0)" :: "r"(va_page) : "memory");
        va_page += PAGE_SIZE;
    }
    return 0;
}

/* ================================================================
 * Process syscalls
 * ================================================================ */

static long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    (void)argv; (void)envp;
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';
    int ret = exec_elf(kpath);
    if (ret < 0) current->t_errno = ENOENT;
    return ret;
}

static long sys_getpid(void) {
    if (!current) { current->t_errno = ESRCH; return -1; }
    return current->pid;
}

static long sys_exit(int code) {
    do_exit_current(code);
    return 0;
}

static long sys_waitpid(int pid, int *status, int options) {
    int kstatus = 0;
    int ret = waitpid(pid, &kstatus, options);
    if (ret < 0) { current->t_errno = ECHILD; return -1; }
    if (status && copy_to_user(status, &kstatus, sizeof(int)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return ret;
}

/* ================================================================
 * Signal + pipe wrappers
 * ================================================================ */

static long wrap_sys_kill(int pid, int sig) {
    return do_sys_kill(pid, sig);
}

static long wrap_sys_sigaction(int signo, const struct sigaction *act,
                               struct sigaction *oldact) {
    return do_sys_sigaction(signo, act, oldact);
}

static long wrap_sys_sigreturn(void) {
    do_sys_sigreturn();
    return 0;
}

static long wrap_sys_pipe(int *fds) {
    return sys_pipe(fds);
}

/*
 * sys_fork: Clone current process using COW page tables.
 * Returns child PID to parent, 0 to child.
 *
 * The child task's is_fork_child flag is set.  When the child is first
 * scheduled, it returns through syscall_return_point which pops the
 * saved registers and executes swapgs+sysretq back to user space with
 * RAX=0 (forced by syscall_trap).
 *
 * The child's kernel stack is pre-populated with a copy of the parent's
 * trapframe, so the child resumes at the same user-space instruction
 * (right after the fork syscall) with the same register state.
 */
long sys_fork(void) {
    uint64_t child_cr3 = clone_current_pml4();
    if (child_cr3 == get_kernel_cr3()) { current->t_errno = ENOMEM; return -1; }

    /* Create child task that starts from the fork return point */
    struct task_struct *child = create_task(NULL);
    if (!child) {
        extern void free_pagetable(uint64_t pml4_phys);
        free_pagetable(child_cr3);
        current->t_errno = ENOMEM; return -1;
    }

    child->cr3 = child_cr3;
    child->is_fork_child = 1;   /* child will return 0 */

    /*
     * Copy the parent's trapframe into the child's kernel stack so the
     * child resumes user-space execution at the same point with the
     * same register state (except RAX, which will be 0).
     *
     * The child's stack layout (from create_task):
     *   sp → [13 regs (trapframe)] [6 callee-saved] [ret=syscall_return_point]
     *
     * The 13 regs are at sp[0..12] in syscall_entry push order:
     *   r15,r14,r13,r12,r11,r10,r9,r8,rsi,rdi,rdx,rcx,rax
     */
    if (current_tf) {
        uint64_t *child_sp = (uint64_t *)child->rsp;
        child_sp[0]  = current_tf->r15;
        child_sp[1]  = current_tf->r14;
        child_sp[2]  = current_tf->r13;
        child_sp[3]  = current_tf->r12;
        child_sp[4]  = current_tf->r11;
        child_sp[5]  = current_tf->r10;
        child_sp[6]  = current_tf->r9;
        child_sp[7]  = current_tf->r8;
        child_sp[8]  = current_tf->rsi;
        child_sp[9]  = current_tf->rdi;
        child_sp[10] = current_tf->rdx;
        child_sp[11] = current_tf->rcx;   /* user RIP */
        child_sp[12] = 0;                  /* child returns 0 (but also set in syscall_trap) */
    }

    /* Child inherits parent's fd table, with file refcounts incremented */
    for (int i = 0; i < MAX_FDS; i++) {
        child->fd_table[i] = current->fd_table[i];
        if (child->fd_table[i] != (uintptr_t)-1) {
            vfs_file_dup((struct file *)child->fd_table[i]);
        }
    }

    /* Child inherits parent's signal handlers */
    if (current->sig) {
        child->sig = signal_alloc();
        if (child->sig) {
            memcpy(child->sig->actions, current->sig->actions, sizeof(current->sig->actions));
        }
    }

    return child->pid;  /* parent gets child PID */
}

/* ================================================================
 * SYS_UNAME — Get system name and information
 * ================================================================ */
static long sys_uname(struct utsname *buf) {
    if (!buf || !user_addr_range_ok(buf, sizeof(struct utsname))) {
        current->t_errno = EFAULT; return -1;
    }
    struct utsname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "AuroraOS");
    strcpy(u.nodename, "aurora");
    strcpy(u.release, "3.2.0");
    strcpy(u.version, "#1 SMP 2026-06-20");
    strcpy(u.machine, "x86_64");
    if (copy_to_user(buf, &u, sizeof(u)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_TIMES — Get process times (dummy values)
 * ================================================================ */
static long sys_times(struct tms *buf) {
    if (!buf || !user_addr_range_ok(buf, sizeof(struct tms))) {
        current->t_errno = EFAULT; return -1;
    }
    struct tms t;
    t.tms_utime = 0;
    t.tms_stime = 0;
    t.tms_cutime = 0;
    t.tms_cstime = 0;
    if (copy_to_user(buf, &t, sizeof(t)) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return 0;
}

/* ================================================================
 * SYS_GETCWD — Get current working directory
 * ================================================================ */
static long sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) { current->t_errno = EINVAL; return -1; }
    if (!user_addr_range_ok(buf, size)) { current->t_errno = EFAULT; return -1; }
    size_t len = strlen(current->cwd);
    if (len + 1 > size) { current->t_errno = ERANGE; return -1; }
    if (copy_to_user(buf, current->cwd, len + 1) != 0) {
        current->t_errno = EFAULT; return -1;
    }
    return (long)len;
}

/* ================================================================
 * SYS_CHDIR — Change current working directory
 * ================================================================ */
static long sys_chdir(const char *path) {
    if (!path) { current->t_errno = EFAULT; return -1; }
    char kpath[256];
    int len = strncpy_from_user(kpath, path, sizeof(kpath) - 1);
    if (len < 0) { current->t_errno = EFAULT; return -1; }
    kpath[len] = '\0';
    if (len > 255) { current->t_errno = ENAMETOOLONG; return -1; }
    /* Simple: just store the path as-is (no path resolution yet) */
    strcpy(current->cwd, kpath);
    return 0;
}

/* ================================================================
 * Dispatcher
 * ================================================================ */

/* Maximum valid syscall number */
#define SYS_MAX_NUM  128

long handle_syscall(int num, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6) {
    /* Bounds check: reject invalid syscall numbers */
    if (num < 0 || num >= SYS_MAX_NUM) {
        current->t_errno = ENOSYS;
        return -1;
    }

    /* Performance counter: count syscalls */
    perf_inc(PERF_SYSCALL_COUNT);

    /* Seccomp filter check: deny if blocked by task's filter */
    if (seccomp_check(current, num) != 0) {
        current->t_errno = EPERM;
        return -1;
    }

    long ret = -1;
    switch (num) {
        case SYS_READ:    ret = sys_read((int)a1, (void *)a2, (size_t)a3); break;
        case SYS_WRITE:   ret = sys_write((int)a1, (const void *)a2, (size_t)a3); break;
        case SYS_OPEN:    ret = sys_open((const char *)a1, (int)a2); break;
        case SYS_CLOSE:   ret = sys_close((int)a1); break;
        case SYS_DUP:     ret = sys_dup((int)a1); break;
        case SYS_DUP2:    ret = sys_dup2((int)a1, (int)a2); break;
        case SYS_GETDENTS:ret = sys_getdents((int)a1, (void *)a2, (size_t)a3); break;
        case SYS_LSEEK:   ret = sys_lseek((int)a1, (off_t)a2, (int)a3); break;
        case SYS_FSTAT:   ret = sys_fstat((int)a1, (struct kstat *)a2); break;
        case SYS_MMAP:    ret = sys_mmap((void *)a1, (size_t)a2, (int)a3,
                                         (int)a4, (int)a5, (off_t)a6); break;
        case SYS_MPROTECT: ret = sys_mprotect((void *)a1, (size_t)a2, (int)a3); break;
        case SYS_EXECVE:  ret = sys_execve((const char *)a1, (char *const *)a2, (char *const *)a3); break;
        case SYS_EXIT:    ret = sys_exit((int)a1); break;
        case SYS_GETPID:  ret = sys_getpid(); break;
        case SYS_WAITPID: ret = sys_waitpid((int)a1, (int *)a2, (int)a3); break;
        case SYS_KILL:    ret = wrap_sys_kill((int)a1, (int)a2); break;
        case SYS_SIGACTION: ret = wrap_sys_sigaction((int)a1, (const struct sigaction *)a2, (struct sigaction *)a3); break;
        case SYS_SIGRETURN: ret = wrap_sys_sigreturn(); break;
        case SYS_PIPE:    ret = wrap_sys_pipe((int *)a1); break;
        case SYS_FORK:    ret = sys_fork(); break;
        case SYS_UNAME:   ret = sys_uname((struct utsname *)a1); break;
        case SYS_TIMES:   ret = sys_times((struct tms *)a1); break;
        case SYS_GETCWD:  ret = sys_getcwd((char *)a1, (size_t)a2); break;
        case SYS_CHDIR:   ret = sys_chdir((const char *)a1); break;
        default:
            current->t_errno = ENOSYS;
            ret = -1;
            break;
    }
    return ret;
}

void syscall_trap(struct trapframe *tf) {
    if (!tf) return;
    int num       = (int)tf->rax;
    uint64_t a1   = tf->rdi;
    uint64_t a2   = tf->rsi;
    uint64_t a3   = tf->rdx;
    uint64_t a4   = tf->r10;  /* 4th syscall arg (x86_64 ABI) */
    uint64_t a5   = tf->r8;   /* 5th syscall arg */
    uint64_t a6   = tf->r9;   /* 6th syscall arg */

    /* Set global trapframe for signal delivery */
    current_tf = tf;

    /*
     * Fork child returns 0 to user space.
     * The child's kernel stack is set up by create_task() with fn=NULL,
     * so when the child is first scheduled, context_switch's ret jumps
     * to syscall.S's return path.  The trapframe's RAX was set by the
     * parent's syscall handler, but we override it here for the child.
     */
    if (current && current->is_fork_child) {
        current->is_fork_child = 0;
        tf->rax = 0;
    } else {
        /* Measure syscall latency via RDTSC */
        uint32_t tsc_lo_start, tsc_hi_start;
        asm volatile ("rdtsc" : "=a"(tsc_lo_start), "=d"(tsc_hi_start));
        long ret = handle_syscall(num, a1, a2, a3, a4, a5, a6);
        uint32_t tsc_lo_end, tsc_hi_end;
        asm volatile ("rdtsc" : "=a"(tsc_lo_end), "=d"(tsc_hi_end));
        uint64_t tsc_diff = (((uint64_t)tsc_hi_end << 32) | tsc_lo_end)
                          - (((uint64_t)tsc_hi_start << 32) | tsc_lo_start);
        perf_add_latency(PERF_SYSCALL_LATENCY, perf_tsc_to_ns(tsc_diff));
        tf->rax = (uint64_t)ret;
    }

    check_signals();
    current_tf = NULL;
}

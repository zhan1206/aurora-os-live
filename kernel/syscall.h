/*
 * syscall.h - System call numbers and interface
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* System call numbers (matching Linux x86_64 ABI where applicable) */
enum {
    SYS_READ      = 0,
    SYS_WRITE     = 1,
    SYS_OPEN      = 2,
    SYS_CLOSE     = 3,
    SYS_FSTAT     = 5,
    SYS_LSEEK     = 8,
    SYS_MMAP      = 9,
    SYS_MPROTECT  = 10,
    SYS_DUP       = 32,
    SYS_DUP2      = 33,
    SYS_UNAME     = 63,
    SYS_GETCWD    = 79,
    SYS_CHDIR     = 80,
    SYS_GETDENTS  = 78,   /* getdents64 on Linux */
    SYS_FORK      = 57,
    SYS_PIPE      = 22,
    SYS_GETPID    = 39,
    SYS_KILL      = 62,
    SYS_SIGACTION = 13,   /* rt_sigaction on Linux */
    SYS_SIGRETURN = 15,   /* rt_sigreturn on Linux */
    SYS_EXECVE    = 59,
    SYS_EXIT      = 60,
    SYS_WAITPID   = 61,
    SYS_TIMES     = 100,
};

/* struct utsname — POSIX uname structure */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* struct tms — POSIX times structure */
struct tms {
    uint64_t tms_utime;
    uint64_t tms_stime;
    uint64_t tms_cutime;
    uint64_t tms_cstime;
};

/* Called from syscall_entry.c (MSR setup) */
void syscall_init(void);

/* System call implementations (from pipe.c) */
int sys_pipe(int *fds);

/* System call implementations (from syscall.c) */
long sys_fork(void);

#endif /* SYSCALL_H */

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
    /* Network / socket syscalls */
    SYS_SOCKET    = 41,
    SYS_CONNECT   = 42,
    SYS_BIND      = 49,
    SYS_LISTEN    = 50,
    SYS_ACCEPT    = 43,
    SYS_SENDTO    = 44,
    SYS_RECVFROM  = 45,
    SYS_SEND      = 46,
    SYS_RECV      = 47,
    SYS_SHUTDOWN  = 48,
    SYS_GETSOCKNAME = 51,
    /* Time syscalls */
    SYS_GETTIMEOFDAY = 96,
    SYS_NANOSLEEP    = 35,
    /* Extended stat */
    SYS_STAT      = 4,
    /* Filesystem management */
    SYS_MKDIR     = 83,
    SYS_RMDIR     = 84,
    SYS_UNLINK    = 87,
    SYS_RENAME    = 82,
    SYS_CHMOD     = 90,
    /* Device control */
    SYS_IOCTL     = 16,
    /* I/O multiplexing */
    SYS_POLL      = 7,
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

/* struct timeval — POSIX timeval structure */
struct timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};

/* struct timespec — POSIX timespec structure */
struct timespec {
    uint64_t tv_sec;
    uint64_t tv_nsec;
};

/* struct sockaddr — basic socket address (AF_INET) */
struct sockaddr {
    uint16_t sa_family;
    uint8_t  sa_data[14];
};

/* struct sockaddr_in — IPv4 socket address */
struct sockaddr_in {
    uint16_t sin_family;  /* AF_INET = 2 */
    uint16_t sin_port;
    uint8_t  sin_addr[4];
    uint8_t  sin_zero[8];
};

/* AF_INET = 2 (IPv4) */
#define AF_INET 2

/* Socket types */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

/* struct pollfd — poll file descriptor */
struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* poll event flags */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010

/* struct stat — extended file stat */
struct kstat_ext {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

/* Called from syscall_entry.c (MSR setup) */
void syscall_init(void);

/* System call implementations (from pipe.c) */
int sys_pipe(int *fds);

/* System call implementations (from syscall.c) */
long sys_fork(void);

#endif /* SYSCALL_H */

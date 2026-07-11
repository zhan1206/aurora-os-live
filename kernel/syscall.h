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
    SYS_STAT      = 4,
    SYS_FSTAT     = 5,
    SYS_POLL      = 7,
    SYS_LSEEK     = 8,
    SYS_MMAP      = 9,
    SYS_MPROTECT  = 10,
    SYS_BRK       = 12,
    SYS_SIGACTION = 13,
    SYS_SIGRETURN = 15,
    SYS_IOCTL     = 16,
    SYS_ACCESS    = 21,
    SYS_PIPE      = 22,
    SYS_PIPE2     = 293,
    SYS_MADVISE   = 28,
    SYS_DUP       = 32,
    SYS_DUP2      = 33,
    SYS_NICE      = 34,
    SYS_NANOSLEEP = 35,
    SYS_GETPID    = 39,
    SYS_SOCKET    = 41,
    SYS_CONNECT   = 42,
    SYS_ACCEPT    = 43,
    SYS_SENDTO    = 44,
    SYS_RECVFROM  = 45,
    SYS_SEND      = 46,
    SYS_RECV      = 47,
    SYS_SHUTDOWN  = 48,
    SYS_BIND      = 49,
    SYS_LISTEN    = 50,
    SYS_GETSOCKNAME = 51,
    SYS_FORK      = 57,
    SYS_EXECVE    = 59,
    SYS_EXIT      = 60,
    SYS_WAITPID   = 61,
    SYS_KILL      = 62,
    SYS_UNAME     = 63,
    SYS_FCNTL     = 72,
    SYS_FSYNC     = 74,
    SYS_FTRUNCATE = 77,
    SYS_GETDENTS  = 78,
    SYS_GETCWD    = 79,
    SYS_CHDIR     = 80,
    SYS_RENAME    = 82,
    SYS_MKDIR     = 83,
    SYS_RMDIR     = 84,
    SYS_UNLINK    = 87,
    SYS_SYMLINK   = 88,
    SYS_READLINK  = 89,
    SYS_CHMOD     = 90,
    SYS_FCHMOD    = 91,
    SYS_CHOWN     = 92,
    SYS_FCHOWN    = 93,
    SYS_GETTIMEOFDAY = 96,
    SYS_TIMES     = 100,
    SYS_GETUID    = 102,
    SYS_GETGID    = 104,
    SYS_SETUID    = 105,
    SYS_SETGID    = 106,
    SYS_GETEUID   = 107,
    SYS_GETEGID   = 108,
    SYS_SETPGID   = 109,
    SYS_GETPPID   = 110,
    SYS_SETSID    = 112,
    SYS_GETPGID   = 121,
    SYS_CLOCK_GETTIME = 228,
    /* Custom AuroraOS syscalls */
    SYS_SBRK      = 256,
    SYS_SYSINFO   = 99,
    SYS_GETRLIMIT = 97,
    SYS_SETRLIMIT = 160,
    SYS_SCHED_YIELD = 24,
    SYS_GETRANDOM = 318,
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

/* waitpid options */
#define WNOHANG    1   /* return immediately if no child has exited */

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

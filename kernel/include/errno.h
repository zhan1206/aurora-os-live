/*
 * errno.h - Error code definitions (POSIX-compatible)
 *
 * Defines standard error numbers for system calls and kernel operations.
 * Negative values follow the convention: return -EXXX for errors.
 */
#ifndef ERRNO_H
#define ERRNO_H

#define EPERM    1    /* Operation not permitted */
#define ENOENT   2    /* No such file or directory */
#define ESRCH    3    /* No such process */
#define EINTR    4    /* Interrupted system call */
#define EIO      5    /* I/O error */
#define ENXIO    6    /* No such device or address */
#define E2BIG    7    /* Argument list too long */
#define EBADF    9    /* Bad file descriptor */
#define ECHILD   10   /* No child processes */
#define EAGAIN   11   /* Try again */
#define ENOMEM   12   /* Out of memory */
#define EACCES   13   /* Permission denied */
#define EFAULT   14   /* Bad address */
#define EBUSY    16   /* Device or resource busy */
#define EEXIST   17   /* File exists */
#define ENODEV   19   /* No such device */
#define ENOTDIR  20   /* Not a directory */
#define EISDIR   21   /* Is a directory */
#define EINVAL   22   /* Invalid argument */
#define ENFILE   23   /* File table overflow */
#define EMFILE   24   /* Too many open files */
#define ENOTTY   25   /* Not a typewriter */
#define ENOSPC   28   /* No space left on device */
#define ESPIPE   29   /* Illegal seek */
#define EPIPE    32   /* Broken pipe */
#define ENOSYS   38   /* Function not implemented */
#define ENAMETOOLONG 36 /* File name too long */
#define ERANGE   34   /* Math result not representable */
#define ENOTSOCK 88   /* Socket operation on non-socket */
#define EOVERFLOW 75  /* Value too large for defined data type */
#define EAFNOSUPPORT 97   /* Address family not supported */
#define EPROTONOSUPPORT 93 /* Protocol not supported */
#define EADDRINUSE 98  /* Address already in use */
#define ECONNREFUSED 111 /* Connection refused */
#define ECONNRESET 104 /* Connection reset by peer */
#define ENETUNREACH 101 /* Network is unreachable */
#define ETIMEDOUT 110  /* Connection timed out */
#define EADDRNOTAVAIL 99 /* Cannot assign requested address */
#define ENOTEMPTY 39   /* Directory not empty */
#define EROFS    30    /* Read-only file system */
#define EXDEV    18    /* Cross-device link */
#define ENODATA  61    /* No data available */
#define EISCONN  106   /* Transport endpoint is already connected */
#define ENOTCONN 107   /* Transport endpoint is not connected */
#define ESHUTDOWN 108  /* Cannot send after transport endpoint shutdown */

/* Per-task error number — accessed via current->t_errno in task_struct */
/* (see sched.h for the task_struct definition) */

#endif /* ERRNO_H */

/*
 * explain.c - Kernel decision explainability (Vision §10 foundation)
 *
 * Records the "why" behind every significant kernel decision.
 */
#include "include/log.h"
#include "include/kstdio.h"
#include "include/errno.h"
#include "sched.h"
#include "signal.h"
#include "mem.h"
#include <string.h>
#include <stdint.h>

#define EXPLAIN_BUF_SIZE 128
#define EXPLAIN_MAX_EVENTS (EXPLAIN_BUF_SIZE / sizeof(struct explain_event))

enum explain_type {
    EXPLAIN_PROCESS_EXIT    = 0,
    EXPLAIN_SCHEDULE        = 1,
    EXPLAIN_SIGNAL_DELIVER  = 2,
    EXPLAIN_ALLOC_FAIL      = 3,
    EXPLAIN_OOM_KILL        = 4,
};

struct explain_event {
    uint32_t type;
    uint32_t pid;
    uint64_t timestamp;
    char     reason[112];
};

static struct explain_event explain_buf[EXPLAIN_MAX_EVENTS];
static uint32_t explain_head = 0;
static uint64_t explain_ts   = 0;

static void explain_record(uint32_t type, uint32_t pid, const char *reason) {
    struct explain_event *e = &explain_buf[explain_head % EXPLAIN_MAX_EVENTS];
    e->type      = type;
    e->pid       = pid;
    e->timestamp = explain_ts++;

    size_t len = strlen(reason);
    if (len > sizeof(e->reason) - 1) len = sizeof(e->reason) - 1;
    memcpy(e->reason, reason, len);
    e->reason[len] = '\0';
    explain_head++;
}

void explain_exit(int pid, int code, int signal) {
    char buf[112];
    char *p = buf;
    const char *s1;

    /* Build: "pid=N terminated by signal S" or "pid=N exited with code C" */
    s1 = "pid="; while (*s1) *p++ = *s1++;
    p += itoa(pid, p, sizeof(buf) - (size_t)(p - buf));
    if (signal) {
        s1 = " terminated by signal ";
        while (*s1) *p++ = *s1++;
        p += itoa(signal, p, sizeof(buf) - (size_t)(p - buf));
    } else {
        s1 = " exited with code ";
        while (*s1) *p++ = *s1++;
        p += itoa(code, p, sizeof(buf) - (size_t)(p - buf));
    }
    *p = '\0';
    explain_record(EXPLAIN_PROCESS_EXIT, (uint32_t)pid, buf);
}

void explain_signal(int pid, int sig, const char *action) {
    if (sig < 0 || sig >= NSIG || !action) return;
    char buf[112];
    char *p = buf;
    const char *s1;
    s1 = "pid="; while (*s1) *p++ = *s1++;
    p += itoa(pid, p, sizeof(buf) - (size_t)(p - buf));
    s1 = " signal "; while (*s1) *p++ = *s1++;
    p += itoa(sig, p, sizeof(buf) - (size_t)(p - buf));
    *p++ = ':'; *p++ = ' ';
    while (*action) *p++ = *action++;
    *p = '\0';
    explain_record(EXPLAIN_SIGNAL_DELIVER, (uint32_t)pid, buf);
}

void explain_oom_kill(int victim_pid, const char *reason) {
    if (!reason) return;
    char buf[112];
    char *p = buf;
    const char *s1 = "OOM killed pid=";
    while (*s1) *p++ = *s1++;
    p += itoa(victim_pid, p, sizeof(buf) - (size_t)(p - buf));
    *p++ = ':'; *p++ = ' ';
    while (*reason) *p++ = *reason++;
    *p = '\0';
    explain_record(EXPLAIN_OOM_KILL, (uint32_t)victim_pid, buf);
    log_printf(LOG_LEVEL_ERR, "explain: %s\n", buf);
}

static const char *explain_type_names[] = {
    "EXIT", "SCHED", "SIGNAL", "ALLOC_FAIL", "OOM_KILL"
};

void explain_dump(void (*emit)(const char *line)) {
    if (!emit) return;
    uint32_t total = explain_head;
    uint32_t start = (total > EXPLAIN_MAX_EVENTS) ? total - EXPLAIN_MAX_EVENTS : 0;
    uint32_t count = total - start;
    char line[256];

    for (uint32_t i = 0; i < count; ++i) {
        struct explain_event *e = &explain_buf[(start + i) % EXPLAIN_MAX_EVENTS];
        const char *tname = (e->type < 5) ? explain_type_names[e->type] : "???";
        char *p = line;
        *p++ = '['; p += uitoa(e->timestamp, p, sizeof(line) - (size_t)(p - line));
        *p++ = ']'; *p++ = ' ';
        while (*tname) *p++ = *tname++;
        *p++ = ' '; *p++ = 'p'; *p++ = 'i'; *p++ = 'd'; *p++ = '=';
        p += uitoa((uint64_t)e->pid, p, sizeof(line) - (size_t)(p - line));
        *p++ = ' ';
        const char *r = e->reason;
        while (*r) *p++ = *r++;
        *p = '\0';
        emit(line);
    }
}

/* ================================================================
 * errno → human-readable string (for shell error messages)
 * ================================================================ */
const char *explain_errno(int err) {
    switch (err) {
        case EPERM:    return "Operation not permitted";
        case ENOENT:   return "No such file or directory";
        case ESRCH:    return "No such process";
        case EINTR:    return "Interrupted system call";
        case EIO:      return "I/O error";
        case EBADF:    return "Bad file descriptor";
        case ECHILD:   return "No child processes";
        case EAGAIN:   return "Resource temporarily unavailable";
        case ENOMEM:   return "Out of memory";
        case EACCES:   return "Permission denied";
        case EFAULT:   return "Bad address";
        case EBUSY:    return "Device or resource busy";
        case EEXIST:   return "File exists";
        case ENODEV:   return "No such device";
        case ENOTDIR:  return "Not a directory";
        case EISDIR:   return "Is a directory";
        case EINVAL:   return "Invalid argument";
        case ENFILE:   return "File table overflow";
        case EMFILE:   return "Too many open files";
        case ENOSPC:   return "No space left on device";
        case EPIPE:    return "Broken pipe";
        case ENOSYS:   return "Function not implemented";
        default:       return "Unknown error";
    }
}

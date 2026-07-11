/*
 * signal.c - POSIX signal framework (Phase 1: complete handler delivery)
 */
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "include/log.h"
#include "include/userspace.h"
#include "include/trapframe.h"
#include "mem.h"
#include <string.h>
#include <stdint.h>

struct signal_state *signal_alloc(void) {
    struct signal_state *s = (struct signal_state *)kmalloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    return s;
}

/* ================================================================
 * do_sys_kill
 * ================================================================ */
int do_sys_kill(int pid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;
    if (pid < 0) return -1;

    struct task_struct *target = find_task_by_pid(pid);
    if (!target) return -1;

    log_printf(LOG_LEVEL_DEBUG, "signal: kill(pid=%d, sig=%d)\n", pid, sig);

    if (target->sig) {
        target->sig->pending |= (1U << sig);
    } else {
        target->sig = signal_alloc();
        if (!target->sig) return -1;
        target->sig->pending |= (1U << sig);
    }

    if (target->state == TASK_BLOCKED) {
        if (sig == SIGKILL ||
            (target->sig->actions[sig].sa_handler != SIG_IGN)) {
            target->state = TASK_READY;
        }
    }

    return 0;
}

/* ================================================================
 * do_sys_sigaction
 * ================================================================ */
int do_sys_sigaction(int signo, const struct sigaction *act,
                      struct sigaction *oldact) {
    if (signo < 1 || signo >= NSIG) return -1;
    if (signo == SIGKILL) return -1;

    if (!current->sig) {
        current->sig = signal_alloc();
        if (!current->sig) return -1;
    }

    if (oldact) {
        if (copy_to_user(oldact, &current->sig->actions[signo],
                         sizeof(struct sigaction)) != 0)
            return -1;
    }

    if (act) {
        if (copy_from_user(&current->sig->actions[signo], act,
                           sizeof(struct sigaction)) != 0)
            return -1;
    }

    return 0;
}

/* ================================================================
 * do_sys_sigreturn: Restore user context from sigframe
 *
 * When a user signal handler returns, it calls the sigreturn syscall.
 * The sigframe is at the top of the user stack (RSP when sigreturn
 * was invoked). We read it and restore the trapframe so that
 * syscall.S's iretq/sysretq returns to the original user context.
 * ================================================================ */
void do_sys_sigreturn(void) {
    if (!current_tf) {
        log_printf(LOG_LEVEL_ERR, "signal: sigreturn with no trapframe\n");
        do_exit_current(1);
        return;
    }

    if (!current->sig) {
        log_printf(LOG_LEVEL_WARN, "signal: sigreturn with no signal state\n");
        return;
    }

    if (current->sig->saved_rip) {
        /*
         * Restore the sigframe from the user stack. The sigframe was
         * placed at user_rsp - sizeof(struct sigframe) by check_signals(),
         * where user_rsp is the original RSP before signal delivery.
         *
         * Stack layout (low to high):
         *   [new_rsp]         = return addr (8 bytes) → consumed by handler's ret
         *   [new_rsp+8]       = trampoline (16 bytes)
         *   [new_rsp+24]      = sigframe (sizeof(struct sigframe) bytes)
         *   [orig user_rsp]   = original stack top
         *
         * new_rsp = user_rsp - (sizeof(sigframe) + 8 + TRAMPOLINE_SIZE)
         * sigframe is at: user_rsp - sizeof(sigframe)
         */
        uint64_t user_rsp = current->sig->saved_rsp;
        uint64_t frame_addr = user_rsp - sizeof(struct sigframe);

        struct sigframe frame;
        if (copy_from_user(&frame, (void *)(uintptr_t)frame_addr,
                           sizeof(frame)) == 0 &&
            frame.signo > 0 && frame.signo < NSIG) {
            /* Restore all general-purpose registers from the sigframe */
            current_tf->r15 = frame.r15;
            current_tf->r14 = frame.r14;
            current_tf->r13 = frame.r13;
            current_tf->r12 = frame.r12;
            current_tf->r11 = frame.r11;
            current_tf->r10 = frame.r10;
            current_tf->r9  = frame.r9;
            current_tf->r8  = frame.r8;
            current_tf->rsi = frame.rsi;
            current_tf->rdi = frame.rdi;
            current_tf->rdx = frame.rdx;
            current_tf->rcx = frame.rcx;
            current_tf->rax = frame.rax;
            current_tf->rip = frame.rip;
            current_tf->rsp = frame.rsp;
        } else {
            /* Fallback: restore RIP/RSP from saved context only */
            current_tf->rip = current->sig->saved_rip;
            current_tf->rsp = current->sig->saved_rsp;
        }

        current->sig->saved_rip = 0;
        current->sig->saved_rsp = 0;

        /* Unblock all signals that were blocked during handler */
        current->sig->blocked = 0;

        log_printf(LOG_LEVEL_DEBUG, "signal: sigreturn restored RIP=%p RSP=%p\n",
                   (void *)current_tf->rip, (void *)current_tf->rsp);
    } else {
        log_printf(LOG_LEVEL_WARN, "signal: sigreturn with no saved context\n");
    }
}

/* ================================================================
 * do_signal_default
 * ================================================================ */
void do_signal_default(int sig) {
    switch (sig) {
        case SIGKILL:
        case SIGTERM:
        case SIGINT:
            log_printf(LOG_LEVEL_INFO, "signal: terminating pid=%d on sig=%d\n",
                       current->pid, sig);
            do_exit_current(128 + sig);
            break;
        case SIGCHLD:
            break;
        default:
            log_printf(LOG_LEVEL_INFO, "signal: terminating pid=%d on sig=%d\n",
                       current->pid, sig);
            do_exit_current(128 + sig);
            break;
    }
}

/* ================================================================
 * check_signals: Deliver pending signals
 *
 * Now with full user-handler support: pushes a sigframe onto the
 * user stack and redirects RIP/RSP in the trapframe.
 * ================================================================ */
void check_signals(void) {
    if (!current) return;
    if (!current->sig) return;
    if (current->sig->pending == 0) return;
    if (!current_tf) return;  /* no trapframe = kernel context, defer */

    struct signal_state *sig = current->sig;

    for (int s = 1; s < NSIG; ++s) {
        if (!(sig->pending & (1U << s))) continue;
        if (sig->blocked & (1U << s)) continue;

        sig->pending &= ~(1U << s);
        sighandler_t handler = sig->actions[s].sa_handler;

        if (handler == SIG_DFL) {
            do_signal_default(s);
            return;
        }

        if (handler == SIG_IGN) {
            continue;
        }

        /*
         * User-defined handler: push sigframe onto user stack
         * and redirect execution to the handler.
         *
         * User stack layout after setup (grows downward):
         *   [original RSP]      ← original top
         *   [sigframe]          ← saved registers
         *   [trampoline code]   ← mov eax, SYS_SIGRETURN; syscall (8 bytes)
         *   [return addr]       ← pointer to trampoline code
         *   [new RSP]           ← trapframe->rsp points here
         *
         * The handler is called with (signo) in RDI.
         * When it returns (ret), it pops the return address which
         * points to the trampoline code that calls syscall(SYS_SIGRETURN).
         *
         * Saved context is stored in per-task signal_state,
         * not in globals (fixes thread-safety issue).
         */
        uint64_t user_rsp = current_tf->rsp;

        /* Trampoline code: mov eax, SYS_SIGRETURN; syscall (7 bytes) */
        #define TRAMPOLINE_SIZE 16  /* 16-byte aligned for safety */
        uint64_t frame_size = sizeof(struct sigframe) + 8 + TRAMPOLINE_SIZE;

        /* Check user stack bounds */
        if (user_rsp < frame_size + 0x1000) {
            log_printf(LOG_LEVEL_ERR, "signal: user stack too small for sigframe\n");
            do_signal_default(s);
            return;
        }

        uint64_t new_rsp = user_rsp - frame_size;

        /* Validate new_rsp is still in user address space (not wrapped to kernel) */
        if (new_rsp > user_rsp) {
            log_printf(LOG_LEVEL_ERR, "signal: stack underflow detected\n");
            do_signal_default(s);
            return;
        }

        /* Validate the entire trampoline + sigframe region is in valid user memory */
        size_t frame_total = 8 + TRAMPOLINE_SIZE + sizeof(struct sigframe);
        if (!user_addr_range_ok((const void *)(uintptr_t)new_rsp, frame_total) ||
            !user_pages_mapped((const void *)(uintptr_t)new_rsp, frame_total)) {
            log_printf(LOG_LEVEL_ERR, "signal: user stack region invalid at %p\n",
                       (void *)(uintptr_t)new_rsp);
            do_signal_default(s);
            return;
        }

        /* Write trampoline code at new_rsp + 8.
         * Temporarily disable SMAP via STAC to allow kernel access
         * to user-space stack for signal frame setup. */
        asm volatile ("stac" ::: "memory");

        uint8_t *tramp = (uint8_t *)(uintptr_t)(new_rsp + 8);
        tramp[0] = 0xB8;                        /* mov eax, imm32 */
        {
            uint32_t sigret = (uint32_t)SYS_SIGRETURN;
            tramp[1] = (uint8_t)(sigret);
            tramp[2] = (uint8_t)(sigret >> 8);
            tramp[3] = (uint8_t)(sigret >> 16);
            tramp[4] = (uint8_t)(sigret >> 24);
        }
        tramp[5] = 0x0F;                        /* syscall */
        tramp[6] = 0x05;
        /* Zero the remaining trampoline bytes for security */
        memset(tramp + 7, 0, TRAMPOLINE_SIZE - 7);

        /* Write return address pointing to trampoline code */
        *(uint64_t *)(uintptr_t)new_rsp = new_rsp + 8;

        /* Place sigframe above trampoline */
        struct sigframe *frame = (struct sigframe *)(uintptr_t)(new_rsp + 8 + TRAMPOLINE_SIZE);

        /* Save current user context */
        frame->signo  = s;
        frame->r15    = current_tf->r15;
        frame->r14    = current_tf->r14;
        frame->r13    = current_tf->r13;
        frame->r12    = current_tf->r12;
        frame->r11    = current_tf->r11;
        frame->r10    = current_tf->r10;
        frame->r9     = current_tf->r9;
        frame->r8     = current_tf->r8;
        frame->rsi    = current_tf->rsi;
        frame->rdi    = current_tf->rdi;
        frame->rdx    = current_tf->rdx;
        frame->rcx    = current_tf->rcx;
        frame->rax    = current_tf->rax;
        frame->rip    = current_tf->rip;
        frame->rsp    = user_rsp;
        /* RFLAGS not directly in trapframe but R11 holds it for syscall */

        /* Store saved context in per-task signal_state (thread-safe) */
        sig->saved_rsp = frame->rsp;
        sig->saved_rip = frame->rip;

        /* Modify trapframe: redirect to handler */
        current_tf->rip = (uint64_t)(uintptr_t)handler;
        current_tf->rsp = new_rsp;
        current_tf->rdi = (uint64_t)s;  /* arg0 = signo */

        /* Re-enable SMAP via CLAC — user-space writes are done */
        asm volatile ("clac" ::: "memory");

        /* Block the signal during handler execution */
        sig->blocked |= (1U << s);

        log_printf(LOG_LEVEL_DEBUG, "signal: delivering sig=%d handler=%p\n",
                   s, (void *)handler);
        return;
    }
}

/* ================================================================
 * signal_child_event
 * ================================================================ */
void signal_child_event(struct task_struct *child, int event) {
    (void)event;
    if (!child || !child->parent) return;
    do_sys_kill(child->parent->pid, SIGCHLD);
}

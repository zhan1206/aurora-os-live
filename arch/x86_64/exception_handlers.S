/*
 * exception_handlers.S - Exception entry stubs for x86_64 (NEW)
 *
 * Each exception handler:
 *   1. Saves caller-saved registers
 *   2. Calls generic_exception_handler(vector, error_code, RIP)
 *   3. Halts (exceptions are fatal in this kernel)
 *
 * Vectors with error code: 8 (#DF), 10 (#TS), 11 (#NP), 12 (#SS),
 *                           13 (#GP), 14 (#PF), 17 (#AC), 30 (#SX)
 * Vectors without error code: all others
 */

    .text

    .extern generic_exception_handler

/*
 * Macro: EXC_HANDLER_NOERR(vector)
 * Handler for exceptions that do NOT push an error code.
 * We push a dummy 0 to keep the stack frame uniform.
 */
.macro EXC_HANDLER_NOERR vector
    .global exc_handler_\vector
    .type exc_handler_\vector, @function
exc_handler_\vector:
    pushq $0                    /* dummy error code */
    pushq $\vector              /* vector number */
    jmp exc_common
.endm

/*
 * Macro: EXC_HANDLER_ERR(vector)
 * Handler for exceptions that DO push an error code.
 */
.macro EXC_HANDLER_ERR vector
    .global exc_handler_\vector
    .type exc_handler_\vector, @function
exc_handler_\vector:
    pushq $\vector              /* vector number */
    jmp exc_common
.endm

/* ================================================================
 * Common exception entry point
 *
 * Stack layout on entry:
 *   [RSP+16] = original RIP (from CPU interrupt frame)
 *   [RSP+8]  = error code (pushed by CPU or dummy)
 *   [RSP+0]  = vector number (pushed by macro)
 *
 * We save all caller-saved registers, then call the C handler.
 * Exceptions are fatal — we panic and halt.
 * ================================================================ */
exc_common:
    /* Save caller-saved registers */
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rax

    /*
     * Arguments for generic_exception_handler:
     *   %rdi = vector number   (at RSP + 13*8)
     *   %rsi = error code       (at RSP + 14*8)
     *   %rdx = RIP              (at RSP + 15*8)
     */
    mov 13*8(%rsp), %rdi       /* vector number */
    mov 14*8(%rsp), %rsi       /* error code */
    mov 15*8(%rsp), %rdx       /* RIP */

    call generic_exception_handler

    /* NOTREACHED: panic halts */
    cli
1:  hlt
    jmp 1b

/* ================================================================
 * Generate handlers for all 32 exception vectors
 * ================================================================ */

/* Vector 0: Divide Error (no error code) */
    EXC_HANDLER_NOERR 0

/* Vector 1: Debug (no error code) */
    EXC_HANDLER_NOERR 1

/* Vector 2: NMI (no error code) */
    EXC_HANDLER_NOERR 2

/* Vector 3: Breakpoint (no error code) */
    EXC_HANDLER_NOERR 3

/* Vector 4: Overflow (no error code) */
    EXC_HANDLER_NOERR 4

/* Vector 5: Bound Range (no error code) */
    EXC_HANDLER_NOERR 5

/* Vector 6: Invalid Opcode (no error code) */
    EXC_HANDLER_NOERR 6

/* Vector 7: Device Not Available (no error code) */
    EXC_HANDLER_NOERR 7

/* Vector 8: Double Fault (has error code, always 0) */
    EXC_HANDLER_ERR 8

/* Vector 9: Coprocessor Segment Overrun (no error code) */
    EXC_HANDLER_NOERR 9

/* Vector 10: Invalid TSS (has error code) */
    EXC_HANDLER_ERR 10

/* Vector 11: Segment Not Present (has error code) */
    EXC_HANDLER_ERR 11

/* Vector 12: Stack Fault (has error code) */
    EXC_HANDLER_ERR 12

/* Vector 13: General Protection (has error code) */
    EXC_HANDLER_ERR 13

/* Vector 14: Page Fault — handled separately in pf_handler.S */
/* Vector 15: Reserved by Intel */

/* Vector 16: x87 FPU Error (no error code) */
    EXC_HANDLER_NOERR 16

/* Vector 17: Alignment Check (has error code) */
    EXC_HANDLER_ERR 17

/* Vector 18: Machine Check (no error code) */
    EXC_HANDLER_NOERR 18

/* Vector 19: SIMD Exception (no error code) */
    EXC_HANDLER_NOERR 19

/* Vector 20: Virtualization (no error code) */
    EXC_HANDLER_NOERR 20

/* Vectors 21-29: Reserved */
    EXC_HANDLER_NOERR 21
    EXC_HANDLER_NOERR 22
    EXC_HANDLER_NOERR 23
    EXC_HANDLER_NOERR 24
    EXC_HANDLER_NOERR 25
    EXC_HANDLER_NOERR 26
    EXC_HANDLER_NOERR 27
    EXC_HANDLER_NOERR 28
    EXC_HANDLER_NOERR 29

/* Vector 30: Security Exception (has error code) */
    EXC_HANDLER_ERR 30

/* Vector 31: Reserved */
    EXC_HANDLER_NOERR 31

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

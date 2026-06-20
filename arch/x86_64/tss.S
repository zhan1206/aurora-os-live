/*
 * tss.S - Task State Segment for x86_64 (NEW)
 *
 * Provides IST (Interrupt Stack Table) entries for critical exceptions.
 * IST1 is used by Double Fault (#DF) to ensure it has a valid stack
 * even if the main kernel stack is corrupted.
 *
 * (Report §5.1, issue #3: no IST for critical exceptions)
 * (Report §13.3: no interrupt stack isolation)
 *
 * TSS layout (long mode, 104 bytes + IST + IOPB):
 *   [0x00] reserved  (4 bytes)
 *   [0x04] RSP0      (8 bytes) — stack for ring 0 (not used with IST)
 *   [0x0C] RSP1      (8 bytes)
 *   [0x14] RSP2      (8 bytes)
 *   [0x1C] reserved  (8 bytes)
 *   [0x24] IST1      (8 bytes) — stack for IST index 1
 *   [0x2C] IST2      (8 bytes)
 *   [0x34] IST3      (8 bytes)
 *   [0x3C] IST4      (8 bytes)
 *   [0x44] IST5      (8 bytes)
 *   [0x4C] IST6      (8 bytes)
 *   [0x54] IST7      (8 bytes)
 *   [0x5C] reserved  (8 bytes)
 *   [0x64] reserved  (2 bytes)
 *   [0x66] IOPB offset (2 bytes)
 */

    .section .data
    .align 16
    .global tss
    .type tss, @object
tss:
    .skip 104                /* TSS body, zero-initialized */
    .quad 0                  /* IOPB: no I/O permission bitmap */
tss_end:

    .section .bss
    .align 16
    /*
     * IST stacks: 4 KiB each for critical exception handlers.
     * IST1: Double Fault (#DF, vector 8)
     * IST2: NMI (vector 2)
     * IST3: Machine Check (#MC, vector 18)
     */
ist1_stack:
    .skip 4096
ist1_stack_top:

ist2_stack:
    .skip 4096
ist2_stack_top:

ist3_stack:
    .skip 4096
ist3_stack_top:

    .section .text
    .global tss_init
    .type tss_init, @function

/*
 * tss_init: Initialize the TSS with IST stack pointers.
 *
 * Must be called after memory management is set up.
 * The TSS descriptor in GDT must already be loaded.
 */
tss_init:
    /* Set IST1 (Double Fault stack) */
    lea ist1_stack_top(%rip), %rax
    mov %rax, tss + 0x24

    /* Set IST2 (NMI stack) */
    lea ist2_stack_top(%rip), %rax
    mov %rax, tss + 0x2C

    /* Set IST3 (Machine Check stack) */
    lea ist3_stack_top(%rip), %rax
    mov %rax, tss + 0x34

    /* Load TSS via LTR instruction.
     * TSS selector must be in GDT. Our GDT has 5 entries:
     *   0: null, 1: kernel code, 2: kernel data,
     *   3: user code, 4: user data, 5: TSS
     * TSS selector = 5 * 8 = 0x28 */
    mov $0x28, %ax
    ltr %ax

    ret

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

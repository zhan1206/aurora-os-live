/*
 * arch/aarch64/boot.S - AArch64 boot entry point
 *
 * Expects to be entered at EL1 (or higher) with the MMU disabled.
 * Configures exception levels, sets up the stack, and jumps to kernel_main.
 *
 * On entry:
 *   x0 = device tree blob (DTB) physical address
 *   x1-x3 = reserved (0)
 */
    .section .text.boot
    .global _start
    .type _start, @function

_start:
    /*
     * Check current exception level.  We want to be at EL1.
     * If we're at EL2 or EL3, drop to EL1.
     */
    mrs x4, CurrentEL
    lsr x4, x4, #2
    and x4, x4, #3

    /* If EL3, configure and drop to EL2 */
    cmp x4, #3
    b.ne 1f

    /* EL3: set EL2 to be AArch64 */
    mov x5, #0x30
    orr x5, x5, #(1 << 10)    /* RW = 1 (AArch64) */
    msr scr_el3, x5

    /* Set EL2 entry address */
    adr x5, 2f
    msr elr_el3, x5

    /* Set SPSR for EL2 (AArch64, DAIF masked) */
    mov x5, #0x3C5
    msr spsr_el3, x5
    eret

1:
    /* If EL2, configure and drop to EL1 */
    cmp x4, #2
    b.ne 3f

2:
    /* EL2: configure HCR_EL2 for EL1 execution */
    mov x5, #(1 << 31)         /* RW = 1 (AArch64) */
    msr hcr_el2, x5

    /* Set EL1 entry address */
    adr x5, 3f
    msr elr_el2, x5

    /* Set SPSR for EL1 (AArch64, DAIF masked) */
    mov x5, #0x3C5
    msr spsr_el2, x5
    eret

3:
    /* Now at EL1 */

    /* Disable MMU and caches */
    mrs x5, sctlr_el1
    bic x5, x5, #1              /* Clear M bit (disable MMU) */
    bic x5, x5, #(1 << 2)       /* Clear C bit (disable data cache) */
    bic x5, x5, #(1 << 12)      /* Clear I bit (disable instruction cache) */
    msr sctlr_el1, x5

    /* Ensure all previous operations are complete */
    dsb sy
    isb

    /* Mask all interrupts */
    msr daifset, #0xF

    /* Set up stack pointer */
    adrp x5, _stack_top
    add sp, x5, :lo12:_stack_top

    /* Save DTB address for kernel_main */
    mov x19, x0

    /* Clear BSS */
    adrp x5, _bss_start
    add x5, x5, :lo12:_bss_start
    adrp x6, _bss_end
    add x6, x6, :lo12:_bss_end
1:  cmp x5, x6
    b.hs 2f
    str xzr, [x5], #8
    b 1b
2:

    /* Jump to kernel_main */
    mov x0, x19                 /* DTB pointer */
    bl kernel_main

    /* If kernel_main returns, halt */
halt:
    wfi
    b halt

    .section .note.GNU-stack,"",@progbits
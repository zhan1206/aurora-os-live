/*
 * arch/riscv64/boot.S - RISC-V 64-bit boot entry point
 *
 * Sets up stack and jumps to kernel_main.
 * Assumes machine mode (M-mode) entry with BBL/OpenSBI or
 * supervisor mode (S-mode) entry via SBI.
 */
    .section .text.boot
    .global _start
    .type _start, @function

_start:
    /* Disable interrupts */
    csrw sie, zero
    csrw sip, zero

    /* Set up stack pointer (stack top defined in linker script) */
    la sp, _stack_top

    /* Clear BSS */
    la t0, _bss_start
    la t1, _bss_end
1:  bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
2:

    /* Jump to kernel_main */
    /* On RISC-V, a0 holds the hart ID (from SBI) */
    /* a1 holds a pointer to the device tree blob (DTB) */
    call kernel_main

    /* If kernel_main returns, halt */
halt:
    wfi
    j halt

    .section .note.GNU-stack,"",@progbits
/*
 * arch/loongarch64/boot.S - LoongArch 64-bit boot entry point
 *
 * LoongArch is a MIPS-derived RISC architecture developed by Loongson.
 * Uses the LA64 instruction set (64-bit).
 *
 * On entry:
 *   a0 = firmware argument (usually ACPI/DTB pointer)
 *   a1 = boot argument (usually 0)
 */
    .section .text.boot
    .global _start
    .type _start, @function

_start:
    /* Disable interrupts */
    csrwr zero, 0x0             /* CRMD.IE = 0 */

    /* Set up stack pointer */
    la.abs $sp, _stack_top

    /* Clear BSS */
    la.abs $t0, _bss_start
    la.abs $t1, _bss_end
1:  bgeu $t0, $t1, 2f
    st.d $zero, $t0, 0
    addi.d $t0, $t0, 8
    b 1b
2:

    /* Set up direct address translation mode (DA=1, PG=0) */
    csrrd $t0, 0x0              /* Read CRMD */
    /* Set DA=1 (direct mapping), PG=0 (no paging) */
    ori $t0, $t0, (1 << 1)
    /* Clear IE (interrupt enable).
     * ~(1<<2) = 0xFFFFFFFFFFFFFFFB which exceeds the 12-bit immediate
     * field of andi.  Use a two-step approach: load the mask into a
     * register first, then AND. */
    li.w $t1, ~(1 << 2)
    and $t0, $t0, $t1
    csrwr $t0, 0x0

    /* Jump to kernel_main */
    /* a0 = firmware argument, a1 = boot argument */
    bl kernel_main

    /* If kernel_main returns, halt */
halt:
    idle 0
    b halt

    .section .note.GNU-stack,"",@progbits
/*
 * keyboard_handler.S - IRQ1 (keyboard) entry/exit stub (FIXED v2)
 *
 * Runs in kernel context — no swapgs needed.
 * swapgs is only for user↔kernel transitions via syscall/sysret.
 */
    .text
    .global irq1_handler
    .type irq1_handler,@function

    .extern keyboard_c_handler

irq1_handler:
    /* Save caller-saved registers */
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11

    /* Call C handler */
    mov %rsp, %rdi
    call keyboard_c_handler

    /* Restore registers */
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax

    /* Send EOI to master PIC */
    mov $0x20, %al
    outb %al, $0x20

    /* Send EOI to slave PIC (IRQ cascades through slave) */
    outb %al, $0xA0

    iretq

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

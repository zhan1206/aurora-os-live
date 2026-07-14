    .text
    .global context_switch
    .type context_switch,@function

/*
 * context_switch(old_rsp_ptr, new_rsp)
 * old_rsp_ptr: pointer to where to store current RSP (uint64_t*)
 * new_rsp: new RSP value to load
 */
context_switch:
    /* Save RFLAGS to preserve interrupt flag state */
    pushfq

    /* Save registers callee-saved (rbx, rbp, r12-r15) */
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    /* Save current stack pointer into *old_rsp_ptr */
    mov %rdi, %rax
    mov %rsp, (%rax)
    /* Load new stack pointer */
    mov %rsi, %rsp

    /* Restore registers from new stack (pop in reverse) */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp

    /* Restore RFLAGS from new context's saved value */
    popfq

    /* Return to new context (ret will pop RIP from new stack) */
    ret

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

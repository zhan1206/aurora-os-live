    .text
    .global start_user
    .type start_user,@function

/* stack layout when start_user is entered by ret: rsp points to the first pushed value (USER_STACK_TOP)
   we pushed: entry, USER_STACK_TOP, start_user
   After ret, rsp -> USER_STACK_TOP, next -> entry
   So pop USER_STACK_TOP into %rsi, pop entry into %rdi, then jump to enter_user
*/
start_user:
    pop %rsi    /* user stack top */
    pop %rdi    /* entry point */
    jmp enter_user

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

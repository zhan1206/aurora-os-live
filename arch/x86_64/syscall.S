    .text
    .global syscall_entry
    .global syscall_return_point
    .type syscall_entry,@function

.extern syscall_trap

/* syscall_entry: entry point for SYSCALL (from user) */
syscall_entry:
/* swapgs to use kernel GS base if needed */
swapgs

/*
 * BUGFIX: Switch to per-CPU kernel stack before pushing the trapframe.
 * The user-controlled RSP must not be used as the kernel stack, as it
 * allows user-space to write arbitrary data into kernel memory.
 *
 * After swapgs, GS points to the per-CPU cpu_data struct.
 *   GS:192 = kernel_stack (uint64_t)
 *   GS:200 = user_rsp_save (uint64_t)
 *
 * We save the user RSP, then load the kernel RSP before pushing.
 * Before returning to user mode, we restore the user RSP.
 */
mov %rsp, %gs:200    /* save user RSP */
mov %gs:192, %rsp    /* load kernel stack pointer */

/* save registers into a trapframe on kernel stack (order must be consistent with struct) */
push %r15
push %r14
push %r13
push %r12
push %r11
push %r10
push %r9
push %r8
push %rsi
push %rdi
push %rdx
push %rcx
push %rax

/* rdi = pointer to trapframe (current rsp) */
mov %rsp, %rdi
/* call C handler which will set return value in the trapframe->rax */
call syscall_trap

/* syscall_return_point: fork children return here to go through normal return */
syscall_return_point:

/* restore registers (pop in reverse) */
pop %rax
pop %rcx
pop %rdx
pop %rdi
pop %rsi
pop %r8
pop %r9
pop %r10
pop %r11
pop %r12
pop %r13
pop %r14
pop %r15

/* Restore user RSP before returning to user mode */
mov %gs:200, %rsp

/* swap back */
swapgs

/* return to user via sysretq (rax already contains return value)
   RCX and R11 hold user RIP and RFLAGS as restored above
*/
sysretq

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

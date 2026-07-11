/*
 * arch/aarch64/context.S - AArch64 context switch
 *
 * Saves and restores the full register set for context switching.
 * For user tasks, also saves/restores ELR_EL1 and SPSR_EL1.
 *
 * Register layout on stack (top to bottom):
 *   x0-x30, sp, elr_el1, spsr_el1
 *   (34 registers × 8 bytes = 272 bytes)
 *
 * context_switch(old_sp_ptr, new_sp):
 *   x0 = pointer to uint64_t where old SP is stored
 *   x1 = new stack pointer to switch to
 */
    .text
    .global context_switch
    .type context_switch, @function

context_switch:
    /* Save all general-purpose registers */
    sub sp, sp, #272

    stp x0,  x1,  [sp, #0]
    stp x2,  x3,  [sp, #16]
    stp x4,  x5,  [sp, #32]
    stp x6,  x7,  [sp, #48]
    stp x8,  x9,  [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    stp x30, xzr, [sp, #240]     /* x30 (LR) + padding */

    /* Save ELR_EL1 and SPSR_EL1 */
    mrs x9, elr_el1
    mrs x10, spsr_el1
    stp x9, x10, [sp, #256]

    /* Save current stack pointer into *old_sp_ptr (x0) */
    mov x9, sp
    str x9, [x0]

    /* Load new stack pointer */
    mov sp, x1

    /* Restore ELR_EL1 and SPSR_EL1 */
    ldp x9, x10, [sp, #256]
    msr elr_el1, x9
    msr spsr_el1, x10

    /* Restore all general-purpose registers */
    ldp x2,  x3,  [sp, #16]
    ldp x4,  x5,  [sp, #32]
    ldp x6,  x7,  [sp, #48]
    ldp x8,  x9,  [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldp x30, xzr, [sp, #240]

    /* Restore x0 and x1 last (they hold return values) */
    ldp x0,  x1,  [sp, #0]

    add sp, sp, #272
    ret

    .section .note.GNU-stack,"",@progbits
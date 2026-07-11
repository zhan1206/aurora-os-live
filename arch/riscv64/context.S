/*
 * arch/riscv64/context.S - RISC-V 64-bit context switch
 *
 * Saves and restores the full register set for context switching.
 * Register layout on stack (top to bottom):
 *   ra, sp, gp, tp, t0-t6, a0-a7, s0-s11
 *   (31 registers × 8 bytes = 248 bytes)
 *
 * context_switch(old_sp_ptr, new_sp):
 *   a0 = pointer to uint64_t where old RSP is stored
 *   a1 = new stack pointer to switch to
 */
    .text
    .global context_switch
    .type context_switch, @function

context_switch:
    /* Save all caller-saved and callee-saved registers */
    addi sp, sp, -248

    sd ra,   0(sp)
    sd sp,   8(sp)
    sd gp,   16(sp)
    sd tp,   24(sp)
    sd t0,   32(sp)
    sd t1,   40(sp)
    sd t2,   48(sp)
    sd t3,   56(sp)
    sd t4,   64(sp)
    sd t5,   72(sp)
    sd t6,   80(sp)
    sd a0,   88(sp)
    sd a1,   96(sp)
    sd a2,   104(sp)
    sd a3,   112(sp)
    sd a4,   120(sp)
    sd a5,   128(sp)
    sd a6,   136(sp)
    sd a7,   144(sp)
    sd s0,   152(sp)
    sd s1,   160(sp)
    sd s2,   168(sp)
    sd s3,   176(sp)
    sd s4,   184(sp)
    sd s5,   192(sp)
    sd s6,   200(sp)
    sd s7,   208(sp)
    sd s8,   216(sp)
    sd s9,   224(sp)
    sd s10,  232(sp)
    sd s11,  240(sp)

    /* Save current stack pointer into *old_sp_ptr (a0) */
    /* a0 was saved above, so we use the saved a0 value */
    ld t0, 88(sp)          /* restore original a0 */
    sd sp, 0(t0)           /* save sp to *old_sp_ptr */

    /* Load new stack pointer */
    mv sp, a1

    /* Restore all registers from new stack */
    ld ra,   0(sp)
    ld gp,   16(sp)
    ld tp,   24(sp)
    ld t0,   32(sp)
    ld t1,   40(sp)
    ld t2,   48(sp)
    ld t3,   56(sp)
    ld t4,   64(sp)
    ld t5,   72(sp)
    ld t6,   80(sp)
    ld a0,   88(sp)
    ld a1,   96(sp)
    ld a2,   104(sp)
    ld a3,   112(sp)
    ld a4,   120(sp)
    ld a5,   128(sp)
    ld a6,   136(sp)
    ld a7,   144(sp)
    ld s0,   152(sp)
    ld s1,   160(sp)
    ld s2,   168(sp)
    ld s3,   176(sp)
    ld s4,   184(sp)
    ld s5,   192(sp)
    ld s6,   200(sp)
    ld s7,   208(sp)
    ld s8,   216(sp)
    ld s9,   224(sp)
    ld s10,  232(sp)
    ld s11,  240(sp)

    addi sp, sp, 248
    ret

    .section .note.GNU-stack,"",@progbits
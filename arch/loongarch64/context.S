/*
 * arch/loongarch64/context.S - LoongArch 64-bit context switch
 *
 * Saves and restores the full register set for context switching.
 * LoongArch has 32 general-purpose registers ($r0-$r31, $r0 = zero).
 *
 * Register layout on stack (top to bottom):
 *   $r1(ra), $r2(tp), $r3(sp), $r4-$r31 (a0-a7, t0-t8, s0-s8, fp)
 *   (31 registers × 8 bytes = 248 bytes)
 *
 * context_switch(old_sp_ptr, new_sp):
 *   a0 = pointer to uint64_t where old SP is stored
 *   a1 = new stack pointer to switch to
 */
    .text
    .global context_switch
    .type context_switch, @function

context_switch:
    /* Save all general-purpose registers */
    addi.d $sp, $sp, -248

    st.d $r1,  $sp, 0           /* $ra (return address) */
    st.d $r2,  $sp, 8           /* $tp (thread pointer) */
    st.d $r3,  $sp, 16          /* $sp (will be corrected) */
    st.d $r4,  $sp, 24          /* a0 */
    st.d $r5,  $sp, 32          /* a1 */
    st.d $r6,  $sp, 40          /* a2 */
    st.d $r7,  $sp, 48          /* a3 */
    st.d $r8,  $sp, 56          /* a4 */
    st.d $r9,  $sp, 64          /* a5 */
    st.d $r10, $sp, 72          /* a6 */
    st.d $r11, $sp, 80          /* a7 */
    st.d $r12, $sp, 88          /* t0 */
    st.d $r13, $sp, 96          /* t1 */
    st.d $r14, $sp, 104         /* t2 */
    st.d $r15, $sp, 112         /* t3 */
    st.d $r16, $sp, 120         /* t4 */
    st.d $r17, $sp, 128         /* t5 */
    st.d $r18, $sp, 136         /* t6 */
    st.d $r19, $sp, 144         /* t7 */
    st.d $r20, $sp, 152         /* t8 */
    st.d $r21, $sp, 160         /* reserved */
    st.d $r22, $sp, 168         /* fp (s0 / frame pointer) */
    st.d $r23, $sp, 176         /* s1 */
    st.d $r24, $sp, 184         /* s2 */
    st.d $r25, $sp, 192         /* s3 */
    st.d $r26, $sp, 200         /* s4 */
    st.d $r27, $sp, 208         /* s5 */
    st.d $r28, $sp, 216         /* s6 */
    st.d $r29, $sp, 224         /* s7 */
    st.d $r30, $sp, 232         /* s8 */
    st.d $r31, $sp, 240         /* reserved */

    /* Save correct sp value to the saved r3 slot */
    addi.d $t0, $sp, 248        /* pre-adjust sp */
    st.d $t0, $sp, 16

    /* Save current stack pointer into *old_sp_ptr (a0) */
    st.d $sp, $r4, 0            /* a0 = r4, saved at offset 24 */

    /* Load new stack pointer */
    or $sp, $r5, $zero          /* a1 = r5 */

    /* Restore all registers from new stack */
    ld.d $r1,  $sp, 0
    ld.d $r2,  $sp, 8
    /* skip sp at offset 16 */
    ld.d $r4,  $sp, 24
    ld.d $r5,  $sp, 32
    ld.d $r6,  $sp, 40
    ld.d $r7,  $sp, 48
    ld.d $r8,  $sp, 56
    ld.d $r9,  $sp, 64
    ld.d $r10, $sp, 72
    ld.d $r11, $sp, 80
    ld.d $r12, $sp, 88
    ld.d $r13, $sp, 96
    ld.d $r14, $sp, 104
    ld.d $r15, $sp, 112
    ld.d $r16, $sp, 120
    ld.d $r17, $sp, 128
    ld.d $r18, $sp, 136
    ld.d $r19, $sp, 144
    ld.d $r20, $sp, 152
    ld.d $r21, $sp, 160
    ld.d $r22, $sp, 168
    ld.d $r23, $sp, 176
    ld.d $r24, $sp, 184
    ld.d $r25, $sp, 192
    ld.d $r26, $sp, 200
    ld.d $r27, $sp, 208
    ld.d $r28, $sp, 216
    ld.d $r29, $sp, 224
    ld.d $r30, $sp, 232
    ld.d $r31, $sp, 240

    addi.d $sp, $sp, 248
    jirl $r0, $r1, 0

    .section .note.GNU-stack,"",@progbits
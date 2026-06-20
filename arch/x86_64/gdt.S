/*
 * gdt.S - Global Descriptor Table with TSS entry (FIXED)
 *
 * GDT layout (x86_64, 8-byte entries):
 *   0: null descriptor
 *   1: kernel code (DPL=0, 64-bit)
 *   2: kernel data (DPL=0)
 *   3: user code (DPL=3, 64-bit)
 *   4: user data (DPL=3)
 *   5: TSS (system segment, 16 bytes in long mode)
 *
 * TSS descriptor (16 bytes in long mode, split into low 8 + high 8):
 *   Low 8:  limit[15:0] | base[23:0] | type[3:0] | 0 | DPL[1:0] | P | limit[19:16] | AVL | 0 | 0 | G | base[31:24]
 *   High 8: base[63:32] | reserved
 *
 *   For 64-bit TSS: type=0x9 (available 64-bit TSS), DPL=0, P=1
 *   Limit = sizeof(TSS) - 1 = 103
 */

    .section .data
    .align 8

    .extern tss
    .extern tss_init

gdt_start:
    /* Entry 0: Null descriptor */
    .quad 0x0000000000000000

    /* Entry 1: Kernel code (64-bit, DPL=0)
     *   Base=0, Limit=0xFFFFF, G=1, L=1, D=0, P=1, DPL=0, type=0xA */
    .quad 0x00AF9A000000FFFF

    /* Entry 2: Kernel data (64-bit, DPL=0)
     *   Base=0, Limit=0xFFFFF, G=1, P=1, DPL=0, type=0x2, W=1 */
    .quad 0x00AF92000000FFFF

    /* Entry 3: User code (64-bit, DPL=3)
     *   Same as kernel code but DPL=3 */
    .quad 0x00AFFA000000FFFF

    /* Entry 4: User data (64-bit, DPL=3)
     *   Same as kernel data but DPL=3 */
    .quad 0x00AFF2000000FFFF

    /*
     * Entry 5: TSS descriptor (16 bytes in long mode)
     *
     * We construct it at runtime in load_gdt because the TSS address
     * is only known after linking. The GDT descriptor points to
     * gdt_start which is in .data, so we can write to it.
     *
     * Low 8 bytes: set during load_gdt
     * High 8 bytes: set during load_gdt
     */
    .quad 0  /* TSS low (will be filled) */
    .quad 0  /* TSS high (will be filled) */

gdt_end:

    .section .text
    .global load_gdt
    .type load_gdt,@function

load_gdt:
    /*
     * Build TSS descriptor dynamically.
     * TSS base = address of tss symbol.
     * TSS limit = tss_end - tss - 1 = 104 - 1 = 103 = 0x67
     *
     * TSS descriptor format (low 8 bytes):
     *   bits [15:0]  = limit[15:0]    = 0x0067
     *   bits [23:0]  = base[23:0]     = tss[23:0]
     *   bits [39:32] = base[31:24]    = tss[31:24]
     *   bits [43:40] = type            = 0x9 (64-bit TSS available)
     *   bit  [44]    = 0
     *   bits [46:45] = DPL             = 0
     *   bit  [47]    = P (present)     = 1
     *   bits [51:48] = limit[19:16]    = 0
     *   bit  [52]    = AVL
     *   bits [54:53] = 0
     *   bit  [55]    = G (granularity) = 0 (byte granularity)
     *
     * So low 8 bytes:
     *   0x0067 | (base[23:0] << 16) | (0x89 << 40) | (base[31:24] << 56)
     *   = 0x0000890000000067 | (base_low << 16) | (base_high8 << 56)
     *
     * High 8 bytes: base[63:32]
     */
    lea tss(%rip), %rax
    mov %rax, %rdx

    /* Build low 8 bytes */
    mov %eax, %ecx            /* ecx = base[31:0] */
    and $0xFFFFFF, %ecx       /* ecx = base[23:0] */
    shl $16, %rcx             /* shift to bits [39:16] */
    or  $0x67, %rcx            /* limit=0x67 */
    movabs $0x890000000000, %rsi
    or  %rsi, %rcx            /* type=0x89, P=1 */

    mov %eax, %r8d
    shr $24, %r8d             /* r8d = base[31:24] */
    shl $56, %r8              /* shift to bits [63:56] */
    or  %r8, %rcx             /* merge into low 8 bytes */

    /* Store low 8 bytes at gdt_start + 5*8 */
    lea gdt_start(%rip), %r8
    mov %rcx, 40(%r8)         /* offset 40 = entry 5 */

    /* Build high 8 bytes: base[63:32] */
    shr $32, %rdx             /* rdx = base[63:32] */
    mov %rdx, 48(%r8)         /* offset 48 = entry 5 high */

    /* Load GDT */
    lea gdt_descriptor(%rip), %rax
    lgdt (%rax)

    /* Reload segment registers */
    mov $0x10, %ax            /* kernel data segment = 2*8 */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Far return to reload CS */
    lea 1f(%rip), %rax
    pushq $0x08               /* kernel code segment */
    pushq %rax
    lretq
1:
    /* Initialize TSS (set IST stacks, load TR) */
    call tss_init

    ret

    .align 8
gdt_descriptor:
    .word gdt_end - gdt_start - 1
    .quad gdt_start

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

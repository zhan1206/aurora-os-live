/*
 * entry.S - Multiboot1 entry point for 64-bit kernel
 *
 * Uses Multiboot1 (not Multiboot2) for maximum GRUB compatibility.
 * GRUB loads us in 32-bit protected mode, we switch to long mode.
 */
.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000003   /* ALIGN + MEMINFO */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot
.align 8
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

    /* A.OUT kludge fields (unused for ELF) */
    .long 0  /* header_addr */
    .long 0  /* load_addr */
    .long 0  /* load_end_addr */
    .long 0  /* bss_end_addr */
    .long 0  /* entry_addr */

.section .text
.code32
.global start
.type start,@function
start:
    cli
    cld

    /* Save Multiboot info */
    mov %eax, %esi               /* magic */
    mov %ebx, %edi               /* multiboot_info pointer */

    /* Set up temporary 32-bit stack */
    lea stack_top, %esp

    /* Check if CPU supports CPUID */
    pushf
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax
    push %eax
    popf
    pushf
    pop %eax
    xor %ecx, %eax
    jz no_long_mode

    /* Check extended features for long mode */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode

    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx       /* LM bit */
    jz no_long_mode

    /* Set up 4-level paging: PML4[0] → PDPT[0] → PD[0] (2MB pages, 0-1GB) */
    /* Clear page tables */
    xor %eax, %eax
    mov $pml4, %edi
    mov $((4096*3)/4), %ecx
    rep stosl

    /* PML4[0] = pdpt | PRESENT | RW */
    movl $(pdpt + 0x003), pml4

    /* PDPT[0] = pd | PRESENT | RW */
    movl $(pd + 0x003), pdpt

    /* PD: 512 entries × 2MB pages = 1GB identity mapping */
    mov $pd, %edi
    mov $0x00000083, %eax        /* 2MB page, PRESENT, RW, PS */
    mov $512, %ecx
1:  stosl
    movl $0, (%edi)               /* upper 32 bits = 0 */
    add $4, %edi
    add $0x200000, %eax          /* next 2MB */
    loop 1b

    /* Enable PAE */
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    /* Load PML4 */
    mov $pml4, %eax
    mov %eax, %cr3

    /* Enable Long Mode in EFER */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    /* Enable paging */
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    /* Load 64-bit GDT */
    lgdt gdt64_desc

    /* Far jump to 64-bit code */
    ljmp $0x08, $long_mode_start

no_long_mode:
    /* Write 'ERR' to VGA */
    movl $0x0E45, 0xB8000       /* 'E' */
    movl $0x0E52, 0xB8002       /* 'R' */
    movl $0x0E52, 0xB8004       /* 'R' */
    cli
1:  hlt
    jmp 1b

.code64
long_mode_start:
    /* Set up segment registers for 64-bit mode */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Enable SSE/FPU (required for -O2 compiled code which uses SSE) */
    mov %cr0, %rax
    and $~0x4, %rax              /* Clear CR0.EM (Emulation) */
    or  $0x2, %rax               /* Set CR0.MP (Monitor Coprocessor) */
    mov %rax, %cr0
    mov %cr4, %rax
    or  $0x600, %rax             /* Set CR4.OSFXSR | CR4.OSXMMEXCPT */
    mov %rax, %cr4
    fninit                        /* Initialize x87 FPU */

    /* Set up 64-bit stack */
    movabs $stack_top, %rsp

    /* Zero-extend saved 32-bit values */
    mov %esi, %esi               /* magic */
    mov %ebx, %ebx               /* multiboot_info (FIXED: %edi was clobbered in 32-bit code) */

    /* Pass arguments to kernel_main */
    mov %rsi, %rdi               /* magic */
    mov %rbx, %rsi               /* multiboot info (FIXED: was %rdi which was overwritten) */

    call kernel_main

    cli
1:  hlt
    jmp 1b

.section .data
.align 4096
pml4:
    .space 4096
pdpt:
    .space 4096
pd:
    .space 4096

.align 16
gdt64:
    .quad 0                      /* null */
    .quad 0x00AF9A000000FFFF     /* kernel code, 64-bit */
    .quad 0x00AF92000000FFFF     /* kernel data */
gdt64_end:

gdt64_desc:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.section .bss
.align 16
stack_bottom:
    .space 32768
stack_top:

.global bss_start, bss_end
bss_start:
bss_end:

    /* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits

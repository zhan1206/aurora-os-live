/*
 * smp.c - SMP initialization and management
 *
 * Architecture:
 *   1. BSP parses ACPI MADT table to discover APIC IDs of all CPUs.
 *   2. BSP allocates per-CPU stacks and sets up trampoline code at 0x8000.
 *   3. BSP sends INIT-SIPI-SIPI to each AP.
 *   4. AP starts in real mode at 0x8000, transitions to long mode,
 *      initializes its own LAPIC, GDT, TSS, and enters the scheduler.
 *   5. BSP waits for each AP to signal online via atomic counter.
 *
 * The trampoline code is assembled at runtime (using machine code bytes)
 * because we're in a freestanding environment without an assembler for
 * 16-bit real mode code. The trampoline:
 *   - Loads a temporary GDT (16-bit + 32-bit + 64-bit segments)
 *   - Enters protected mode (32-bit)
 *   - Enables PAE and long mode
 *   - Loads the kernel's PML4 (passed via a data field)
 *   - Jumps to 64-bit AP entry point
 *
 * Spinlock: Upgraded from CLI/STI to atomic lock cmpxchg with pause.
 * The old spinlock_t in mem.h is now a typedef for the new spinlock.
 */

#include "smp.h"
#include "apic.h"
#include "include/log.h"
#include "include/portio.h"
#include "mem.h"
#include "pagetable.h"
#include <stdint.h>
#include <string.h>

/* ================================================================
 * Global state
 * ================================================================ */
struct cpu_data cpu_data[MAX_CPUS];
int num_cpus = 0;

/* Per-CPU data pointer accessible via GS segment */
/* GS base is set to &cpu_data[cpu_id] during AP initialization */

/* Atomic counter for AP startup synchronization */
static volatile uint32_t ap_online_count = 0;

/* Spinlock for SMP initialization */
static spinlock_t smp_init_lock;
static int smp_initialized = 0;

/* ================================================================
 * Forward declarations
 * ================================================================ */
static void smp_init_percpu(int cpu_id);
extern void ap_entry(void);

/* ================================================================
 * Trampoline code (loaded at TRAMPOLINE_ADDR = 0x8000 physical)
 *
 * The trampoline runs in 16-bit real mode, transitions to 32-bit
 * protected mode, then to 64-bit long mode.
 *
 * Layout at 0x8000:
 *   [0x0000] 16-bit real-mode startup code
 *   [0x0100] 32-bit protected-mode code (GDT, CR0, CR4, EFER setup)
 *   [0x0200] 64-bit long-mode code (load PML4, jump to ap_entry)
 *   [0x0300] Temporary GDT (null, 16-bit code, 32-bit code, 64-bit code)
 *   [0x03F0] Data fields: PML4 pointer, stack pointer, cpu_id, entry point
 * ================================================================ */

/*
 * Build the trampoline code as a byte array.
 * This is hand-assembled x86 machine code because the trampoline
 * starts in 16-bit real mode, which the compiler can't target.
 */

/* Offsets for data fields in trampoline page */
#define TRAMP_DATA_PML4     0x03F0
#define TRAMP_DATA_STACK    0x03F8
#define TRAMP_DATA_CPU_ID   0x03FC
#define TRAMP_DATA_ENTRY    0x03F0  /* same as PML4 (only one needed at a time) */

static const uint8_t trampoline_code[] __attribute__((unused)) = {
    /* ============================================
     * Offset 0x0000: 16-bit real mode code
     * ============================================ */
    /* Disable interrupts */
    0xFA,                         /* cli */
    /* Clear direction flag */
    0xFC,                         /* cld */
    /* Set DS to 0 */
    0x31, 0xC0,                   /* xor %ax, %ax */
    0x8E, 0xD8,                   /* mov %ax, %ds */
    0x8E, 0xC0,                   /* mov %ax, %es */
    0x8E, 0xD0,                   /* mov %ax, %ss */
    /* Load temporary GDT pointer (located at 0x8000 + 0x0300) */
    /* lgdt [gdt_desc] */
    0x66, 0x0F, 0x01, 0x16,       /* lgdtl [xxxx] */
    0x00, 0x03,                   /* offset 0x0300 (relative to DS=0) */
    0x00, 0x00,                   /* base = 0x8000, so [0x8300] */
    /* Actually, lgdt in real mode: the offset in the GDT descriptor
     * is the linear address. We need to put the right value in the
     * GDT descriptor. Let's use a different approach: set the GDT
     * descriptor at 0x83F0 with the correct linear address. */
    /* Let's just hardcode the lgdt with a 6-byte descriptor at 0x83F8 */
    /* We'll pre-fill the descriptor at runtime. Skip this for now. */

    /* Enable A20 line via keyboard controller */
    0xE4,                         /* in $0x64, %al */
    0xA8, 0x02,                   /* test $2, %al */
    0x75, 0xFA,                   /* jnz -6 (wait for input buffer empty) */
    0xB0, 0xD1,                   /* mov $0xD1, %al */
    0xE6, 0x64,                   /* out %al, $0x64 */
    0xE4, 0x64,                   /* in $0x64, %al */
    0xA8, 0x02,                   /* test $2, %al */
    0x75, 0xFA,                   /* jnz -6 */
    0xB0, 0xDF,                   /* mov $0xDF, %al */
    0xE6, 0x60,                   /* out %al, $0x60 */
    0xE4, 0x64,                   /* in $0x64, %al */
    0xA8, 0x02,                   /* test $2, %al */
    0x75, 0xFA,                   /* jnz -6 */

    /* Load GDT descriptor (linear address = 0x8300, limit = 0x0027) */
    /* The GDT descriptor is at 0x83F8, which we fill at runtime */
    0x0F, 0x01, 0x16,             /* lgdt [0x83F8] */
    0xF8, 0x83,

    /* Set PE bit in CR0 */
    0x0F, 0x20, 0xC0,             /* mov %cr0, %eax */
    0x66, 0x83, 0xC8, 0x01,       /* or $1, %eax */
    0x0F, 0x22, 0xC0,             /* mov %eax, %cr0 */

    /* Far jump to 32-bit code segment (selector 0x10) */
    0x66, 0xEA,                   /* ljmp $0x0010, $prot32_start */
    0x00, 0x01, 0x00, 0x00,       /* offset = 0x0100 */
    0x10, 0x00,                   /* selector = 0x10 */

    /*
     * PAD to 0x0100
     */
};

/* We need to build the trampoline dynamically at runtime because
 * the GDT descriptor contains the linear address and the 64-bit
 * entry address varies. Let's use a simpler approach: write the
 * trampoline as a function that gets compiled with the kernel,
 * and just copy it to 0x8000. But it needs to be 16-bit code...
 *
 * Best approach: use a minimal 16-bit trampoline in raw bytes,
 * with the variable parts (GDT base, PML4, stack, entry) filled
 * at runtime. The rest of the trampoline is static.
 *
 * Actually, let's take an even simpler approach: the trampoline
 * code is small enough that we can write it all in C as inline
 * data, with the 16-bit part handling the mode switch and the
 * 64-bit part being a simple jump to the C entry point.
 */

/*
 * Build and copy the trampoline to physical address 0x8000.
 * The trampoline does:
 *   1. 16-bit real mode: disable interrupts, load GDT, enable PE, jump to 32-bit
 *   2. 32-bit protected mode: set up segments, enable PAE, enable long mode,
 *      load PML4, enable paging, jump to 64-bit
 *   3. 64-bit long mode: set up GDT, IDT, LAPIC, GS base, jump to ap_entry
 *
 * Variable fields filled at runtime:
 *   0x03F0: PML4 physical address (8 bytes)
 *   0x03F8: Stack pointer (8 bytes)   [unused: AP uses its own stack]
 *   0x03F8: GDT descriptor (6 bytes: limit 2 + base 4)
 *   0x83FE: 64-bit entry point (4 bytes offset from 0x8000)
 */
static void build_trampoline(uint64_t pml4_phys) {
    uint8_t *tramp = (uint8_t *)TRAMPOLINE_ADDR;

    /* Fill with NOPs first */
    memset(tramp, 0x90, TRAMPOLINE_SIZE);

    /* ============================================
     * 16-bit real mode code at offset 0x00
     * ============================================ */
    int off = 0;

    /* cli */
    tramp[off++] = 0xFA;
    /* cld */
    tramp[off++] = 0xFC;
    /* xor %ax, %ax */
    tramp[off++] = 0x31; tramp[off++] = 0xC0;
    /* mov %ax, %ds */
    tramp[off++] = 0x8E; tramp[off++] = 0xD8;
    /* mov %ax, %es */
    tramp[off++] = 0x8E; tramp[off++] = 0xC0;
    /* mov %ax, %ss */
    tramp[off++] = 0x8E; tramp[off++] = 0xD0;

    /* Enable A20 via fast method (port 0x92) */
    tramp[off++] = 0xE4; tramp[off++] = 0x92;  /* in $0x92, %al */
    tramp[off++] = 0x0C; tramp[off++] = 0x02;  /* or $2, %al */
    tramp[off++] = 0xE6; tramp[off++] = 0x92;  /* out %al, $0x92 */

    /* Load GDT descriptor: lgdt [cs:0x83F8] — 0x66 0x0F 0x01 0x16 0xF8 0x83 */
    /* Actually, in 16-bit mode: 0F 01 /2 = lgdt m16&32 */
    tramp[off++] = 0x0F; tramp[off++] = 0x01;
    tramp[off++] = 0x16;  /* mod=00, r/m=110 (direct address) */
    /* 16-bit address of GDT descriptor: 0x83F8 (relative to DS=0, CS=0x8000) */
    tramp[off++] = 0xF8; tramp[off++] = 0x83;

    /* Set PE bit in CR0 */
    tramp[off++] = 0x0F; tramp[off++] = 0x20; tramp[off++] = 0xC0;  /* mov %cr0, %eax */
    tramp[off++] = 0x66; tramp[off++] = 0x83; tramp[off++] = 0xC8; tramp[off++] = 0x01;  /* or $1, %eax */
    tramp[off++] = 0x0F; tramp[off++] = 0x22; tramp[off++] = 0xC0;  /* mov %eax, %cr0 */

    /* ljmp $0x0010, $prot32 (far jump to 32-bit code selector)
     * In 16-bit mode with 32-bit operand prefix: 66 EA oooooooo ssss */
    tramp[off++] = 0x66; tramp[off++] = 0xEA;
    /* offset = 0x0100 (entry at 0x8100) */
    tramp[off++] = 0x00; tramp[off++] = 0x01; tramp[off++] = 0x00; tramp[off++] = 0x00;
    /* selector = 0x0010 (32-bit code) */
    tramp[off++] = 0x10; tramp[off++] = 0x00;

    /* Pad to 0x0100 */
    while (off < 0x0100) tramp[off++] = 0x90;

    /* ============================================
     * 32-bit protected mode code at offset 0x0100
     * ============================================ */
    off = 0x0100;

    /* Load data segments with 32-bit data selector (0x18) */
    tramp[off++] = 0xB8; tramp[off++] = 0x18; tramp[off++] = 0x00; tramp[off++] = 0x00; tramp[off++] = 0x00;  /* mov $0x18, %eax */
    tramp[off++] = 0x8E; tramp[off++] = 0xD8;  /* mov %ax, %ds */
    tramp[off++] = 0x8E; tramp[off++] = 0xC0;  /* mov %ax, %es */
    tramp[off++] = 0x8E; tramp[off++] = 0xD0;  /* mov %ax, %ss */

    /* Set PAE bit in CR4 */
    tramp[off++] = 0x0F; tramp[off++] = 0x20; tramp[off++] = 0xE0;  /* mov %cr4, %eax */
    tramp[off++] = 0x83; tramp[off++] = 0xC8; tramp[off++] = 0x20;  /* or $0x20, %eax */
    tramp[off++] = 0x0F; tramp[off++] = 0x22; tramp[off++] = 0xE0;  /* mov %eax, %cr4 */

    /* Load PML4 into CR3 (read from 0x83F0) */
    tramp[off++] = 0xA1;  /* movl (0x83F0), %eax */
    tramp[off++] = 0xF0; tramp[off++] = 0x83; tramp[off++] = 0x00; tramp[off++] = 0x00;
    tramp[off++] = 0x0F; tramp[off++] = 0x22; tramp[off++] = 0xD8;  /* mov %eax, %cr3 */

    /* Set LME bit in EFER MSR */
    tramp[off++] = 0xB9;  /* mov $0xC0000080, %ecx */
    tramp[off++] = 0x80; tramp[off++] = 0x00; tramp[off++] = 0x00; tramp[off++] = 0xC0;
    tramp[off++] = 0x0F; tramp[off++] = 0x32;  /* rdmsr */
    tramp[off++] = 0x0D; tramp[off++] = 0x00; tramp[off++] = 0x01; tramp[off++] = 0x00; tramp[off++] = 0x00;  /* or $0x100, %eax */
    tramp[off++] = 0x0F; tramp[off++] = 0x30;  /* wrmsr */

    /* Enable paging (set PG bit in CR0) */
    tramp[off++] = 0x0F; tramp[off++] = 0x20; tramp[off++] = 0xC0;  /* mov %cr0, %eax */
    tramp[off++] = 0x0D;  /* or $0x80000000, %eax */
    tramp[off++] = 0x00; tramp[off++] = 0x00; tramp[off++] = 0x00; tramp[off++] = 0x80;
    tramp[off++] = 0x0F; tramp[off++] = 0x22; tramp[off++] = 0xC0;  /* mov %eax, %cr0 */

    /* Far jump to 64-bit code (selector 0x20 = 64-bit code) */
    /* In 32-bit mode: 0xEA = ljmp ptr16:32; with 0x66 prefix = 16:32 */
    tramp[off++] = 0xEA;
    tramp[off++] = 0x00; tramp[off++] = 0x02; tramp[off++] = 0x00; tramp[off++] = 0x00;  /* offset = 0x0200 */
    tramp[off++] = 0x20; tramp[off++] = 0x00;  /* selector = 0x20 */

    /* Pad to 0x0200 */
    while (off < 0x0200) tramp[off++] = 0x90;

    /* ============================================
     * 64-bit long mode code at offset 0x0200
     * ============================================ */
    off = 0x0200;

    /* Load 64-bit entry point address from 0x83F0 and jump to it.
     * 0x83F0 contains the 8-byte physical address of ap_entry.
     *
     * In 64-bit mode: mov 0x83F0(%rip), %rax ; jmp *%rax
     * But we need RIP-relative addressing, and the trampoline is at
     * 0x8000 physical. In 64-bit mode, virtual = physical (identity-mapped).
     *
     * movabs 0x83F0, %rax: 48 A1 F0 83 00 00 00 00 00 00
     */
    tramp[off++] = 0x48; tramp[off++] = 0xA1;
    tramp[off++] = 0xF0; tramp[off++] = 0x83; tramp[off++] = 0x00; tramp[off++] = 0x00;
    tramp[off++] = 0x00; tramp[off++] = 0x00; tramp[off++] = 0x00; tramp[off++] = 0x00;
    tramp[off++] = 0xFF; tramp[off++] = 0xE0;  /* jmp *%rax */

    /* Pad to 0x0300 */
    while (off < 0x0300) tramp[off++] = 0x90;

    /* ============================================
     * Temporary GDT at offset 0x0300
     * (6 entries × 8 bytes = 48 bytes)
     * ============================================ */
    off = 0x0300;
    /* Entry 0: null descriptor */
    for (int i = 0; i < 8; i++) tramp[off++] = 0x00;
    /* Entry 1: 16-bit code (selector 0x08) — unused by trampoline */
    /* Base=0, Limit=0xFFFF, G=0, D=0, P=1, DPL=0, S=1, Type=0xA (code, exec/read) */
    tramp[off++] = 0xFF; tramp[off++] = 0xFF;  /* limit 0:15 */
    tramp[off++] = 0x00; tramp[off++] = 0x00;  /* base 0:15 */
    tramp[off++] = 0x00;                        /* base 16:23 */
    tramp[off++] = 0x9A;                        /* access: P=1, DPL=0, S=1, Type=0xA */
    tramp[off++] = 0x00;                        /* flags + limit 16:19 */
    tramp[off++] = 0x00;                        /* base 24:31 */
    /* Entry 2: 32-bit code (selector 0x10) */
    /* Base=0, Limit=0xFFFFF, G=1, D=1, P=1, DPL=0, S=1, Type=0xA */
    tramp[off++] = 0xFF; tramp[off++] = 0xFF;  /* limit 0:15 */
    tramp[off++] = 0x00; tramp[off++] = 0x00;  /* base 0:15 */
    tramp[off++] = 0x00;                        /* base 16:23 */
    tramp[off++] = 0x9A;                        /* access */
    tramp[off++] = 0xCF;                        /* G=1, D=1, limit 16:19 = F */
    tramp[off++] = 0x00;                        /* base 24:31 */
    /* Entry 3: 32-bit data (selector 0x18) */
    tramp[off++] = 0xFF; tramp[off++] = 0xFF;  /* limit 0:15 */
    tramp[off++] = 0x00; tramp[off++] = 0x00;  /* base 0:15 */
    tramp[off++] = 0x00;                        /* base 16:23 */
    tramp[off++] = 0x92;                        /* access: P=1, DPL=0, S=1, Type=0x2 (data, r/w) */
    tramp[off++] = 0xCF;                        /* G=1, D=1, limit 16:19 = F */
    tramp[off++] = 0x00;                        /* base 24:31 */
    /* Entry 4: 64-bit code (selector 0x20) */
    /* Base=0, Limit=0, G=0, L=1, D=0, P=1, DPL=0, S=1, Type=0xA */
    tramp[off++] = 0x00; tramp[off++] = 0x00;  /* limit 0:15 (ignored in 64-bit) */
    tramp[off++] = 0x00; tramp[off++] = 0x00;  /* base 0:15 */
    tramp[off++] = 0x00;                        /* base 16:23 */
    tramp[off++] = 0x9A;                        /* access */
    tramp[off++] = 0x20;                        /* L=1, D=0 */
    tramp[off++] = 0x00;                        /* base 24:31 */
    /* Entry 5: 64-bit data (selector 0x28) */
    tramp[off++] = 0x00; tramp[off++] = 0x00;  /* limit (ignored) */
    tramp[off++] = 0x00; tramp[off++] = 0x00;  /* base 0:15 */
    tramp[off++] = 0x00;                        /* base 16:23 */
    tramp[off++] = 0x92;                        /* access */
    tramp[off++] = 0x00;                        /* L=0, D=0 */
    tramp[off++] = 0x00;                        /* base 24:31 */

    /* Pad to 0x03F0 */
    while (off < 0x03F0) tramp[off++] = 0x00;

    /* ============================================
     * Data fields at offset 0x03F0
     * ============================================ */
    /* 0x03F0: PML4 physical address (8 bytes) */
    *(uint64_t *)(tramp + 0x03F0) = pml4_phys;

    /* 0x03F8: GDT descriptor (limit 2 bytes + base 4 bytes)
     * Base = 0x8000 + 0x0300 = 0x8300 (linear address of GDT)
     * Limit = 6 * 8 - 1 = 47 = 0x2F */
    tramp[0x03F8] = 0x2F; tramp[0x03F9] = 0x00;  /* limit = 0x002F */
    /* Base = 0x00008300 */
    tramp[0x03FA] = 0x00; tramp[0x03FB] = 0x83;
    tramp[0x03FC] = 0x00; tramp[0x03FD] = 0x00;
}

/* ================================================================
 * ACPI MADT parsing
 *
 * ACPI tables are located by searching for the RSDP (Root System
 * Description Pointer), then finding the RSDT/XSDT, then the MADT.
 *
 * RSDP signature: "RSD PTR " (8 bytes)
 * RSDT/XSDT signature: "RSDT" / "XSDT" (4 bytes)
 * MADT signature: "APIC" (4 bytes)
 * ================================================================ */

/* ACPI SDT header */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* RSDP */
struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* Extended fields (ACPI 2.0+) */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* RSDT/XSDT entry */
struct acpi_rsdt {
    struct acpi_sdt_header h;
    uint32_t entries[];  /* array of 32-bit pointers to other SDTs */
} __attribute__((packed));

struct acpi_xsdt {
    struct acpi_sdt_header h;
    uint64_t entries[];  /* array of 64-bit pointers to other SDTs */
} __attribute__((packed));

/* MADT entry types */
#define MADT_TYPE_LAPIC      0x00
#define MADT_TYPE_IOAPIC     0x01
#define MADT_TYPE_ISO        0x02
#define MADT_TYPE_NMI        0x04
#define MADT_TYPE_LAPIC_OVERRIDE 0x05

/* MADT header */
struct acpi_madt {
    struct acpi_sdt_header h;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t  entries[];  /* variable-length entries */
} __attribute__((packed));

/* MADT entry header */
struct madt_entry_hdr {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

/* MADT Local APIC entry */
struct madt_lapic {
    struct madt_entry_hdr h;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;  /* bit 0 = enabled */
} __attribute__((packed));

/* MADT I/O APIC entry */
struct madt_ioapic {
    struct madt_entry_hdr h;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed));

/* ================================================================
 * RSDP search: scan physical memory for the RSDP signature
 * ================================================================ */

/*
 * Find RSDP by scanning the EBDA and BIOS areas.
 * For Multiboot, the RSDP may be passed in the boot info.
 * We search:
 *   - First 1KB of EBDA (Extended BIOS Data Area)
 *   - 0x000E0000 - 0x000FFFFF (BIOS ROM area)
 */
static struct acpi_rsdp *find_rsdp(void *mb_info) {
    /* Try to get RSDP from Multiboot2 tags first */
    if (mb_info) {
        /* Check for Multiboot2 ACPI tags */
        struct mb2_tag {
            uint32_t type;
            uint32_t size;
        };

        uint32_t *ptr = (uint32_t *)mb_info;
        uint32_t total_size = ptr[0];

        for (uint32_t offset = 8; offset < total_size; ) {
            struct mb2_tag *tag = (struct mb2_tag *)((uint8_t *)mb_info + offset);
            if (tag->type == 0) break;  /* end tag */

            /* Multiboot2 ACPI v1 RSDP tag = 14 */
            if (tag->type == 14) {
                struct acpi_rsdp *rsdp = (struct acpi_rsdp *)((uint8_t *)mb_info + offset + 8);
                log_printf(LOG_LEVEL_INFO, "smp: found RSDP via Multiboot2 tag\n");
                return rsdp;
            }
            /* Multiboot2 ACPI v2 RSDP tag = 15 */
            if (tag->type == 15) {
                struct acpi_rsdp *rsdp = (struct acpi_rsdp *)((uint8_t *)mb_info + offset + 8);
                log_printf(LOG_LEVEL_INFO, "smp: found RSDP via Multiboot2 ACPI v2 tag\n");
                return rsdp;
            }

            offset += ((tag->size + 7) & ~7U);
        }
    }

    /* Fallback: scan BIOS memory areas for RSDP */
    /* The RSDP is on a 16-byte boundary in the EBDA or 0xE0000-0xFFFFF */
    uint32_t scan_start = 0x000E0000;
    uint32_t scan_end   = 0x00100000;

    for (uint32_t addr = scan_start; addr < scan_end; addr += 16) {
        if (addr >= KERNEL_PHYS_MAX) break;  /* beyond identity-mapped range */
        const char *sig = (const char *)(uintptr_t)addr;
        if (sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' &&
            sig[3] == ' ' && sig[4] == 'P' && sig[5] == 'T' &&
            sig[6] == 'R' && sig[7] == ' ') {
            return (struct acpi_rsdp *)(uintptr_t)addr;
        }
    }

    return NULL;
}

/*
 * Validate ACPI checksum
 */
static int acpi_checksum(void *table, uint32_t length) {
    uint8_t sum = 0;
    uint8_t *ptr = (uint8_t *)table;
    for (uint32_t i = 0; i < length; i++) {
        sum += ptr[i];
    }
    return sum == 0;
}

/*
 * Find the MADT table from the RSDP.
 * Also extracts the I/O APIC base address.
 */
static struct acpi_madt *find_madt(struct acpi_rsdp *rsdp, uint64_t *ioapic_base_out) {
    (void)ioapic_base_out;  /* reserved for future use */
    if (!rsdp) return NULL;

    /* Check RSDP checksum */
    if (!acpi_checksum(rsdp, 20)) {
        log_printf(LOG_LEVEL_WARN, "smp: RSDP checksum failed\n");
        return NULL;
    }

    /* Use XSDT if available (ACPI 2.0+) */
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        struct acpi_xsdt *xsdt = (struct acpi_xsdt *)(uintptr_t)rsdp->xsdt_address;
        if (!acpi_checksum(xsdt, xsdt->h.length)) {
            log_printf(LOG_LEVEL_WARN, "smp: XSDT checksum failed\n");
            return NULL;
        }

        int num_entries = ((int)xsdt->h.length - (int)sizeof(struct acpi_sdt_header)) / 8;
        log_printf(LOG_LEVEL_DEBUG, "smp: XSDT at %p, %d entries\n",
                   (void *)xsdt, num_entries);

        for (int i = 0; i < num_entries; i++) {
            uint64_t entry = xsdt->entries[i];
            if (entry >= KERNEL_PHYS_MAX) continue;
            struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)entry;

            if (hdr->signature[0] == 'A' && hdr->signature[1] == 'P' &&
                hdr->signature[2] == 'I' && hdr->signature[3] == 'C') {
                if (!acpi_checksum(hdr, hdr->length)) {
                    log_printf(LOG_LEVEL_WARN, "smp: MADT checksum failed\n");
                    continue;
                }
                return (struct acpi_madt *)hdr;
            }
        }
        return NULL;
    }

    /* Fallback to RSDT (ACPI 1.0) */
    if (rsdp->rsdt_address) {
        struct acpi_rsdt *rsdt = (struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_address;
        if (!acpi_checksum(rsdt, rsdt->h.length)) {
            log_printf(LOG_LEVEL_WARN, "smp: RSDT checksum failed\n");
            return NULL;
        }

        int num_entries = ((int)rsdt->h.length - (int)sizeof(struct acpi_sdt_header)) / 4;
        log_printf(LOG_LEVEL_DEBUG, "smp: RSDT at %p, %d entries\n",
                   (void *)rsdt, num_entries);

        struct acpi_madt *madt = NULL;
        for (int i = 0; i < num_entries; i++) {
            uint32_t entry = rsdt->entries[i];
            if (entry >= KERNEL_PHYS_MAX) continue;
            struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)entry;

            if (hdr->signature[0] == 'A' && hdr->signature[1] == 'P' &&
                hdr->signature[2] == 'I' && hdr->signature[3] == 'C') {
                if (!acpi_checksum(hdr, hdr->length)) {
                    log_printf(LOG_LEVEL_WARN, "smp: MADT checksum failed\n");
                    continue;
                }
                madt = (struct acpi_madt *)hdr;
            }
        }
        return madt;
    }

    return NULL;
}

/* ================================================================
 * AP entry point (called from trampoline in 64-bit mode)
 *
 * The trampoline jumps here with:
 *   - Identity-mapped paging enabled (kernel PML4)
 *   - 64-bit mode active
 *   - GDT from the trampoline (temporary)
 *   - GS base not yet set
 *
 * This function:
 *   1. Sets up this CPU's entry in cpu_data
 *   2. Sets up a proper per-CPU GDT and TSS
 *   3. Initializes the local APIC
 *   4. Sets GS base to point to this CPU's cpu_data
 *   5. Jumps into the scheduler
 * ================================================================ */

void ap_entry(void) {
    int cpu_id = -1;

    /* Find our CPU ID by checking which cpu_data slot we belong to.
     * The BSP increments ap_online_count after setting up each slot,
     * so we use an atomic increment to claim our slot. */
    cpu_id = (int)__sync_fetch_and_add(&ap_online_count, 1);

    /* Check if we already have a slot (unlikely) */
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) {
        log_printf(LOG_LEVEL_ERR, "smp: AP got invalid cpu_id=%d, halting\n", cpu_id);
        for (;;) asm volatile ("cli; hlt");
    }

    /* Initialize our cpu_data */
    struct cpu_data *cpu = &cpu_data[cpu_id];
    memset(cpu, 0, sizeof(*cpu));
    cpu->cpu_id = cpu_id;
    cpu->lapic_id = (int)lapic_read_id();
    cpu->online = 1;
    cpu->tsc_freq = 0;  /* will be calibrated later */

    /* Set GS base to point to our cpu_data */
    /* wrmsr IA32_GS_BASE (0xC0000101) */
    uint64_t gs_base = (uint64_t)(uintptr_t)cpu;
    uint32_t gs_lo = (uint32_t)(gs_base & 0xFFFFFFFF);
    uint32_t gs_hi = (uint32_t)(gs_base >> 32);
    asm volatile (
        "mov $0xC0000101, %%ecx\n"
        "mov %0, %%eax\n"
        "mov %1, %%edx\n"
        "wrmsr\n"
        :: "r"(gs_lo), "r"(gs_hi) : "eax", "ecx", "edx", "memory"
    );

    /* Also set IA32_KERNEL_GS_BASE (0xC0000102) for swapgs */
    asm volatile (
        "mov $0xC0000102, %%ecx\n"
        "mov %0, %%eax\n"
        "mov %1, %%edx\n"
        "wrmsr\n"
        :: "r"(gs_lo), "r"(gs_hi) : "eax", "ecx", "edx", "memory"
    );

    /* Set up our own GDT and TSS */
    smp_init_percpu(cpu_id);

    /* Initialize LAPIC for this CPU (enables interrupts, sets SVR).
     * lapic_vbase is already mapped by the BSP; this just enables
     * the LAPIC for this specific CPU. */
    lapic_init();

    /* Initialize LAPIC timer for this CPU */
    lapic_timer_init(100);  /* 100 Hz */

    /* Enable interrupts */
    asm volatile ("sti" ::: "memory");

    log_printf(LOG_LEVEL_INFO, "smp: CPU %d (LAPIC id=%d) online\n",
               cpu_id, cpu->lapic_id);

    /* Enter the idle loop — this CPU is now ready to schedule tasks */
    /* The scheduler will pick up tasks from the per-CPU run queue */
    while (1) {
        /* Check if there's a task to run */
        if (cpu->current_task && cpu->current_task->state == TASK_READY) {
            cpu->current_task->state = TASK_RUNNING;
            /* We'd do a context switch here, but for now we just idle */
        }
        /* Halt until next interrupt */
        asm volatile ("sti; hlt" ::: "memory");
    }
}

/* ================================================================
 * smp_init_percpu: Set up per-CPU GDT and TSS
 *
 * Each CPU needs its own GDT (because the TSS descriptor is per-CPU)
 * and its own TSS (for IST stack pointers).
 * ================================================================ */

static void smp_init_percpu(int cpu_id) {
    struct cpu_data *cpu = &cpu_data[cpu_id];

    /* Initialize per-CPU TSS (zeroed by memset in ap_entry) */
    /* The TSS IST fields will be set up later when stacks are allocated */

    /* Build per-CPU GDT (copy from kernel's GDT layout, but with our TSS) */
    /* Per-CPU GDT layout:
     *   0: null
     *   1: kernel code (64-bit, DPL=0)
     *   2: kernel data (64-bit, DPL=0)
     *   3: user code (64-bit, DPL=3)
     *   4: user data (64-bit, DPL=3)
     *   5: TSS low (16 bytes for long mode TSS, split into 2 entries)
     *   6: TSS high
     */

    uint64_t *gdt = cpu->gdt;

    /* Entry 0: null */
    gdt[0] = 0;

    /* Entry 1: 64-bit kernel code (selector 0x08) */
    /* Base=0, Limit=0, L=1, D=0, P=1, DPL=0, S=1, Type=0xA */
    gdt[1] = 0x00209A0000000000ULL;

    /* Entry 2: 64-bit kernel data (selector 0x10) */
    gdt[2] = 0x0000920000000000ULL;

    /* Entry 3: 64-bit user code (selector 0x18) */
    gdt[3] = 0x0020FA0000000000ULL;

    /* Entry 4: 64-bit user data (selector 0x20) */
    gdt[4] = 0x0000F20000000000ULL;

    /* Entry 5 & 6: TSS (16 bytes)
     * TSS base = &cpu->tss[0], limit = 103 = 0x67
     * TSS descriptor format (low 8 bytes):
     *   [15:0]   = limit 15:0
     *   [23:16]  = base 15:0
     *   [31:24]  = base 23:16
     *   [39:32]  = base 31:24
     *   [43:40]  = type = 0x9 (available 64-bit TSS)
     *   [44]     = 0 (system)
     *   [45:46]  = DPL = 0
     *   [47]     = P = 1
     *   [51:48]  = limit 19:16
     *   [55:52]  = flags = 0
     *   [63:56]  = base 39:32
     */
    uint64_t tss_base = (uint64_t)(uintptr_t)cpu->tss;
    uint64_t tss_limit = 103;  /* sizeof(TSS) - 1 = 104 - 1 */

    gdt[5] = (tss_limit & 0xFFFF)
           | ((tss_base & 0xFFFF) << 16)
           | (((tss_base >> 16) & 0xFF) << 32)
           | (0x9ULL << 40)      /* type = 0x9 (available 64-bit TSS) */
           | (1ULL << 47)        /* P = 1 */
           | (((tss_limit >> 16) & 0xF) << 48)
           | (((tss_base >> 24) & 0xFF) << 56);

    gdt[6] = (tss_base >> 32);

    /* Load the per-CPU GDT using lgdt */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;

    gdtr.limit = (uint16_t)(sizeof(cpu->gdt) - 1);
    gdtr.base = (uint64_t)(uintptr_t)cpu->gdt;

    asm volatile (
        "lgdt %0\n"
        /* Reload segment registers */
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        /* Reload CS via far return */
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "m"(gdtr)
        : "rax", "memory"
    );

    /* Load TSS (selector = 5 * 8 = 0x28) */
    asm volatile ("ltr %%ax" :: "a"(0x28) : "memory");

    log_printf(LOG_LEVEL_DEBUG, "smp: CPU %d GDT+TSS initialized\n", cpu_id);
}

/* ================================================================
 * smp_init: Initialize SMP subsystem
 *
 * Called once by the BSP during kernel initialization.
 * Parses ACPI MADT to find all CPUs, sets up per-CPU data,
 * builds trampoline, and starts APs.
 * ================================================================ */

void smp_init(void *mb_info) {
    if (smp_initialized) return;
    spin_init(&smp_init_lock);
    spin_lock(&smp_init_lock);

    if (smp_initialized) {
        spin_unlock(&smp_init_lock);
        return;
    }

    log_printf(LOG_LEVEL_INFO, "smp: initializing SMP...\n");

    /* Initialize cpu_data array */
    memset(cpu_data, 0, sizeof(cpu_data));
    num_cpus = 0;

    /* Find RSDP and MADT */
    struct acpi_rsdp *rsdp = find_rsdp(mb_info);
    if (!rsdp) {
        log_printf(LOG_LEVEL_INFO, "smp: no ACPI RSDP found, single-CPU mode\n");
        num_cpus = 1;
        cpu_data[0].cpu_id = 0;
        cpu_data[0].lapic_id = (int)lapic_read_id();
        cpu_data[0].online = 1;
        cpu_data[0].current_task = NULL;
        smp_initialized = 1;
        spin_unlock(&smp_init_lock);
        return;
    }

    uint64_t ioapic_base = 0;
    struct acpi_madt *madt = find_madt(rsdp, &ioapic_base);
    if (!madt) {
        log_printf(LOG_LEVEL_INFO, "smp: no MADT found, single-CPU mode\n");
        num_cpus = 1;
        cpu_data[0].cpu_id = 0;
        cpu_data[0].lapic_id = (int)lapic_read_id();
        cpu_data[0].online = 1;
        cpu_data[0].current_task = NULL;
        smp_initialized = 1;
        spin_unlock(&smp_init_lock);
        return;
    }

    log_printf(LOG_LEVEL_INFO, "smp: MADT at %p, LAPIC base=0x%x\n",
               (void *)madt, madt->lapic_address);

    /* Parse MADT entries */
    int lapic_count = 0;
    int apic_ids[MAX_CPUS];
    uint8_t *entry = (uint8_t *)madt->entries;
    uint8_t *madt_end = (uint8_t *)madt + madt->h.length;

    while (entry < madt_end) {
        struct madt_entry_hdr *hdr = (struct madt_entry_hdr *)entry;
        if (hdr->length == 0) break;

        if (hdr->type == MADT_TYPE_LAPIC) {
            struct madt_lapic *lapic = (struct madt_lapic *)entry;
            /* Check if enabled (flags bit 0) or if it's the BSP */
            if ((lapic->flags & 1) || lapic_count == 0) {
                if (lapic_count < MAX_CPUS) {
                    apic_ids[lapic_count] = (int)lapic->apic_id;
                    /* Initialize cpu_data */
                    cpu_data[lapic_count].cpu_id = lapic_count;
                    cpu_data[lapic_count].lapic_id = (int)lapic->apic_id;
                    cpu_data[lapic_count].online = 0;
                    cpu_data[lapic_count].current_task = NULL;
                    log_printf(LOG_LEVEL_INFO, "smp: found CPU %d (LAPIC id=%d, flags=0x%x)\n",
                               lapic_count, lapic->apic_id, lapic->flags);
                    lapic_count++;
                }
            }
        } else if (hdr->type == MADT_TYPE_IOAPIC) {
            struct madt_ioapic *ioapic = (struct madt_ioapic *)entry;
            ioapic_base = ioapic->ioapic_address;
            log_printf(LOG_LEVEL_INFO, "smp: found IOAPIC id=%d at 0x%x\n",
                       ioapic->ioapic_id, ioapic->ioapic_address);
        }

        entry += hdr->length;
    }

    num_cpus = lapic_count;
    if (num_cpus == 0) {
        log_printf(LOG_LEVEL_INFO, "smp: no APs found, single-CPU mode\n");
        num_cpus = 1;
        cpu_data[0].cpu_id = 0;
        cpu_data[0].lapic_id = (int)lapic_read_id();
        cpu_data[0].online = 1;
    }

    log_printf(LOG_LEVEL_INFO, "smp: detected %d CPU(s)\n", num_cpus);

    /* If only one CPU, we're done */
    if (num_cpus <= 1) {
        cpu_data[0].online = 1;
        smp_initialized = 1;
        spin_unlock(&smp_init_lock);
        return;
    }

    /* Initialize I/O APIC */
    if (ioapic_base) {
        ioapic_init(ioapic_base);
    }

    /* Build trampoline code at 0x8000 */
    uint64_t pml4 = get_kernel_cr3();
    build_trampoline(pml4);

    /* Write the 64-bit entry point address to the trampoline data field */
    /* 0x83F0 already contains the PML4 pointer. We need to put the
     * ap_entry address at 0x83F0 too (it replaces the PML4 which is
     * already in the 32-bit code's load path). Wait — the 32-bit code
     * reads 0x83F0 for PML4, and the 64-bit code reads 0x83F0 for
     * the entry point. Since the 32-bit code runs first, we need to
     * keep PML4 at 0x83F0. After the 32-bit code loads CR3, the
     * 64-bit code reads 0x83F0 for the entry point.
     *
     * But the 64-bit code is at 0x8200 and reads from 0x83F0 using
     * movabs. After the 32-bit code loads CR3 and enables paging,
     * the 64-bit code will read from 0x83F0 (which is still identity-mapped).
     * So we need to write the entry point BEFORE the 32-bit code runs,
     * but the 32-bit code needs the PML4 at 0x83F0 too.
     *
     * Solution: Use a different offset for entry point. Let's put
     * the entry point at 0x83E8 instead, and update the 64-bit code.
     * Actually, let's restructure: the 32-bit code loads PML4 from 0x83F0.
     * Then the 64-bit code reads the entry point from 0x83F8.
     * We need to update the trampoline to read from 0x83F8 instead.
     *
     * Actually, the simplest fix: the 32-bit code reads PML4 from 0x83F0,
     * then the 64-bit code can read the entry point from 0x83F8.
     * We need to change the movabs address in the 64-bit code from 0x83F0 to 0x83F8.
     */

    /* Fix the 64-bit trampoline code to read from 0x83E8 instead of 0x83F0 */
    /* The 64-bit code at offset 0x0200 has: 48 A1 [8 bytes] FF E0 */
    /* Change the address bytes from 0x83F0 to 0x83E8 */
    uint8_t *tramp = (uint8_t *)TRAMPOLINE_ADDR;
    if (tramp[0x0200] == 0x48 && tramp[0x0201] == 0xA1) {
        tramp[0x0202] = 0xE8;  /* was 0xF0, change to 0x83E8 */
        /* 0x83E8 = E8 83 00 00 00 00 00 00 (little-endian) */
    }

    /* Write the ap_entry address at 0x83E8 (not overlapping with GDT descriptor at 0x83F8) */
    *(uint64_t *)(tramp + 0x03E8) = (uint64_t)(uintptr_t)&ap_entry;

    /* Mark BSP as online */
    cpu_data[0].online = 1;
    ap_online_count = 1;  /* BSP is already online */

    /* Set GS base for BSP */
    uint64_t gs_base = (uint64_t)(uintptr_t)&cpu_data[0];
    uint32_t gs_lo = (uint32_t)(gs_base & 0xFFFFFFFF);
    uint32_t gs_hi = (uint32_t)(gs_base >> 32);
    asm volatile (
        "mov $0xC0000101, %%ecx\n"
        "mov %0, %%eax\n"
        "mov %1, %%edx\n"
        "wrmsr\n"
        :: "r"(gs_lo), "r"(gs_hi) : "eax", "ecx", "edx", "memory"
    );
    asm volatile (
        "mov $0xC0000102, %%ecx\n"
        "mov %0, %%eax\n"
        "mov %1, %%edx\n"
        "wrmsr\n"
        :: "r"(gs_lo), "r"(gs_hi) : "eax", "ecx", "edx", "memory"
    );

    /* Start each AP */
    for (int i = 1; i < num_cpus; i++) {
        log_printf(LOG_LEVEL_INFO, "smp: starting CPU %d (LAPIC id=%d)...\n",
                   i, apic_ids[i]);

        lapic_start_ap(apic_ids[i], (uint32_t)TRAMPOLINE_ADDR);

        /* Wait for AP to come online (with timeout) */
        int timeout = 1000000;  /* ~1 second */
        while (ap_online_count <= (uint32_t)i && timeout > 0) {
            timeout--;
            /* Small delay */
            for (volatile int d = 0; d < 1000; d++) {
                asm volatile ("" ::: "memory");
            }
        }

        if (timeout <= 0) {
            log_printf(LOG_LEVEL_WARN, "smp: CPU %d failed to start (timeout)\n", i);
        }
    }

    log_printf(LOG_LEVEL_INFO, "smp: %d CPU(s) online\n", (int)ap_online_count);

    smp_sched_ready = 1;  /* Allow GS-based current_cpu_id() */
    smp_initialized = 1;
    spin_unlock(&smp_init_lock);
}

/* ================================================================
 * get_cpu_count: Return the number of detected CPUs
 * ================================================================ */

int get_cpu_count(void) {
    return num_cpus;
}

/* ================================================================
 * smp_send_ipi: Send an IPI to a specific CPU
 * ================================================================ */

void smp_send_ipi(int cpu_id, int vector) {
    if (cpu_id < 0 || cpu_id >= num_cpus) return;
    if (!cpu_data[cpu_id].online) return;

    int lapic_id = cpu_data[cpu_id].lapic_id;
    lapic_send_ipi(lapic_id, vector);
}

/* ================================================================
 * smp_tlb_shootdown: Invalidate TLB entry on all other CPUs
 *
 * When the kernel modifies page tables, it must invalidate the
 * TLB on all CPUs that might have cached the old mapping.
 * This sends IPI_TLB_VECTOR to every online CPU except self.
 * ================================================================ */

void smp_tlb_shootdown(uint64_t vaddr) {
    int my_cpu = -1;
    struct cpu_data *me = this_cpu();
    if (me) my_cpu = me->cpu_id;

    for (int i = 0; i < num_cpus; i++) {
        if (i == my_cpu) continue;
        if (!cpu_data[i].online) continue;
        smp_send_ipi(i, IPI_TLB_VECTOR);
    }

    /* We also need to invalidate our own TLB */
    asm volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/* ================================================================
 * ap_startup: Legacy trampoline entry (for external use)
 *
 * This is the function that the trampoline code jumps to.
 * It's the same as ap_entry — provided for API compatibility.
 * ================================================================ */

void ap_startup(void) {
    ap_entry();
}
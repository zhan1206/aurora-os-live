/*
 * linker.ld - AuroraOS kernel linker script
 */
ENTRY(start)

SECTIONS
{
    . = 0x100000;

    .text : {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    /*
     * .data must be page-aligned because entry.S places page tables
     * (pml4/pdpt/pd) in .data with .align 4096.
     * Without ALIGN here, the alignment directive in entry.S
     * cannot guarantee 4KiB alignment.
     */
    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.note.gnu.property)
        *(.comment)
    }
}

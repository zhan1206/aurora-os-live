/*
 * uefi.lds - Linker script for UEFI PE32+ application
 *
 * Produces a position-independent shared library that can be
 * converted to a PE32+ EFI application via objcopy.
 *
 * The UEFI firmware loads the PE image at any address; the code
 * must be position-independent.  We use -fpic and link as a
 * shared object (-shared).
 *
 * Usage:
 *   x86_64-elf-ld -nostdlib -shared -Bsymbolic -T boot/uefi.lds \
 *       build/efi_main.o -o build/efi_app.so
 *   x86_64-elf-objcopy -j .text -j .data -j .rodata -j .reloc \
 *       -j .dynsym -j .dynstr --target=efi-app-x86_64 \
 *       build/efi_app.so iso/EFI/BOOT/BOOTX64.EFI
 */
SECTIONS
{
    . = 0;
    _text_start = .;

    .text : {
        *(.text)
        *(.text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data)
        *(.data.*)
    }

    . = ALIGN(4096);
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    . = ALIGN(4096);
    .reloc : {
        *(.reloc)
        *(.rela.*)
        *(.rel.*)
    }

    .dynsym : {
        *(.dynsym)
    }

    .dynstr : {
        *(.dynstr)
    }

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.note.gnu.property)
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
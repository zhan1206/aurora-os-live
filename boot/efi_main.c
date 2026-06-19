/*
 * efi_main.c - UEFI bootloader for AuroraOS
 *
 * This is a PE32+ EFI application loaded by UEFI firmware.
 * It:
 *   1. Loads the kernel ELF from the same volume
 *   2. Collects the UEFI memory map and GOP framebuffer info
 *   3. Calls ExitBootServices() to take control of the machine
 *   4. Sets up 4-level paging identity-mapping the first 1GB
 *   5. Jumps to the kernel's kernel_main() entry point in 64-bit long mode
 *
 * The kernel is already compiled for 64-bit, so no mode switching is needed.
 * The UEFI firmware provides us with 64-bit long mode already active.
 */

#include "uefi.h"
#include "boot_info.h"

/* ================================================================
 * Minimal ELF64 definitions (for loading the kernel ELF)
 * ================================================================ */
#define ELF_MAGIC  0x464C457F  /* "\x7FELF" little-endian */
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_PHDR    6

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/* ================================================================
 * Page table constants (matching kernel's entry.S layout)
 * ================================================================ */
#define PAGE_SIZE_4K    0x1000ULL
#define PAGE_SIZE_2M    0x200000ULL
#define PTE_PRESENT     0x001ULL
#define PTE_RW          0x002ULL
#define PTE_PS          0x080ULL  /* 2MB page size */

/* Page table locations (must match the addresses used by kernel's entry.S).
 * We place them at low memory, away from the kernel (loaded at 1MB). */
#define PML4_ADDR   0x7000ULL
#define PDPT_ADDR   0x8000ULL
#define PD_ADDR     0x9000ULL

/* ================================================================
 * Known GUID instances
 * ================================================================ */
static const EFI_GUID gLoadedImageGuid =
    {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static const EFI_GUID gGopGuid =
    {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};

static const EFI_GUID gSfsGuid =
    {0x0964E5B2, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

/* ================================================================
 * Helper: Convert ASCII string to wide CHAR16 (in-place)
 * ================================================================ */
static void ascii_to_wide(CHAR16 *dst, const char *src) {
    while (*src) {
        *dst++ = (CHAR16)(unsigned char)*src++;
    }
    *dst = 0;
}

/* ================================================================
 * Helper: memset for the bootloader
 * ================================================================ */
static void zero_mem(void *buf, uint64_t size) {
    uint8_t *p = (uint8_t *)buf;
    for (uint64_t i = 0; i < size; i++) p[i] = 0;
}

/* ================================================================
 * Helper: memcpy for the bootloader
 * ================================================================ */
static void copy_mem(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < size; i++) d[i] = s[i];
}

/* ================================================================
 * Set up identity-mapped paging (4-level, first 1GB via 2MB pages)
 *
 * This matches the mapping set up by kernel/entry.S for Multiboot boot.
 * The kernel's page_table_init() reads the current CR3, so we must
 * provide a valid page table before jumping to kernel_main().
 * ================================================================ */
static void setup_identity_paging(void) {
    uint64_t *pml4 = (uint64_t *)PML4_ADDR;
    uint64_t *pdpt = (uint64_t *)PDPT_ADDR;
    uint64_t *pd   = (uint64_t *)PD_ADDR;

    zero_mem(pml4, PAGE_SIZE_4K);
    zero_mem(pdpt, PAGE_SIZE_4K);
    zero_mem(pd, PAGE_SIZE_4K);

    /* PML4[0] = pdpt | PRESENT | RW */
    pml4[0] = PDPT_ADDR | PTE_PRESENT | PTE_RW;

    /* PDPT[0] = pd | PRESENT | RW */
    pdpt[0] = PD_ADDR | PTE_PRESENT | PTE_RW;

    /* PD: 512 entries × 2MB pages = 1GB identity mapping */
    uint64_t addr = 0;
    for (int i = 0; i < 512; i++) {
        pd[i] = addr | PTE_PRESENT | PTE_RW | PTE_PS;
        addr += PAGE_SIZE_2M;
    }

    /* Load PML4 into CR3 */
    asm volatile (
        "mov %0, %%cr3\n"
        :
        : "r"(PML4_ADDR)
        : "memory"
    );
}

/* ================================================================
 * UEFI entry point
 * ================================================================ */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = (void *)0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SFS = (void *)0;
    EFI_FILE_PROTOCOL *Root = (void *)0;
    EFI_FILE_PROTOCOL *KernelFile = (void *)0;
    struct uefi_boot_info *boot_info = (void *)0;

    /* ============================================================
     * Step 1: Get the Loaded Image Protocol
     * ============================================================ */
    status = BS->HandleProtocol(
        ImageHandle,
        (EFI_GUID *)&gLoadedImageGuid,
        (void **)&LoadedImage
    );
    if (EFI_ERROR(status) || !LoadedImage) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: Cannot get LoadedImageProtocol\r\n"
        );
        return status;
    }

    /* ============================================================
     * Step 2: Open the filesystem volume
     * ============================================================ */
    status = BS->HandleProtocol(
        LoadedImage->DeviceHandle,
        (EFI_GUID *)&gSfsGuid,
        (void **)&SFS
    );
    if (EFI_ERROR(status) || !SFS) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: Cannot open filesystem\r\n"
        );
        return status;
    }

    status = SFS->OpenVolume(SFS, &Root);
    if (EFI_ERROR(status) || !Root) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: Cannot open root volume\r\n"
        );
        return status;
    }

    /* ============================================================
     * Step 3: Open and read the kernel ELF file
     * ============================================================ */
    {
        CHAR16 fname[64];
        ascii_to_wide(fname, "\\kernel.elf");
        status = Root->Open(Root, &KernelFile, fname, EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(status) || !KernelFile) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: Cannot open \\kernel.elf\r\n"
        );
        Root->Close(Root);
        return status;
    }

    /* Read the ELF header */
    Elf64_Ehdr ehdr;
    {
        uint64_t fsize = sizeof(Elf64_Ehdr);
        zero_mem(&ehdr, sizeof(ehdr));
        status = KernelFile->Read(KernelFile, &fsize, &ehdr);
        if (EFI_ERROR(status) || fsize != sizeof(Elf64_Ehdr)) {
            SystemTable->ConOut->OutputString(
                SystemTable->ConOut,
                (CHAR16 *)L"\r\nERROR: Cannot read kernel ELF header\r\n"
            );
            KernelFile->Close(KernelFile);
            Root->Close(Root);
            return status;
        }
    }

    /* Validate ELF magic */
    if (*(uint32_t *)ehdr.e_ident != ELF_MAGIC) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: Invalid kernel ELF magic\r\n"
        );
        KernelFile->Close(KernelFile);
        Root->Close(Root);
        return EFI_LOAD_ERROR;
    }

    /* ============================================================
     * Step 4: Load ELF program segments
     * ============================================================ */
    {
        /* Read program headers */
        uint64_t phdr_size = (uint64_t)ehdr.e_phentsize * (uint64_t)ehdr.e_phnum;
        if (phdr_size > 0x10000) {
            /* Sanity: program headers shouldn't be > 64KB */
            SystemTable->ConOut->OutputString(
                SystemTable->ConOut,
                (CHAR16 *)L"\r\nERROR: Kernel ELF program headers too large\r\n"
            );
            KernelFile->Close(KernelFile);
            Root->Close(Root);
            return EFI_LOAD_ERROR;
        }

        /* Allocate a buffer for program headers (using UEFI pool) */
        Elf64_Phdr *phdrs = (void *)0;
        status = BS->AllocatePool(EfiLoaderData, phdr_size, (void **)&phdrs);
        if (EFI_ERROR(status) || !phdrs) {
            SystemTable->ConOut->OutputString(
                SystemTable->ConOut,
                (CHAR16 *)L"\r\nERROR: Cannot allocate memory for program headers\r\n"
            );
            KernelFile->Close(KernelFile);
            Root->Close(Root);
            return EFI_OUT_OF_RESOURCES;
        }

        /* Seek to program header offset */
        {
            uint64_t pos = ehdr.e_phoff;
            KernelFile->SetPosition(KernelFile, pos);
        }

        {
            uint64_t to_read = phdr_size;
            zero_mem(phdrs, phdr_size);
            status = KernelFile->Read(KernelFile, &to_read, phdrs);
            if (EFI_ERROR(status) || to_read != phdr_size) {
                BS->FreePool(phdrs);
                SystemTable->ConOut->OutputString(
                    SystemTable->ConOut,
                    (CHAR16 *)L"\r\nERROR: Cannot read kernel program headers\r\n"
                );
                KernelFile->Close(KernelFile);
                Root->Close(Root);
                return EFI_LOAD_ERROR;
            }
        }

        /* Load each PT_LOAD segment */
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            Elf64_Phdr *ph = &phdrs[i];

            if (ph->p_type != PT_LOAD) continue;
            if (ph->p_memsz == 0) continue;

            /* Allocate physical pages for the segment.
             * Use AllocateAddress to get the exact physical address
             * the kernel expects (identity-mapped). */
            uint64_t pages = (ph->p_memsz + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
            EFI_PHYSICAL_ADDRESS alloc_addr = ph->p_paddr;

            /* Allocate at the specific physical address */
            status = BS->AllocatePages(
                AllocateAddress,
                EfiLoaderData,
                pages,
                &alloc_addr
            );
            if (EFI_ERROR(status)) {
                /* Try AllocateAnyPages as fallback.
                 * This will break identity mapping, but try anyway. */
                alloc_addr = 0x100000;  /* prefer 1MB */
                status = BS->AllocatePages(
                    AllocateMaxAddress,
                    EfiLoaderData,
                    pages,
                    &alloc_addr
                );
                if (EFI_ERROR(status)) {
                    BS->FreePool(phdrs);
                    SystemTable->ConOut->OutputString(
                        SystemTable->ConOut,
                        (CHAR16 *)L"\r\nERROR: Cannot allocate memory for kernel segment\r\n"
                    );
                    KernelFile->Close(KernelFile);
                    Root->Close(Root);
                    return EFI_OUT_OF_RESOURCES;
                }
            }

            /* Zero the allocated memory */
            zero_mem((void *)(uintptr_t)alloc_addr, ph->p_memsz);

            /* Read segment data from file */
            if (ph->p_filesz > 0) {
                /* Seek to segment offset */
                KernelFile->SetPosition(KernelFile, ph->p_offset);

                uint64_t to_read = ph->p_filesz;
                status = KernelFile->Read(
                    KernelFile,
                    &to_read,
                    (void *)(uintptr_t)alloc_addr
                );
                if (EFI_ERROR(status)) {
                    BS->FreePool(phdrs);
                    SystemTable->ConOut->OutputString(
                        SystemTable->ConOut,
                        (CHAR16 *)L"\r\nERROR: Cannot read kernel segment data\r\n"
                    );
                    KernelFile->Close(KernelFile);
                    Root->Close(Root);
                    return EFI_LOAD_ERROR;
                }
            }
        }

        BS->FreePool(phdrs);
    }

    /* Close the kernel file (we're done reading) */
    KernelFile->Close(KernelFile);
    KernelFile = (void *)0;

    /* ============================================================
     * Step 5: Get the UEFI memory map
     * ============================================================ */
    uint64_t mmap_size = 0;
    uint64_t mmap_key = 0;
    uint64_t desc_size = 0;
    uint32_t desc_version = 0;
    EFI_MEMORY_DESCRIPTOR *mmap = (void *)0;

    /* First call: get the required buffer size */
    status = BS->GetMemoryMap(&mmap_size, (void *)0, &mmap_key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: GetMemoryMap (size query) failed\r\n"
        );
        Root->Close(Root);
        return status;
    }

    /* Add extra space for the map growth that ExitBootServices may cause */
    mmap_size += 4096;

    /* Allocate buffer for the memory map */
    status = BS->AllocatePool(EfiLoaderData, mmap_size, (void **)&mmap);
    if (EFI_ERROR(status) || !mmap) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: Cannot allocate memory map buffer\r\n"
        );
        Root->Close(Root);
        return EFI_OUT_OF_RESOURCES;
    }

    /* Second call: get the actual memory map */
    status = BS->GetMemoryMap(&mmap_size, mmap, &mmap_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nERROR: GetMemoryMap failed\r\n"
        );
        BS->FreePool(mmap);
        Root->Close(Root);
        return status;
    }

    /* ============================================================
     * Step 6: Get GOP framebuffer info
     * ============================================================ */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *GOP = (void *)0;
    status = BS->HandleProtocol(
        ImageHandle,
        (EFI_GUID *)&gGopGuid,
        (void **)&GOP
    );
    if (EFI_ERROR(status)) {
        /* Try LocateProtocol as fallback */
        status = BS->LocateProtocol(
            (EFI_GUID *)&gGopGuid,
            (void *)0,
            (void **)&GOP
        );
    }

    int fb_valid = 0;
    uint64_t fb_addr = 0;
    uint32_t fb_width = 0, fb_height = 0, fb_pitch = 0, fb_bpp = 0;
    uint8_t fb_red_ms = 0, fb_red_mp = 0;
    uint8_t fb_green_ms = 0, fb_green_mp = 0;
    uint8_t fb_blue_ms = 0, fb_blue_mp = 0;
    uint8_t fb_pixel_fmt = 0;

    if (!EFI_ERROR(status) && GOP && GOP->Mode) {
        fb_addr  = GOP->Mode->FrameBufferBase;
        fb_width  = GOP->Mode->Info->HorizontalResolution;
        fb_height = GOP->Mode->Info->VerticalResolution;
        fb_pitch  = GOP->Mode->Info->PixelsPerScanLine * 4;
        fb_bpp    = 32;
        fb_pixel_fmt = (uint8_t)GOP->Mode->Info->PixelFormat;

        if (GOP->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
            fb_red_ms = 8; fb_red_mp = 0;
            fb_green_ms = 8; fb_green_mp = 8;
            fb_blue_ms = 8; fb_blue_mp = 16;
        } else if (GOP->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
            fb_blue_ms = 8; fb_blue_mp = 0;
            fb_green_ms = 8; fb_green_mp = 8;
            fb_red_ms = 8; fb_red_mp = 16;
        } else if (GOP->Mode->Info->PixelFormat == PixelBitMask) {
            /* Extract from PixelInformation */
            EFI_PIXEL_BITMASK *bm = &GOP->Mode->Info->PixelInformation;
            uint32_t mask;
            mask = bm->RedMask;
            fb_red_mp = 0; while (!(mask & 1)) { fb_red_mp++; mask >>= 1; }
            fb_red_ms = 0; while (mask & 1) { fb_red_ms++; mask >>= 1; }
            mask = bm->GreenMask;
            fb_green_mp = 0; while (!(mask & 1)) { fb_green_mp++; mask >>= 1; }
            fb_green_ms = 0; while (mask & 1) { fb_green_ms++; mask >>= 1; }
            mask = bm->BlueMask;
            fb_blue_mp = 0; while (!(mask & 1)) { fb_blue_mp++; mask >>= 1; }
            fb_blue_ms = 0; while (mask & 1) { fb_blue_ms++; mask >>= 1; }
        }

        fb_valid = 1;
    }

    /* ============================================================
     * Step 7: Allocate and fill the boot info structure
     * ============================================================ */
    status = BS->AllocatePool(
        EfiLoaderData,
        sizeof(struct uefi_boot_info),
        (void **)&boot_info
    );
    if (EFI_ERROR(status) || !boot_info) {
        BS->FreePool(mmap);
        Root->Close(Root);
        return EFI_OUT_OF_RESOURCES;
    }

    zero_mem(boot_info, sizeof(struct uefi_boot_info));
    boot_info->magic = UEFI_BOOT_MAGIC;
    boot_info->version = 1;
    boot_info->mmap_desc_size = (uint32_t)desc_size;
    boot_info->mmap_key = mmap_key;

    /* Copy memory map entries */
    uint64_t num_entries = mmap_size / desc_size;
    if (num_entries > UEFI_MMAP_MAX_ENTRIES) num_entries = UEFI_MMAP_MAX_ENTRIES;
    boot_info->mmap_num_entries = (uint32_t)num_entries;

    for (uint64_t i = 0; i < num_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mmap + i * desc_size);
        boot_info->mmap[i].phys_start = desc->PhysicalStart;
        boot_info->mmap[i].num_pages  = desc->NumberOfPages;
        boot_info->mmap[i].attributes = desc->Attribute;
        boot_info->mmap[i].type       = desc->Type;
        boot_info->mmap[i].pad        = 0;
    }

    /* Framebuffer info */
    boot_info->fb_valid          = fb_valid;
    boot_info->fb_addr           = fb_addr;
    boot_info->fb_width          = fb_width;
    boot_info->fb_height         = fb_height;
    boot_info->fb_pitch          = fb_pitch;
    boot_info->fb_bpp            = fb_bpp;
    boot_info->fb_red_mask_size  = fb_red_ms;
    boot_info->fb_red_mask_pos   = fb_red_mp;
    boot_info->fb_green_mask_size = fb_green_ms;
    boot_info->fb_green_mask_pos  = fb_green_mp;
    boot_info->fb_blue_mask_size  = fb_blue_ms;
    boot_info->fb_blue_mask_pos   = fb_blue_mp;
    boot_info->fb_pixel_format   = fb_pixel_fmt;

    /* ============================================================
     * Step 8: Exit boot services
     * ============================================================ */
    BS->FreePool(mmap);
    mmap = (void *)0;

    Root->Close(Root);
    Root = (void *)0;

    status = BS->ExitBootServices(ImageHandle, mmap_key);
    if (EFI_ERROR(status)) {
        /* ExitBootServices may fail if the map key changed.
         * In that case, re-get the memory map and try again. */
        SystemTable->ConOut->OutputString(
            SystemTable->ConOut,
            (CHAR16 *)L"\r\nWARNING: ExitBootServices failed, retrying...\r\n"
        );

        /* Get the map key again */
        mmap_size = 0;
        status = BS->GetMemoryMap(&mmap_size, (void *)0, &mmap_key, &desc_size, &desc_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            /* Cannot recover */
            return status;
        }
        mmap_size += 4096;
        status = BS->AllocatePool(EfiLoaderData, mmap_size, (void **)&mmap);
        if (EFI_ERROR(status)) return status;
        status = BS->GetMemoryMap(&mmap_size, mmap, &mmap_key, &desc_size, &desc_version);
        if (EFI_ERROR(status)) return status;
        BS->FreePool(mmap);

        /* Retry ExitBootServices */
        status = BS->ExitBootServices(ImageHandle, mmap_key);
        if (EFI_ERROR(status)) {
            return status;
        }
    }

    /* ============================================================
     * Step 9: Set up identity-mapped paging
     * ============================================================ */
    setup_identity_paging();

    /* ============================================================
     * Step 10: Jump to the kernel
     * ============================================================ */
    {
        /* kernel_main(uint32_t magic, void *boot_info) */
        void (*kernel_entry)(uint32_t, void *) =
            (void (*)(uint32_t, void *))(uintptr_t)ehdr.e_entry;

        /* Pass UEFI boot magic and boot_info pointer */
        kernel_entry(UEFI_BOOT_MAGIC, boot_info);

        /* Should never return */
        while (1) {
            asm volatile ("hlt");
        }
    }

    return EFI_SUCCESS;
}
# AuroraOS Makefile with GRUB2 (Multiboot1) + UEFI boot support
# Supports debug/release builds and auto-detects cross-compiler.
#
# Usage:
#   make              - release build
#   make debug        - debug build (-g -O0)
#   make uefi         - build UEFI bootloader (EFI/BOOT/BOOTX64.EFI)
#   make iso          - build + create bootable ISO (BIOS + UEFI hybrid)
#   make run          - build ISO + run in QEMU
#   make clean        - remove all build artifacts

# Toolchain: try x86_64-elf-gcc first, fall back to system gcc
CC_CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
LD_CROSS := $(shell which x86_64-elf-ld 2>/dev/null)
OC_CROSS := $(shell which x86_64-elf-objcopy 2>/dev/null)

ifeq ($(CC_CROSS),)
  CC := gcc
else
  CC := $(CC_CROSS)
endif

ifeq ($(LD_CROSS),)
  LD := ld
else
  LD := $(LD_CROSS)
endif

ifeq ($(OC_CROSS),)
  OBJCOPY := objcopy
else
  OBJCOPY := $(OC_CROSS)
endif

# Auto-detect version from version.h (single source of truth)
AURORAOS_MAJOR := $(shell grep 'AURORAOS_MAJOR' kernel/include/version.h | grep -o '[0-9]\+' | head -1)
AURORAOS_MINOR := $(shell grep 'AURORAOS_MINOR' kernel/include/version.h | grep -o '[0-9]\+' | head -1)
AURORAOS_PATCH := $(shell grep 'AURORAOS_PATCH' kernel/include/version.h | grep -o '[0-9]\+' | head -1)
AURORAOS_VERSION := v$(AURORAOS_MAJOR).$(AURORAOS_MINOR).$(AURORAOS_PATCH)

# Base flags (build date and git hash are auto-detected)
BUILD_DATE := $(shell date -u +'%Y-%m-%d %H:%M' 2>/dev/null || echo "unknown")
GIT_HASH   := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_TYPE ?= release

CFLAGS_BASE := -ffreestanding -Wall -Wextra -fno-pic -fstack-protector-strong -mno-sse \
               -mgeneral-regs-only -Ikernel/include -std=gnu17 \
               -DBUILD_DATE="\"$(BUILD_DATE)\"" -DGIT_HASH="\"$(GIT_HASH)\"" \
               -DBUILD_TYPE="\"$(BUILD_TYPE)\""

# UEFI bootloader flags (position-independent, ms_abi)
UEFI_CFLAGS := -ffreestanding -fpic -fno-stack-protector -mno-sse \
               -mgeneral-regs-only -mno-red-zone -Ikernel/include -Iboot \
               -std=gnu17 -O2 -DNDEBUG

# Debug build
CFLAGS_DEBUG := -g -O0 -DDEBUG

# Release build
CFLAGS_RELEASE := -O2 -DNDEBUG

# Default: release
CFLAGS := $(CFLAGS_BASE) $(CFLAGS_RELEASE)
LDFLAGS := -nostdlib -T linker.ld

SRCDIR   := kernel
BUILDDIR := build
ISODIR   := iso

KERNEL := $(BUILDDIR)/kernel.elf

# UEFI bootloader artifacts
UEFI_APP := $(BUILDDIR)/efi_app.so
UEFI_EFI := $(ISODIR)/EFI/BOOT/BOOTX64.EFI

# Find all source files
K_C_SRCS := $(shell find $(SRCDIR) -type f -name '*.c' 2>/dev/null)
K_S_SRCS := $(shell find $(SRCDIR) arch -type f -name '*.S' 2>/dev/null)

OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(K_C_SRCS))
OBJS += $(patsubst arch/%.S,$(BUILDDIR)/arch/%.o,$(filter arch/%,$(K_S_SRCS)))
OBJS += $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%.o,$(filter $(SRCDIR)/%,$(K_S_SRCS)))

.PHONY: all debug uefi clean iso run help modules version test check-update checksum

all: $(KERNEL)

debug: CFLAGS := $(CFLAGS_BASE) $(CFLAGS_DEBUG)
debug: BUILD_TYPE = debug
debug: $(KERNEL)

# ================================================================
# Kernel build rules
# ================================================================
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/arch/%.o: arch/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS)
	@echo "  LD    $(KERNEL)"
	$(LD) $(LDFLAGS) -o $@ $^

# ================================================================
# UEFI bootloader build rules
# ================================================================
$(BUILDDIR)/efi_main.o: boot/efi_main.c boot/uefi.h boot/boot_info.h
	@mkdir -p $(BUILDDIR)
	@echo "  CC    $(BUILDDIR)/efi_main.o"
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(UEFI_APP): $(BUILDDIR)/efi_main.o
	@echo "  LD    $(UEFI_APP)"
	$(LD) -nostdlib -shared -Bsymbolic -T boot/uefi.lds \
		-o $(UEFI_APP) $(BUILDDIR)/efi_main.o

$(UEFI_EFI): $(UEFI_APP)
	@echo "  OBJCOPY $(UEFI_EFI)"
	mkdir -p $(dir $@)
	$(OBJCOPY) -j .text -j .data -j .rodata -j .reloc \
		-j .dynsym -j .dynstr --target=efi-app-x86_64 \
		$(UEFI_APP) $(UEFI_EFI)

uefi: $(UEFI_EFI)

# ================================================================
# ISO image (BIOS + UEFI hybrid)
# ================================================================
iso: $(KERNEL) $(UEFI_EFI)
	@echo "  ISO   os.iso"
	mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.elf
	printf 'set timeout=0\nset default=0\n\nmenuentry "AuroraOS" {\n    multiboot /boot/kernel.elf\n    boot\n}\n' > $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o os.iso $(ISODIR) 2>/dev/null

run: iso
	@echo "  QEMU  starting..."
	qemu-system-x86_64 -m 256M -cdrom os.iso -nographic -no-reboot

clean:
	rm -rf $(BUILDDIR) os.iso $(ISODIR)

help:
	@echo "AuroraOS Build System $(AURORAOS_VERSION)"
	@echo "  make              - release build (optimized)"
	@echo "  make debug        - debug build (-g -O0)"
	@echo "  make uefi         - build UEFI bootloader (EFI/BOOT/BOOTX64.EFI)"
	@echo "  make iso          - build + create hybrid ISO (BIOS + UEFI)"
	@echo "  make run          - build + run in QEMU"
	@echo "  make modules      - build kernel modules"
	@echo "  make clean        - remove all artifacts"
	@echo "  make version      - show version information"
	@echo "  make checksum     - generate SHA256 checksum for os.iso"
	@echo "  make check-update - check GitHub for newer version"
	@echo "  make test         - build and run automated tests"
	@echo ""
	@echo "Toolchain: CC=$(CC) LD=$(LD) OBJCOPY=$(OBJCOPY)"
	@echo "Build:     $(BUILD_TYPE) | $(BUILD_DATE) | $(GIT_HASH)"

# Show version information
version:
	@echo "AuroraOS $(AURORAOS_VERSION)"
	@echo "  Build date: $(BUILD_DATE)"
	@echo "  Git hash:   $(GIT_HASH)"
	@echo "  Build type: $(BUILD_TYPE)"
	@echo "  Toolchain:  $(CC)"

# Generate SHA256 checksum for the ISO
checksum: iso
	@echo "  SHA256 os.iso"
	@sha256sum os.iso > os.iso.sha256 2>/dev/null || shasum -a 256 os.iso > os.iso.sha256 2>/dev/null || echo "Warning: sha256sum not found"
	@cat os.iso.sha256 2>/dev/null || true

# Check GitHub for newer version
check-update:
	@echo "  Checking for updates..."
	@scripts/check_update.sh 2>/dev/null || echo "  Update check requires scripts/check_update.sh"

# Run automated tests in QEMU
test: iso
	@echo "  TEST  running self-tests in QEMU..."
	@scripts/run_qemu_test.py 2>/dev/null || (echo "  TEST  running with qemu..."; qemu-system-x86_64 -m 256M -cdrom os.iso -nographic -no-reboot 2>&1 | tee qemu_test.log)

# ================================================================
# Kernel modules
# ================================================================
MODULE_CFLAGS := -ffreestanding -Wall -Wextra -fno-pic -fno-stack-protector \
                 -mno-sse -mgeneral-regs-only -mno-red-zone \
                 -std=gnu17 -O2 -DNDEBUG

MODULE_SRCS := userspace/mod_sample.c
MODULE_OBJS := $(patsubst userspace/%.c,$(BUILDDIR)/modules/%.o,$(MODULE_SRCS))
MODULE_KOS  := $(patsubst userspace/%.c,$(BUILDDIR)/modules/%.ko,$(MODULE_SRCS))

$(BUILDDIR)/modules/%.o: userspace/%.c
	@mkdir -p $(dir $@)
	@echo "  CC[M] $@"
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

$(BUILDDIR)/modules/%.ko: $(BUILDDIR)/modules/%.o
	@echo "  LD[M] $@"
	$(LD) -r -o $@ $<

modules: $(MODULE_KOS)
	@echo "  MODULES built: $(MODULE_KOS)"

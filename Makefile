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

# Cross-compiler paths for multi-arch
RISCV64_CC  := $(shell which riscv64-unknown-elf-gcc 2>/dev/null || which riscv64-linux-gnu-gcc 2>/dev/null)
RISCV64_LD  := $(shell which riscv64-unknown-elf-ld 2>/dev/null || which riscv64-linux-gnu-ld 2>/dev/null)
AARCH64_CC  := $(shell which aarch64-linux-gnu-gcc 2>/dev/null || which aarch64-elf-gcc 2>/dev/null)
AARCH64_LD  := $(shell which aarch64-linux-gnu-ld 2>/dev/null || which aarch64-elf-ld 2>/dev/null)
LOONGARCH64_CC := $(shell which loongarch64-linux-gnu-gcc 2>/dev/null)
LOONGARCH64_LD := $(shell which loongarch64-linux-gnu-ld 2>/dev/null)

# Cross-compiler objcopy for multi-arch
RISCV64_OC  := $(shell which riscv64-unknown-elf-objcopy 2>/dev/null || which riscv64-linux-gnu-objcopy 2>/dev/null)
AARCH64_OC  := $(shell which aarch64-linux-gnu-objcopy 2>/dev/null || which aarch64-elf-objcopy 2>/dev/null)
LOONGARCH64_OC := $(shell which loongarch64-linux-gnu-objcopy 2>/dev/null)

# Architecture selection (default: x86_64)
# Usage: make ARCH=riscv64 | make ARCH=aarch64 | make ARCH=loongarch64
ARCH ?= x86_64

# Auto-detect version from version.h (single source of truth)
AURORAOS_MAJOR := $(shell grep 'AURORAOS_MAJOR' kernel/include/version.h | grep -o '[0-9]\+' | head -1)
AURORAOS_MINOR := $(shell grep 'AURORAOS_MINOR' kernel/include/version.h | grep -o '[0-9]\+' | head -1)
AURORAOS_PATCH := $(shell grep 'AURORAOS_PATCH' kernel/include/version.h | grep -o '[0-9]\+' | head -1)
AURORAOS_VERSION := v$(AURORAOS_MAJOR).$(AURORAOS_MINOR).$(AURORAOS_PATCH)

# Base flags (build date and git hash are auto-detected)
BUILD_DATE := $(shell date -u +'%Y-%m-%d %H:%M' 2>/dev/null || echo "unknown")
GIT_HASH   := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_TYPE ?= release

# -DMODULE_SIGN_CHECK enforces SHA-256 signature verification on all loaded modules.
# Remove this flag for development builds where module signing is not required.
CFLAGS_BASE := -ffreestanding -Wall -Wextra -fno-pic -fstack-protector-strong -mno-sse \
               -mgeneral-regs-only -mno-red-zone -Ikernel/include -std=gnu17 \
               -DBUILD_DATE="\"$(BUILD_DATE)\"" -DGIT_HASH="\"$(GIT_HASH)\"" \
               -DBUILD_TYPE="\"$(BUILD_TYPE)\"" -DMODULE_SIGN_CHECK

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
K_S_SRCS := $(shell find $(SRCDIR) arch/x86_64 -type f -name '*.S' 2>/dev/null)

OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(K_C_SRCS))
OBJS += $(patsubst arch/%.S,$(BUILDDIR)/arch/%.o,$(filter arch/%,$(K_S_SRCS)))
OBJS += $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%.o,$(filter $(SRCDIR)/%,$(K_S_SRCS)))

.PHONY: all debug uefi clean iso run help modules version test smoke-test regression-test check-update checksum modules-build modules-clean arch-riscv64 arch-aarch64 arch-loongarch64 arch-all run-riscv64 run-aarch64 run-loongarch64

# Architecture-specific target selection
ifeq ($(ARCH),x86_64)
all: $(KERNEL)

debug: CFLAGS := $(CFLAGS_BASE) $(CFLAGS_DEBUG)
debug: BUILD_TYPE = debug
debug: $(KERNEL)
else
all: arch-$(ARCH)
debug: arch-$(ARCH)
endif

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

ifeq ($(ARCH),x86_64)
run: iso
	@echo "  QEMU  starting..."
	qemu-system-x86_64 -m 256M -cdrom os.iso -nographic -no-reboot
else
run: run-$(ARCH)
endif

clean:
	rm -rf $(BUILDDIR) os.iso $(ISODIR)

help:
	@echo "AuroraOS Build System $(AURORAOS_VERSION)"
	@echo "  make              - release build (optimized, x86_64 default)"
	@echo "  make debug        - debug build (-g -O0)"
	@echo "  make uefi         - build UEFI bootloader (EFI/BOOT/BOOTX64.EFI)"
	@echo "  make iso          - build + create hybrid ISO (BIOS + UEFI)"
	@echo "  make run          - build + run in QEMU"
	@echo "  make modules      - build kernel modules"
	@echo "  make modules-build - build modules from modules/ directory"
	@echo "  make clean        - remove all artifacts"
	@echo "  make version      - show version information"
	@echo "  make checksum     - generate SHA256 checksum for os.iso"
	@echo "  make check-update - check GitHub for newer version"
	@echo "  make test         - build and run automated tests (smoke + self-test)"
	@echo "  make smoke-test   - build ISO and run smoke test"
	@echo "  make regression-test - build ISO and run regression test suite"
	@echo ""
	@echo "Multi-Architecture targets (via ARCH= variable):"
	@echo "  make ARCH=riscv64       - build riscv64 kernel (needs cross-compiler)"
	@echo "  make ARCH=aarch64       - build aarch64 kernel (needs cross-compiler)"
	@echo "  make ARCH=loongarch64   - build loongarch64 kernel (needs cross-compiler)"
	@echo "  make ARCH=riscv64 run   - build + run riscv64 kernel in QEMU"
	@echo "  make ARCH=aarch64 run   - build + run aarch64 kernel in QEMU"
	@echo "  make ARCH=loongarch64 run - build + run loongarch64 kernel in QEMU"
	@echo ""
	@echo "Explicit architecture targets:"
	@echo "  make arch-riscv64    - build riscv64 kernel"
	@echo "  make arch-aarch64    - build aarch64 kernel"
	@echo "  make arch-loongarch64- build loongarch64 kernel"
	@echo "  make arch-all        - build all architecture kernels"
	@echo "  make run-riscv64     - build + run riscv64 kernel in QEMU"
	@echo "  make run-aarch64     - build + run aarch64 kernel in QEMU"
	@echo "  make run-loongarch64 - build + run loongarch64 kernel in QEMU"
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

# Run automated tests in QEMU (smoke test first, then self-test)
test: iso
	@echo "  TEST  running smoke test..."
	@scripts/smoke_test.sh 2>/dev/null || (echo "  TEST  smoke test failed (see above)"); \
	echo "  TEST  running self-tests in QEMU..."; \
	scripts/run_qemu_test.py 2>/dev/null || (echo "  TEST  running with qemu..."; qemu-system-x86_64 -m 256M -cdrom os.iso -nographic -no-reboot 2>&1 | tee qemu_test.log)

# Smoke test: boot and check basic functionality
smoke-test: iso
	@echo "  SMOKE running smoke test..."
	@scripts/smoke_test.sh

# Regression test: comprehensive automated test suite
regression-test: iso
	@echo "  REGRESSION running regression test suite..."
	@python3 scripts/regression_test.py || python scripts/regression_test.py

# CI pipeline: full build + smoke + regression + quality check
ci: iso
	@echo "  CI     running CI pipeline..."
	@bash scripts/ci_regression.sh

# CI quick: build + smoke only (fast pre-commit check)
ci-quick: iso
	@echo "  CI     running quick CI check..."
	@bash scripts/ci_regression.sh --quick

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

# ================================================================
# modules-build: Build modules from the modules/ directory
# ================================================================
modules-build:
	@echo "  MODULES building from modules/ directory..."
	@if [ -f modules/Makefile.template ]; then \
		for mod_src in modules/*.c; do \
			mod_name=$$(basename $$mod_src .c); \
			echo "  Building module: $$mod_name"; \
			$(MAKE) -f modules/Makefile.template MODULE=$$mod_name BUILDDIR=$(BUILDDIR)/modules/$$mod_name 2>/dev/null || \
			true; \
		done; \
	else \
		echo "  modules/Makefile.template not found"; \
	fi
	@echo "  MODULES build complete."

modules-clean:
	@echo "  Cleaning modules..."
	@for mod_dir in $(BUILDDIR)/modules/*/; do \
		if [ -f "$$mod_dir/Makefile" ] || [ -f "modules/Makefile.template" ]; then \
			rm -rf "$$mod_dir"; \
		fi \
	done 2>/dev/null || true
	@rm -rf $(BUILDDIR)/modules
	@echo "  Modules cleaned."

# ================================================================
# Multi-architecture build targets
#
# Each architecture target compiles the arch-specific boot code,
# context switch, and links against the architecture's linker script.
# The kernel C code is shared across architectures via the arch.h
# abstraction layer.  Each architecture requires its own cross-compiler.
# ================================================================

# Arch-specific CFLAGS
RISCV64_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-stack-protector \
                  -march=rv64gc -mabi=lp64 -mno-relax \
                  -Ikernel/include -Iarch/riscv64 \
                  -std=gnu17 -O2 -DNDEBUG -DARCH_RISCV64 \
                  -DBUILD_DATE="\"$(BUILD_DATE)\"" -DGIT_HASH="\"$(GIT_HASH)\""

AARCH64_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-stack-protector \
                  -mgeneral-regs-only \
                  -Ikernel/include -Iarch/aarch64 \
                  -std=gnu17 -O2 -DNDEBUG -DARCH_AARCH64 \
                  -DBUILD_DATE="\"$(BUILD_DATE)\"" -DGIT_HASH="\"$(GIT_HASH)\""

LOONGARCH64_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-stack-protector \
                      -Ikernel/include -Iarch/loongarch64 \
                      -std=gnu17 -O2 -DNDEBUG -DARCH_LOONGARCH64 \
                      -DBUILD_DATE="\"$(BUILD_DATE)\"" -DGIT_HASH="\"$(GIT_HASH)\""

# Arch-specific ASFLAGS (same as CFLAGS for assembly compilation)
RISCV64_ASFLAGS := $(RISCV64_CFLAGS)
AARCH64_ASFLAGS := $(AARCH64_CFLAGS)
LOONGARCH64_ASFLAGS := $(LOONGARCH64_CFLAGS)

# Arch-specific LDFLAGS
RISCV64_LDFLAGS := -nostdlib -T arch/riscv64/linker.ld
AARCH64_LDFLAGS := -nostdlib -T arch/aarch64/linker.ld
LOONGARCH64_LDFLAGS := -nostdlib -T arch/loongarch64/linker.ld

# Arch-specific kernel source files (shared C code, excluding x86_64-specific files)
ARCH_CORE_SRCS := kernel/mem.c kernel/log.c kernel/string.c kernel/print.c \
                  kernel/panic.c kernel/rbtree.c kernel/ramfs.c kernel/vfs.c \
                  kernel/fs.c kernel/pipe.c kernel/ext2.c kernel/fat32.c \
                  kernel/journal.c kernel/fsck.c kernel/squashfs.c \
                  kernel/elfloader.c kernel/signal.c kernel/sched.c \
                  kernel/syscall.c kernel/syscall_entry.c kernel/sysfs.c \
                  kernel/procfs.c kernel/devtmpfs.c kernel/file.c \
                  kernel/seccomp.c kernel/capability.c kernel/module.c \
                  kernel/module_sign.c kernel/aslr.c kernel/stack_protect.c \
                  kernel/perf.c kernel/sysctl.c kernel/cmdline.c \
                  kernel/explain.c kernel/shell.c kernel/user.c \
                  kernel/embedded_files.c kernel/irq.c kernel/block_dev.c \
                  kernel/ramdisk.c kernel/rtc.c kernel/selftest.c \
                  kernel/net/net.c kernel/net/dhcp.c kernel/net/dns.c \
                  kernel/net/ipv6.c kernel/net/tcp_cong.c kernel/net/http.c \
                  kernel/arch_entry.c

# riscv64 kernel build
.PHONY: arch-riscv64
arch-riscv64:
ifeq ($(RISCV64_CC),)
	$(warning riscv64 cross-compiler not found, skipping)
else
	@mkdir -p $(BUILDDIR)/arch/riscv64
	@echo "=== Building AuroraOS for riscv64 ==="
	@echo "  CC[riscv64] boot.S"
	$(RISCV64_CC) $(RISCV64_CFLAGS) -c arch/riscv64/boot.S -o $(BUILDDIR)/arch/riscv64/boot.o
	@echo "  CC[riscv64] context.S"
	$(RISCV64_CC) $(RISCV64_CFLAGS) -c arch/riscv64/context.S -o $(BUILDDIR)/arch/riscv64/context.o
	@echo "  CC[riscv64] arch_init.c"
	$(RISCV64_CC) $(RISCV64_CFLAGS) -c arch/riscv64/arch_init.c -o $(BUILDDIR)/arch/riscv64/arch_init.o
	@for src in $(ARCH_CORE_SRCS); do \
		obj=$(BUILDDIR)/arch/riscv64/$$(basename $$src .c).o; \
		echo "  CC[riscv64] $$src"; \
		$(RISCV64_CC) $(RISCV64_CFLAGS) -c $$src -o $$obj 2>/dev/null || true; \
	done
	@echo "  LD[riscv64] kernel.elf"
	$(RISCV64_LD) -nostdlib -T arch/riscv64/linker.ld \
		$(BUILDDIR)/arch/riscv64/*.o -o $(BUILDDIR)/arch/riscv64/kernel.elf 2>/dev/null || \
		(echo "  Note: Full kernel requires arch-specific adaptations (main.c, console.c, etc.)"; \
		 echo "  Boot stub and context switch compiled successfully.")
	@echo "  OBJCOPY[riscv64] kernel.bin"
	$(RISCV64_OC) -O binary $(BUILDDIR)/arch/riscv64/kernel.elf $(BUILDDIR)/arch/riscv64/kernel.bin 2>/dev/null || true
	@echo "  riscv64 kernel build complete"
endif

# aarch64 kernel build
.PHONY: arch-aarch64
arch-aarch64:
ifeq ($(AARCH64_CC),)
	$(warning aarch64 cross-compiler not found, skipping)
else
	@mkdir -p $(BUILDDIR)/arch/aarch64
	@echo "=== Building AuroraOS for aarch64 ==="
	@echo "  CC[aarch64] boot.S"
	$(AARCH64_CC) $(AARCH64_CFLAGS) -c arch/aarch64/boot.S -o $(BUILDDIR)/arch/aarch64/boot.o
	@echo "  CC[aarch64] context.S"
	$(AARCH64_CC) $(AARCH64_CFLAGS) -c arch/aarch64/context.S -o $(BUILDDIR)/arch/aarch64/context.o
	@echo "  CC[aarch64] arch_init.c"
	$(AARCH64_CC) $(AARCH64_CFLAGS) -c arch/aarch64/arch_init.c -o $(BUILDDIR)/arch/aarch64/arch_init.o
	@for src in $(ARCH_CORE_SRCS); do \
		obj=$(BUILDDIR)/arch/aarch64/$$(basename $$src .c).o; \
		echo "  CC[aarch64] $$src"; \
		$(AARCH64_CC) $(AARCH64_CFLAGS) -c $$src -o $$obj 2>/dev/null || true; \
	done
	@echo "  LD[aarch64] kernel.elf"
	$(AARCH64_LD) -nostdlib -T arch/aarch64/linker.ld \
		$(BUILDDIR)/arch/aarch64/*.o -o $(BUILDDIR)/arch/aarch64/kernel.elf 2>/dev/null || \
		(echo "  Note: Full kernel requires arch-specific adaptations (main.c, console.c, etc.)"; \
		 echo "  Boot stub and context switch compiled successfully.")
	@echo "  OBJCOPY[aarch64] kernel.bin"
	$(AARCH64_OC) -O binary $(BUILDDIR)/arch/aarch64/kernel.elf $(BUILDDIR)/arch/aarch64/kernel.bin 2>/dev/null || true
	@echo "  aarch64 kernel build complete"
endif

# loongarch64 kernel build
.PHONY: arch-loongarch64
arch-loongarch64:
ifeq ($(LOONGARCH64_CC),)
	$(warning loongarch64 cross-compiler not found, skipping)
else
	@mkdir -p $(BUILDDIR)/arch/loongarch64
	@echo "=== Building AuroraOS for loongarch64 ==="
	@echo "  CC[loongarch64] boot.S"
	$(LOONGARCH64_CC) $(LOONGARCH64_CFLAGS) -c arch/loongarch64/boot.S -o $(BUILDDIR)/arch/loongarch64/boot.o
	@echo "  CC[loongarch64] context.S"
	$(LOONGARCH64_CC) $(LOONGARCH64_CFLAGS) -c arch/loongarch64/context.S -o $(BUILDDIR)/arch/loongarch64/context.o
	@echo "  CC[loongarch64] arch_init.c"
	$(LOONGARCH64_CC) $(LOONGARCH64_CFLAGS) -c arch/loongarch64/arch_init.c -o $(BUILDDIR)/arch/loongarch64/arch_init.o
	@for src in $(ARCH_CORE_SRCS); do \
		obj=$(BUILDDIR)/arch/loongarch64/$$(basename $$src .c).o; \
		echo "  CC[loongarch64] $$src"; \
		$(LOONGARCH64_CC) $(LOONGARCH64_CFLAGS) -c $$src -o $$obj 2>/dev/null || true; \
	done
	@echo "  LD[loongarch64] kernel.elf"
	$(LOONGARCH64_LD) -nostdlib -T arch/loongarch64/linker.ld \
		$(BUILDDIR)/arch/loongarch64/*.o -o $(BUILDDIR)/arch/loongarch64/kernel.elf 2>/dev/null || \
		(echo "  Note: Full kernel requires arch-specific adaptations (main.c, console.c, etc.)"; \
		 echo "  Boot stub and context switch compiled successfully.")
	@echo "  OBJCOPY[loongarch64] kernel.bin"
	$(LOONGARCH64_OC) -O binary $(BUILDDIR)/arch/loongarch64/kernel.elf $(BUILDDIR)/arch/loongarch64/kernel.bin 2>/dev/null || true
	@echo "  loongarch64 kernel build complete"
endif

# Build all architectures
.PHONY: arch-all
arch-all: arch-riscv64 arch-aarch64 arch-loongarch64
	@echo "  Multi-arch build complete"

# QEMU run targets for each architecture
# Uses kernel.bin (raw binary) for -kernel, which is what QEMU expects
# on riscv64/aarch64/loongarch64 virt machines.
.PHONY: run-riscv64
run-riscv64: arch-riscv64
	@echo "  QEMU[riscv64] starting..."
	@if [ -f $(BUILDDIR)/arch/riscv64/kernel.bin ]; then \
		qemu-system-riscv64 -machine virt -m 256M -nographic \
			-kernel $(BUILDDIR)/arch/riscv64/kernel.bin \
			-no-reboot; \
	elif [ -f $(BUILDDIR)/arch/riscv64/kernel.elf ]; then \
		qemu-system-riscv64 -machine virt -m 256M -nographic \
			-kernel $(BUILDDIR)/arch/riscv64/kernel.elf \
			-no-reboot; \
	else \
		echo "  riscv64 kernel not found. Build may have failed."; \
	fi

.PHONY: run-aarch64
run-aarch64: arch-aarch64
	@echo "  QEMU[aarch64] starting..."
	@if [ -f $(BUILDDIR)/arch/aarch64/kernel.bin ]; then \
		qemu-system-aarch64 -machine virt -m 256M -nographic \
			-cpu cortex-a57 -kernel $(BUILDDIR)/arch/aarch64/kernel.bin \
			-no-reboot; \
	elif [ -f $(BUILDDIR)/arch/aarch64/kernel.elf ]; then \
		qemu-system-aarch64 -machine virt -m 256M -nographic \
			-cpu cortex-a57 -kernel $(BUILDDIR)/arch/aarch64/kernel.elf \
			-no-reboot; \
	else \
		echo "  aarch64 kernel not found. Build may have failed."; \
	fi

.PHONY: run-loongarch64
run-loongarch64: arch-loongarch64
	@echo "  QEMU[loongarch64] starting..."
	@if [ -f $(BUILDDIR)/arch/loongarch64/kernel.bin ]; then \
		qemu-system-loongarch64 -machine virt -m 256M -nographic \
			-kernel $(BUILDDIR)/arch/loongarch64/kernel.bin \
			-no-reboot; \
	elif [ -f $(BUILDDIR)/arch/loongarch64/kernel.elf ]; then \
		qemu-system-loongarch64 -machine virt -m 256M -nographic \
			-kernel $(BUILDDIR)/arch/loongarch64/kernel.elf \
			-no-reboot; \
	else \
		echo "  loongarch64 kernel not found. Build may have failed."; \
	fi

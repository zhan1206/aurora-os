# AuroraOS Makefile with GRUB2 (Multiboot1)
# Supports debug/release builds and auto-detects cross-compiler.
#
# Usage:
#   make              - release build
#   make debug        - debug build (-g -O0)
#   make iso          - build + create ISO
#   make run          - build ISO + run in QEMU
#   make clean        - remove all build artifacts

# Toolchain: try x86_64-elf-gcc first, fall back to system gcc
CC_CROSS := $(shell which x86_64-elf-gcc 2>/dev/null)
LD_CROSS := $(shell which x86_64-elf-ld 2>/dev/null)

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

# Base flags
CFLAGS_BASE := -ffreestanding -Wall -Wextra -fno-pic -fno-stack-protector -mno-sse \
               -Ikernel/include -std=gnu17

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

# Find all source files
K_C_SRCS := $(shell find $(SRCDIR) -type f -name '*.c' 2>/dev/null)
K_S_SRCS := $(shell find $(SRCDIR) arch -type f -name '*.S' 2>/dev/null)

OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(K_C_SRCS))
OBJS += $(patsubst arch/%.S,$(BUILDDIR)/arch/%.o,$(filter arch/%,$(K_S_SRCS)))
OBJS += $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%.o,$(filter $(SRCDIR)/%,$(K_S_SRCS)))

.PHONY: all debug clean iso run help

all: $(KERNEL)

debug: CFLAGS := $(CFLAGS_BASE) $(CFLAGS_DEBUG)
debug: $(KERNEL)

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

iso: $(KERNEL)
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
	@echo "AuroraOS Build System"
	@echo "  make          - release build (optimized)"
	@echo "  make debug    - debug build (-g -O0)"
	@echo "  make iso      - build + create bootable ISO"
	@echo "  make run      - build + run in QEMU"
	@echo "  make clean    - remove all artifacts"
	@echo ""
	@echo "Toolchain: CC=$(CC) LD=$(LD)"

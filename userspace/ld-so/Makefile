# LDSO (v4.2.6)
# AuroraOS Dynamic Linker (ld.so) Makefile
# Builds a position-independent shared library that serves as the
# user-space ELF interpreter for dynamically linked executables.
#
# Usage:
#   make -f Makefile          - build ld.so
#   make -f Makefile clean    - remove build artifacts

CC       := x86_64-linux-gnu-gcc
LD       := x86_64-linux-gnu-ld
STRIP    := x86_64-linux-gnu-strip

# Fall back to native tools if cross-compiler not found
ifeq ($(shell which $(CC) 2>/dev/null),)
  CC := gcc
endif
ifeq ($(shell which $(LD) 2>/dev/null),)
  LD := ld
endif
ifeq ($(shell which $(STRIP) 2>/dev/null),)
  STRIP := strip
endif

# Build flags: position-independent, freestanding, optimized
CFLAGS  := -fPIC -ffreestanding -nostdlib -fno-stack-protector \
           -mno-sse -mgeneral-regs-only -mno-red-zone -O2 -std=gnu17 \
           -I. -Wall -Wextra
LDFLAGS := -nostdlib -shared -Bsymbolic

TARGET  := ld.so
SRCS    := ld-so.c
OBJS    := ld-so.o

.PHONY: all clean

all: $(TARGET)

ld-so.o: ld-so.c ld-so.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	$(STRIP) --strip-all $@ 2>/dev/null || true

clean:
	rm -f $(OBJS) $(TARGET)
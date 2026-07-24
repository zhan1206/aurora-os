/*
 * aslr.h - Address Space Layout Randomization definitions
 *
 * Provides randomized base addresses for mmap, stack, and other
 * user-space memory regions to mitigate memory corruption exploits.
 *
 * Randomization uses a ChaCha20-based CSPRNG seeded at boot time
 * from multiple entropy sources (TSC + RDRAND if available).
 * The PRNG is also exposed via chacha20_random_bytes() for use
 * by other kernel subsystems (e.g., sys_getrandom).
 *
 * FIXED (v4.1.9): Added KASLR (Kernel ASLR) support for randomizing
 * kernel heap base and module load addresses.  (H-30: KASLR)
 */
#ifndef ASLR_H
#define ASLR_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * ASLR constants
 * ================================================================ */

/* Base address for mmap allocations (0x70000000000 = 7TB) */
#define ASLR_MMAP_BASE    0x70000000000ULL

/* Base address for user stack (0x7FFFF0000000 = 128TB - 1GB) */
#define ASLR_STACK_BASE   0x7FFFF0000000ULL

/* Maximum random shift: 1GB (0x40000000) */
#define ASLR_MAX_SHIFT    0x40000000ULL

/* ================================================================
 * KASLR constants
 * ================================================================ */

/*
 * KASLR slide granularity: 2MB (matching x86_64 huge page size).
 * The kernel slide offset is a random multiple of 2MB within the
 * range [0, KASLR_MAX_SLIDE].
 */
#define KASLR_SLIDE_GRANULARITY  0x200000ULL   /* 2MB */
#define KASLR_MAX_SLIDE          0x40000000ULL  /* 1GB */

/* KASLR (v4.2.6) — Full Kernel Address Space Layout Randomization */

/*
 * KASLR kernel text offset: 2MB-aligned within [KASLR_TEXT_OFFSET_MIN,
 * KASLR_TEXT_OFFSET_MAX).  This provides ~21 bits of entropy for the
 * kernel's virtual address base.  The offset is generated at boot from
 * multi-source entropy (TSC + RDRAND + CPUID) and applied to kernel
 * page table entries.
 */
#define KASLR_TEXT_OFFSET_MIN    0x200000ULL        /* 2MB */
#define KASLR_TEXT_OFFSET_MAX    0x2000000000ULL    /* 128GB, ~16 bits */

/*
 * KASLR kernel stack random padding: up to 8 pages of random gap
 * between the kernel stack guard page and the actual stack base.
 * This makes stack-based exploits harder by randomizing the stack
 * address for each kernel task.
 */
#define KASLR_STACK_PAD_PAGES    8

/*
 * KASLR kernel heap random range: up to 512MB of randomization
 * for the kernel heap (slab allocator) base address.
 */
#define KASLR_HEAP_MAX_RANGE     0x20000000ULL  /* 512MB */

/*
 * KASLR module random range: up to 2GB of randomization for
 * kernel module load addresses.
 */
#define KASLR_MODULE_MAX_RANGE   0x80000000ULL  /* 2GB */

/*
 * KASLR direct mapping randomization: randomize the physical memory
 * direct mapping (identity mapping) base by up to 1GB, 2MB-aligned.
 */
#define KASLR_DIRECT_MAP_MAX     0x40000000ULL  /* 1GB */

/* ================================================================
 * API
 * ================================================================ */

/*
 * aslr_init: Seed the PRNG using RDTSC or PIT ticks.
 * Must be called early in boot, after memory init.
 */
void aslr_init(void);

/*
 * kaslr_init: Initialize Kernel ASLR.
 * Generates a random kernel slide offset and applies it to kernel
 * data structures.  Must be called after aslr_init() and memory init.
 *
 * FIXED (v4.1.9): Kernel Address Space Layout Randomization.
 * Randomizes the kernel heap base and module load addresses to
 * mitigate kernel heap spray and ROP attacks.  (H-30)
 */
void kaslr_init(void);

/*
 * kaslr_get_slide: Return the current kernel slide offset.
 * Returns 0 if KASLR is not initialized.
 */
uint64_t kaslr_get_slide(void);

/*
 * kaslr_apply_slide: Apply the kernel slide to a base address.
 * Returns base + slide if KASLR is active, base otherwise.
 */
uint64_t kaslr_apply_slide(uint64_t base);

/*
 * aslr_randomize_base: Add a random offset to a base address.
 * @base:       Base address to randomize.
 * @max_shift:  Maximum random offset (must be page-aligned).
 * Returns:     Randomized address (page-aligned).
 */
uint64_t aslr_randomize_base(uint64_t base, uint64_t max_shift);

/*
 * aslr_randomize_stack: Apply ASLR to the user stack base.
 * Should be called during user task creation.
 * Returns the randomized stack top address.
 */
uint64_t aslr_randomize_stack(void);

/*
 * aslr_randomize_mmap: Return a randomized mmap base address.
 * Should be called by sys_mmap for anonymous mappings.
 * Returns a page-aligned address within the mmap region.
 */
uint64_t aslr_randomize_mmap(void);

/*
 * chacha20_random_bytes: Fill @len bytes at @out with cryptographically
 * secure random data from the ChaCha20 CSPRNG.  Thread-safe (internal
 * spinlock protects the global PRNG state).
 *
 * Returns 0 on success, -1 on error (invalid parameters).
 */
int chacha20_random_bytes(uint8_t *out, size_t len);

/*
 * aslr_prng_name: Return a human-readable string identifying the
 * PRNG algorithm in use (e.g., "ChaCha20 CSPRNG").
 */
const char *aslr_prng_name(void);

/* ================================================================
 * KASLR (v4.2.6) — Full Kernel Address Space Layout Randomization
 * ================================================================ */

/*
 * kaslr_offset: Global kernel text offset, generated at boot.
 * 2MB-aligned random value applied to kernel virtual addresses.
 * Set by kaslr_init() and never modified afterwards.
 */
extern uint64_t kaslr_offset;

/*
 * kaslr_apply: Apply the kernel text offset to a virtual address.
 * Used when computing kernel virtual addresses from physical
 * addresses in the direct-mapped range.
 */
static inline uint64_t kaslr_apply(uint64_t addr) {
    return addr + kaslr_offset;
}

/*
 * kaslr_randomize_stack: Add random padding to the kernel stack
 * base for the current task.  Returns a random offset (0 to
 * KASLR_STACK_PAD_PAGES * PAGE_SIZE) to be subtracted from the
 * kernel stack base.
 */
uint64_t kaslr_randomize_stack(void);

/*
 * kaslr_randomize_heap: Return a randomized offset for the kernel
 * heap (slab allocator) base.  Uses the ChaCha20 CSPRNG with
 * the KASLR_HEAP_MAX_RANGE.
 */
uint64_t kaslr_randomize_heap(void);

/*
 * kaslr_randomize_module: Return a randomized base address for
 * loading a kernel module.  @base is the minimum load address,
 * @max_range is the maximum randomization range.
 * Uses the ChaCha20 CSPRNG for cryptographically secure offsets.
 */
uint64_t kaslr_randomize_module(uint64_t base, uint64_t max_range);

/*
 * kaslr_relocate_kernel: Adjust the kernel page table entries
 * by the current kaslr_offset.  This shifts the kernel's virtual
 * address space by the random offset, making ROP/JOP gadgets
 * unpredictable.  Must be called after kaslr_init() and before
 * any user-space tasks are created.
 */
void kaslr_relocate_kernel(void);

#endif /* ASLR_H */
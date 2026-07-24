/*
 * arch.h - Architecture abstraction layer
 *
 * Provides architecture-independent macros and inline functions.
 * x86_64 is the primary build target; multi-architecture code for
 * riscv64, aarch64, and loongarch64 is prepared.
 *
 * The ARCH_* macros are set by the build system (-D flag).  When no
 * architecture is specified, x86_64 is assumed as the default.
 */
#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <stdint.h>

/* ================================================================
 * Architecture selection
 * ================================================================ */
#if !defined(ARCH_X86_64) && !defined(ARCH_RISCV64) && \
    !defined(ARCH_AARCH64) && !defined(ARCH_LOONGARCH64)
#define ARCH_X86_64
#endif

/* ================================================================
 * Memory barrier
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_mfence(void) {
    asm volatile ("mfence" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_mfence(void) {
    asm volatile ("fence iorw, iorw" ::: "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_mfence(void) {
    asm volatile ("dmb ish" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_mfence(void) {
    asm volatile ("dbar 0" ::: "memory");
}
#endif

/* ================================================================
 * Halt instruction
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_halt(void) {
    asm volatile ("hlt" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_halt(void) {
    asm volatile ("wfi" ::: "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_halt(void) {
    asm volatile ("wfi" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_halt(void) {
    asm volatile ("idle 0" ::: "memory");
}
#endif

/* ================================================================
 * Interrupt control
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_disable_irq(void) {
    asm volatile ("cli" ::: "memory");
}
static inline void arch_enable_irq(void) {
    asm volatile ("sti" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_disable_irq(void) {
    asm volatile ("csrc sstatus, %0" : : "i"(1 << 1) : "memory");
}
static inline void arch_enable_irq(void) {
    asm volatile ("csrs sstatus, %0" : : "i"(1 << 1) : "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_disable_irq(void) {
    asm volatile ("msr daifset, #2" ::: "memory");
}
static inline void arch_enable_irq(void) {
    asm volatile ("msr daifclr, #2" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_disable_irq(void) {
    uint64_t val;
    asm volatile ("csrrd %0, 0x0" : "=r"(val));
    val &= ~(1ULL << 2);
    asm volatile ("csrwr %0, 0x0" : : "r"(val) : "memory");
}
static inline void arch_enable_irq(void) {
    uint64_t val;
    asm volatile ("csrrd %0, 0x0" : "=r"(val));
    val |= (1ULL << 2);
    asm volatile ("csrwr %0, 0x0" : : "r"(val) : "memory");
}
#endif

/* ================================================================
 * Get stack pointer
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("mov %%rsp, %0" : "=r"(sp));
    return sp;
}
#elif defined(ARCH_RISCV64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("mv %0, sp" : "=r"(sp));
    return sp;
}
#elif defined(ARCH_AARCH64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("mov %0, sp" : "=r"(sp));
    return sp;
}
#elif defined(ARCH_LOONGARCH64)
static inline uint64_t arch_get_sp(void) {
    uint64_t sp;
    asm volatile ("or %0, $sp, $zero" : "=r"(sp));
    return sp;
}
#endif

/* ================================================================
 * Cache flush
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_cache_flush(void) {
    asm volatile ("wbinvd" ::: "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_cache_flush(void) {
    asm volatile ("fence.i" ::: "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_cache_flush(void) {
    asm volatile ("ic iallu; dsb ish; isb" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_cache_flush(void) {
    asm volatile ("dbar 0; ibar 0" ::: "memory");
}
#endif

/* ================================================================
 * TLB flush
 * /* MULTIARCH (v4.2.6) */
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline void arch_tlb_flush(uint64_t va) {
    asm volatile ("invlpg (%0)" : : "r"(va) : "memory");
}
static inline void arch_tlb_flush_all(void) {
    uint64_t tmp;
    asm volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(tmp) : : "memory");
}
#elif defined(ARCH_RISCV64)
static inline void arch_tlb_flush(uint64_t va) {
    asm volatile ("sfence.vma %0, zero" : : "r"(va) : "memory");
}
static inline void arch_tlb_flush_all(void) {
    asm volatile ("sfence.vma zero, zero" ::: "memory");
}
#elif defined(ARCH_AARCH64)
static inline void arch_tlb_flush(uint64_t va) {
    asm volatile ("tlbi vae1, %0; dsb ish; isb" : : "r"(va >> 12) : "memory");
}
static inline void arch_tlb_flush_all(void) {
    asm volatile ("tlbi vmalle1; dsb ish; isb" ::: "memory");
}
#elif defined(ARCH_LOONGARCH64)
static inline void arch_tlb_flush(uint64_t va) {
    (void)va;
    asm volatile ("invtlb 0x6, $zero, %0" : : "r"(va) : "memory");
}
static inline void arch_tlb_flush_all(void) {
    asm volatile ("invtlb 0x7, $zero, $zero" ::: "memory");
}
#endif

/* ================================================================
 * CPU ID
 * ================================================================ */
#if defined(ARCH_X86_64)
static inline uint32_t arch_get_cpu_id(void) {
    uint32_t id;
    asm volatile ("movl %%gs:0, %0" : "=r"(id));
    return id;
}
#elif defined(ARCH_RISCV64)
static inline uint64_t arch_get_cpu_id(void) {
    uint64_t id;
    asm volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}
#elif defined(ARCH_AARCH64)
static inline uint64_t arch_get_cpu_id(void) {
    uint64_t id;
    asm volatile ("mrs %0, mpidr_el1" : "=r"(id));
    return id & 0xFF;
}
#elif defined(ARCH_LOONGARCH64)
static inline uint64_t arch_get_cpu_id(void) {
    return 0; /* Stub: single-core for now */
}
#endif

/* ================================================================
 * Arch-specific function declarations (implemented in arch/xxx/arch_init.c)
 * ================================================================ */

/* Called once during early boot, before kmain().
 * Sets up console, MMU, and interrupt controller. */
void arch_early_init(void);

/* Enable the MMU for the current address space. */
void arch_setup_mmu(void);

/* Create a new page table root. Returns physical address of the root. */
uint64_t arch_page_table_create(void);

/* Map a virtual address to a physical address in the given page table.
 * Returns 0 on success, negative on error. */
int arch_page_table_map(uint64_t root_phys, uint64_t vaddr, uint64_t paddr,
                        uint64_t size, uint64_t flags);

/* Unmap a virtual address range. */
void arch_page_table_unmap(uint64_t root_phys, uint64_t vaddr, uint64_t size);

/* ================================================================
 * Convenience wrappers matching the naming convention
 * ================================================================ */
#define arch_irq_enable()    arch_enable_irq()
#define arch_irq_disable()   arch_disable_irq()
#define arch_memory_barrier() arch_mfence()

#endif /* KERNEL_ARCH_H */
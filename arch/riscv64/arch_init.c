/*
 * arch/riscv64/arch_init.c - RISC-V 64-bit early initialization
 *
 * Sets up SBI console, Sv39 page tables, and enables the MMU.
 * Called from boot.S before kmain().
 *
 * /* MULTIARCH (v4.2.6) */
 *
 * /* STUB (v4.2.8): Boot stub only. Memory management, scheduling, and
 * filesystem initialization are not yet implemented for this architecture.
 * Only x86_64 is fully functional. The kmain entry point halts
 * immediately after arch_early_init() because no kernel subsystems
 * are initialized for RISC-V. */
 */
#include <stdint.h>
#include "sbi.h"
#include "pagetable.h"

/* Identity-map 1 GiB starting at 0x80000000 using 2 MiB megapages */
#define IDMAP_START   0x80000000ULL
#define IDMAP_SIZE    0x40000000ULL  /* 1 GiB */
#define MEGAPAGE_SIZE 0x200000ULL    /* 2 MiB */

/* Page table root (Sv39: 4 KiB, 512 entries per level) */
static uint64_t __attribute__((aligned(4096))) sv39_root[512];
static uint64_t __attribute__((aligned(4096))) sv39_l1[512];

void arch_early_init(void)
{
    uint64_t satp_val;

    /* Clear page tables */
    for (int i = 0; i < 512; i++) {
        sv39_root[i] = 0;
        sv39_l1[i] = 0;
    }

    /* Build identity mapping: 2 MiB megapages covering 0x80000000..0xC0000000 */
    uint64_t vpn2 = (IDMAP_START >> 30) & 0x1FF;
    uint64_t paddr = IDMAP_START;
    uint64_t l1_phys = (uint64_t)(uintptr_t)sv39_l1;

    sv39_root[vpn2] = ((l1_phys >> 12) << 10) | PTE_V;

    for (uint64_t off = 0; off < IDMAP_SIZE; off += MEGAPAGE_SIZE) {
        uint64_t vpn1 = ((IDMAP_START + off) >> 21) & 0x1FF;
        sv39_l1[vpn1] = ((paddr + off) >> 12 << 10) | PTE_V | PTE_R | PTE_W | PTE_X;
    }

    /* Set up SATP: Sv39 mode, ASID 0, root page table */
    satp_val = MAKE_SATP(((uint64_t)(uintptr_t)sv39_root) >> 12, 0, SATP_MODE_SV39);
    satp_write(satp_val);

    /* Flush TLB */
    sfence_vma();

    /* SBI console is available via ecall — no additional setup needed */
}

void arch_setup_mmu(void)
{
    /* Already done in arch_early_init() */
}

uint64_t arch_page_table_create(void)
{
    /* Allocate a new root page table (4 KiB aligned) */
    /* Stub: returns 0 for now; full implementation needs a physical allocator */
    (void)sv39_root;
    return 0;
}

int arch_page_table_map(uint64_t root_phys, uint64_t vaddr, uint64_t paddr,
                         uint64_t size, uint64_t flags)
{
    (void)root_phys; (void)vaddr; (void)paddr; (void)size; (void)flags;
    return -1; /* Stub */
}

void arch_page_table_unmap(uint64_t root_phys, uint64_t vaddr, uint64_t size)
{
    (void)root_phys; (void)vaddr; (void)size;
}
/*
 * arch/aarch64/arch_init.c - AArch64 early initialization
 *
 * Sets up TTBR0/TTBR1, enables the MMU, and initializes GIC.
 * Called from boot.S before kmain().
 *
 * /* MULTIARCH (v4.2.6) */
 */
#include <stdint.h>
#include "pagetable.h"
#include "gic.h"

/* Identity-map 1 GiB starting at 0x40000000 using 2 MiB blocks */
#define IDMAP_START   0x40000000ULL
#define IDMAP_SIZE    0x40000000ULL  /* 1 GiB */
#define BLOCK_SIZE    0x200000ULL    /* 2 MiB */

/* Page table root (Level 0: 512 entries, 4 KiB each) */
static uint64_t __attribute__((aligned(4096))) tt_l0[512];
static uint64_t __attribute__((aligned(4096))) tt_l1[512];

void arch_early_init(void)
{
    uint64_t tcr;

    /* Clear page tables */
    for (int i = 0; i < 512; i++) {
        tt_l0[i] = 0;
        tt_l1[i] = 0;
    }

    /* Build Level 1 block entries covering 0x40000000..0x80000000 */
    uint64_t l1_idx = (IDMAP_START >> 30) & 0x1FF;
    uint64_t l1_phys = (uint64_t)(uintptr_t)tt_l1;

    /* Level 0 entry: point to Level 1 table */
    tt_l0[l1_idx] = l1_phys | DESC_TABLE;

    /* Level 1 block entries (2 MiB each) */
    for (uint64_t off = 0; off < IDMAP_SIZE; off += BLOCK_SIZE) {
        uint64_t idx = ((IDMAP_START + off) >> 21) & 0x1FF;
        uint64_t pa = IDMAP_START + off;
        tt_l1[idx] = (pa & ~0x1FFFFFULL)  /* Physical address */
                   | DESC_BLOCK            /* Block descriptor */
                   | DESC_ATTR_AF          /* Access flag */
                   | DESC_ATTR_SH_INNER    /* Inner shareable */
                   | (MAIR_NORMAL_WB << 2) /* Normal memory, Write-Back */
                   | DESC_ATTR_AP_RW;      /* Read-Write */
    }

    /* Set up MAIR_EL1 */
    write_mair_el1(MAIR_DEFAULT);

    /* Set up TCR_EL1 */
    tcr = TCR_DEFAULT;
    write_tcr_el1(tcr);

    /* Set TTBR0_EL1 to the root page table */
    write_ttbr0_el1((uint64_t)(uintptr_t)tt_l0);
    write_ttbr1_el1((uint64_t)(uintptr_t)tt_l0);

    /* Flush TLB */
    tlbi_vmalle1();

    /* Enable MMU, caches */
    {
        uint64_t sctlr;
        asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
        asm volatile ("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
        asm volatile ("isb" ::: "memory");
    }

    /* Initialize GIC (stub: GIC base address is platform-specific) */
    /* GICv2 distributor base is typically at 0x08000000 on QEMU virt */
    /* GICv2 CPU interface base is at 0x08010000 */
    {
        uintptr_t gicd_base = 0x08000000ULL;
        uintptr_t gicc_base = 0x08010000ULL;

        /* Disable and enable GIC distributor */
        gicd_write(gicd_base, GICD_CTLR, 0);
        gicd_write(gicd_base, GICD_CTLR, GICD_CTLR_ENABLE);

        /* Enable GIC CPU interface */
        gicc_write(gicc_base, GICC_PMR, 0xFF);  /* Allow all priorities */
        gicc_write(gicc_base, GICC_CTLR, GICC_CTLR_ENABLE);
    }
}

void arch_setup_mmu(void)
{
    /* Already done in arch_early_init() */
}

uint64_t arch_page_table_create(void)
{
    return 0; /* Stub */
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
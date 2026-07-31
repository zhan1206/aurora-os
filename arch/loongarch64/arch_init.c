/*
 * arch/loongarch64/arch_init.c - LoongArch 64-bit early initialization
 *
 * Sets up CSR registers and enables the MMU with direct mapping.
 * Called from boot.S before kmain().
 *
 * // MULTIARCH (v4.2.6)
 *
 * // STUB (v4.2.8): Boot stub only. Memory management, scheduling, and
 * filesystem initialization are not yet implemented for this architecture.
 * Only x86_64 is fully functional. The kmain entry point halts
 * immediately after arch_early_init() because no kernel subsystems
 * are initialized for LoongArch. */
 */
#include <stdint.h>
#include "csr.h"

/* FIXED (v4.3.7): BUG-07 — Guard with arch-specific ifdef */
#ifdef __loongarch__

void arch_early_init(void)
{
    uint64_t crmd;

    /* Read current CRMD */
    crmd = csr_read(CSR_CRMD);

    /* Ensure direct address translation mode (DA=1, PG=0 for now)
     * PG will be set when we have proper page tables. */
    crmd |= CRMD_DA;
    crmd &= ~CRMD_PG;
    csr_write(CSR_CRMD, crmd);

    /* Enable interrupts (IE=1) */
    crmd = csr_read(CSR_CRMD);
    crmd |= CRMD_IE;
    csr_write(CSR_CRMD, crmd);

    /* Invalidate all TLB entries */
    tlb_inv_all();

    /* Set up basic exception configuration */
    {
        uint64_t ecfg = csr_read(CSR_ECFG);
        ecfg &= ~ECFG_LIE_MASK;  /* Clear all local interrupt enables */
        ecfg |= (1 << 11);       /* Enable IPI interrupt */
        ecfg |= (1 << 12);       /* Enable timer interrupt */
        csr_write(CSR_ECFG, ecfg);
    }
}

void arch_setup_mmu(void)
{
    /* Enable paging (PG=1) when page tables are ready */
    uint64_t crmd = csr_read(CSR_CRMD);
    crmd |= CRMD_PG;
    crmd |= CRMD_DA;  /* Keep DA on for direct mapping window */
    csr_write(CSR_CRMD, crmd);
    tlb_inv_all();
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

#endif /* __loongarch__ */
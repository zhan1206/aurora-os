/*
 * acpi.c - ACPI power management implementation
 * ACPI (v4.2.6)
 *
 * Implements:
 *   - RSDP discovery by scanning EBDA and BIOS ROM areas
 *   - RSDT/XSDT parsing with checksum validation
 *   - MADT parsing for CPU topology (LAPIC, IOAPIC)
 *   - FADT parsing for PM1a_CNT and RESET_REG
 *   - ACPI shutdown (S5) via PM1a_CNT.SLP_EN
 *   - ACPI reboot via RESET_REG with keyboard controller fallback
 *
 * All ACPI tables are identity-mapped (physical = virtual).
 * The kernel identity-maps 0..KERNEL_PHYS_MAX (0x40000000 = 1GB).
 */

#include "acpi.h"
#include "include/log.h"
#include "include/portio.h"
#include "pagetable.h"
#include <stdint.h>

/* ================================================================
 * Globals — cached ACPI tables
 * ================================================================ */
static struct rsdp_descriptor *g_rsdp = NULL;
static struct acpi_sdt_header  *g_rsdt = NULL;  /* raw SDT header (RSDT or XSDT) */
static struct acpi_madt        *g_madt = NULL;
static struct acpi_fadt        *g_fadt = NULL;
static int g_use_xsdt = 0;    /* 1 = using XSDT (64-bit), 0 = RSDT (32-bit) */

/* ================================================================
 * acpi_checksum: Validate ACPI checksum (sum of all bytes == 0)
 * ACPI (v4.2.6)
 * ================================================================ */
int acpi_checksum(void *table, uint32_t length) {
    uint8_t sum = 0;
    uint8_t *ptr = (uint8_t *)table;
    for (uint32_t i = 0; i < length; i++) {
        sum += ptr[i];
    }
    return (sum == 0);
}

/* ================================================================
 * RSDP search: scan memory for "RSD PTR " signature
 * ACPI (v4.2.6)
 *
 * Search order:
 *   1. EBDA (Extended BIOS Data Area): 0x80000 - 0x9FFFF
 *   2. BIOS ROM area: 0xE0000 - 0xFFFFF
 *
 * RSDP is always on a 16-byte boundary.
 * ================================================================ */
static struct rsdp_descriptor *acpi_scan_rsdp(void) {
    uint32_t scan_start, scan_end;

    /* Region 1: EBDA (0x80000 - 0x9FFFF) */
    scan_start = 0x00080000;
    scan_end   = 0x000A0000;
    for (uint32_t addr = scan_start; addr < scan_end; addr += 16) {
        if (addr >= KERNEL_PHYS_MAX) break;
        const char *sig = (const char *)(uintptr_t)addr;
        if (sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' &&
            sig[3] == ' ' && sig[4] == 'P' && sig[5] == 'T' &&
            sig[6] == 'R' && sig[7] == ' ') {
            struct rsdp_descriptor *rsdp = (struct rsdp_descriptor *)(uintptr_t)addr;
            /* Validate RSDP checksum */
            uint32_t csum_len = (rsdp->revision >= 2) ? rsdp->length : 20;
            if (acpi_checksum(rsdp, csum_len)) {
                log_printf(LOG_LEVEL_INFO, "acpi: RSDP found in EBDA at 0x%x\n", addr);
                return rsdp;
            }
        }
    }

    /* Region 2: BIOS ROM area (0xE0000 - 0xFFFFF) */
    scan_start = 0x000E0000;
    scan_end   = 0x00100000;
    for (uint32_t addr = scan_start; addr < scan_end; addr += 16) {
        if (addr >= KERNEL_PHYS_MAX) break;
        const char *sig = (const char *)(uintptr_t)addr;
        if (sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' &&
            sig[3] == ' ' && sig[4] == 'P' && sig[5] == 'T' &&
            sig[6] == 'R' && sig[7] == ' ') {
            struct rsdp_descriptor *rsdp = (struct rsdp_descriptor *)(uintptr_t)addr;
            uint32_t csum_len = (rsdp->revision >= 2) ? rsdp->length : 20;
            if (acpi_checksum(rsdp, csum_len)) {
                log_printf(LOG_LEVEL_INFO, "acpi: RSDP found in BIOS at 0x%x\n", addr);
                return rsdp;
            }
        }
    }

    return NULL;
}

/* ================================================================
 * acpi_find_table: Locate an ACPI table by 4-char signature
 * ACPI (v4.2.6)
 *
 * Iterates RSDT or XSDT entries, matches signature, validates checksum.
 * ================================================================ */
struct acpi_sdt_header *acpi_find_table(const char *sig) {
    if (!g_rsdt) return NULL;

    int num_entries;
    if (g_use_xsdt) {
        struct acpi_xsdt *xsdt = (struct acpi_xsdt *)g_rsdt;
        num_entries = ((int)xsdt->h.length - (int)sizeof(struct acpi_sdt_header)) / 8;
        for (int i = 0; i < num_entries; i++) {
            uint64_t entry = xsdt->entries[i];
            if (entry >= KERNEL_PHYS_MAX) continue;
            struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)entry;
            if (hdr->signature[0] == sig[0] && hdr->signature[1] == sig[1] &&
                hdr->signature[2] == sig[2] && hdr->signature[3] == sig[3]) {
                if (acpi_checksum(hdr, hdr->length)) {
                    return hdr;
                }
            }
        }
    } else {
        struct acpi_rsdt *rsdt = (struct acpi_rsdt *)g_rsdt;
        num_entries = ((int)rsdt->h.length - (int)sizeof(struct acpi_sdt_header)) / 4;
        for (int i = 0; i < num_entries; i++) {
            uint32_t entry = rsdt->entries[i];
            if (entry >= KERNEL_PHYS_MAX) continue;
            struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)entry;
            if (hdr->signature[0] == sig[0] && hdr->signature[1] == sig[1] &&
                hdr->signature[2] == sig[2] && hdr->signature[3] == sig[3]) {
                if (acpi_checksum(hdr, hdr->length)) {
                    return hdr;
                }
            }
        }
    }

    return NULL;
}

/* ================================================================
 * acpi_get_rsdp / acpi_get_madt: Accessors for SMP subsystem
 * ACPI (v4.2.6)
 * ================================================================ */
struct rsdp_descriptor *acpi_get_rsdp(void) {
    return g_rsdp;
}

struct acpi_madt *acpi_get_madt(void) {
    return g_madt;
}

/* ================================================================
 * acpi_init: Initialize ACPI subsystem
 * ACPI (v4.2.6)
 *
 * 1. Scan for RSDP in EBDA and BIOS areas
 * 2. Parse RSDT or XSDT (prefer XSDT for ACPI 2.0+)
 * 3. Locate and cache MADT (APIC) and FADT (FACP)
 * 4. Parse MADT for CPU topology discovery
 * ================================================================ */
void acpi_init(void) {
    if (g_rsdp) {
        log_printf(LOG_LEVEL_INFO, "acpi: already initialized\n");
        return;
    }

    log_printf(LOG_LEVEL_INFO, "acpi: initializing ACPI subsystem...\n");

    /* Step 1: Find RSDP */
    g_rsdp = acpi_scan_rsdp();
    if (!g_rsdp) {
        log_printf(LOG_LEVEL_WARN, "acpi: no RSDP found (ACPI unavailable)\n");
        return;
    }

    log_printf(LOG_LEVEL_INFO, "acpi: RSDP at %p, OEM=%.6s, revision=%d\n",
               (void *)g_rsdp, g_rsdp->oem_id, g_rsdp->revision);

    /* Step 2: Parse RSDT / XSDT */
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address) {
        /* Validate extended checksum for ACPI 2.0+ */
        if (!acpi_checksum(g_rsdp, g_rsdp->length)) {
            log_printf(LOG_LEVEL_WARN, "acpi: RSDP extended checksum failed\n");
        }

        struct acpi_xsdt *xsdt = (struct acpi_xsdt *)(uintptr_t)g_rsdp->xsdt_address;
        if (xsdt->h.length >= sizeof(struct acpi_sdt_header) &&
            acpi_checksum(xsdt, xsdt->h.length)) {
            g_rsdt = (struct acpi_sdt_header *)xsdt;
            g_use_xsdt = 1;
            int num = ((int)xsdt->h.length - (int)sizeof(struct acpi_sdt_header)) / 8;
            log_printf(LOG_LEVEL_INFO, "acpi: XSDT at %p, %d entries\n",
                       (void *)xsdt, num);
        } else {
            log_printf(LOG_LEVEL_WARN, "acpi: XSDT checksum failed, falling back to RSDT\n");
        }
    }

    if (!g_rsdt && g_rsdp->rsdt_address) {
        struct acpi_rsdt *rsdt = (struct acpi_rsdt *)(uintptr_t)g_rsdp->rsdt_address;
        if (rsdt->h.length >= sizeof(struct acpi_sdt_header) &&
            acpi_checksum(rsdt, rsdt->h.length)) {
            g_rsdt = (struct acpi_sdt_header *)rsdt;
            g_use_xsdt = 0;
            int num = ((int)rsdt->h.length - (int)sizeof(struct acpi_sdt_header)) / 4;
            log_printf(LOG_LEVEL_INFO, "acpi: RSDT at %p, %d entries\n",
                       (void *)rsdt, num);
        } else {
            log_printf(LOG_LEVEL_ERR, "acpi: RSDT checksum failed\n");
            g_rsdp = NULL;
            return;
        }
    }

    if (!g_rsdt) {
        log_printf(LOG_LEVEL_ERR, "acpi: no valid RSDT or XSDT found\n");
        g_rsdp = NULL;
        return;
    }

    /* Step 3: Locate MADT (APIC) */
    struct acpi_sdt_header *madt_hdr = acpi_find_table("APIC");
    if (madt_hdr) {
        g_madt = (struct acpi_madt *)madt_hdr;
        log_printf(LOG_LEVEL_INFO, "acpi: MADT at %p, LAPIC base=0x%x, flags=0x%x\n",
                   (void *)g_madt, g_madt->lapic_address, g_madt->flags);
    } else {
        log_printf(LOG_LEVEL_WARN, "acpi: MADT not found\n");
    }

    /* Step 4: Locate FADT (FACP) */
    struct acpi_sdt_header *fadt_hdr = acpi_find_table("FACP");
    if (fadt_hdr) {
        g_fadt = (struct acpi_fadt *)fadt_hdr;
        log_printf(LOG_LEVEL_INFO, "acpi: FADT at %p, PM1a_CNT=0x%x, "
                   "PM1b_CNT=0x%x, SCI_INT=%d\n",
                   (void *)g_fadt, g_fadt->pm1a_cnt_blk,
                   g_fadt->pm1b_cnt_blk, g_fadt->sci_int);
    } else {
        log_printf(LOG_LEVEL_WARN, "acpi: FADT not found\n");
    }

    /* Step 5: Parse MADT entries for CPU/IOAPIC discovery */
    if (g_madt) {
        int lapic_count = 0;
        int ioapic_count = 0;
        uint8_t *entry = (uint8_t *)g_madt->entries;
        uint8_t *madt_end = (uint8_t *)g_madt + g_madt->h.length;

        while (entry < madt_end) {
            struct madt_entry_hdr *hdr = (struct madt_entry_hdr *)entry;
            if (hdr->length == 0) break;  /* safety: prevent infinite loop */

            switch (hdr->type) {
            case ACPI_MADT_TYPE_LAPIC: {
                struct madt_lapic_entry *lapic = (struct madt_lapic_entry *)entry;
                log_printf(LOG_LEVEL_DEBUG, "acpi: LAPIC proc_id=%d apic_id=%d "
                           "flags=0x%x\n", lapic->acpi_processor_id,
                           lapic->apic_id, lapic->flags);
                if (lapic->flags & 1) lapic_count++;
                break;
            }
            case ACPI_MADT_TYPE_IOAPIC: {
                struct madt_ioapic_entry *ioapic = (struct madt_ioapic_entry *)entry;
                log_printf(LOG_LEVEL_DEBUG, "acpi: IOAPIC id=%d addr=0x%x "
                           "GSI_base=%d\n", ioapic->ioapic_id,
                           ioapic->ioapic_address,
                           ioapic->global_system_interrupt_base);
                ioapic_count++;
                break;
            }
            case ACPI_MADT_TYPE_ISO: {
                struct madt_iso_entry *iso = (struct madt_iso_entry *)entry;
                log_printf(LOG_LEVEL_DEBUG, "acpi: ISO bus=%d src=%d "
                           "GSI=%d flags=0x%x\n", iso->bus, iso->source,
                           iso->global_system_interrupt, iso->flags);
                break;
            }
            case ACPI_MADT_TYPE_NMI:
                log_printf(LOG_LEVEL_DEBUG, "acpi: NMI entry\n");
                break;
            case ACPI_MADT_TYPE_LAPIC_ADDR_OVERRIDE:
                log_printf(LOG_LEVEL_DEBUG, "acpi: LAPIC address override\n");
                break;
            default:
                log_printf(LOG_LEVEL_DEBUG, "acpi: unknown MADT entry type %d\n",
                           hdr->type);
                break;
            }

            entry += hdr->length;
        }

        log_printf(LOG_LEVEL_INFO, "acpi: MADT parsed: %d LAPIC(s), %d IOAPIC(s)\n",
                   lapic_count, ioapic_count);
    }

    log_printf(LOG_LEVEL_INFO, "acpi: ACPI subsystem initialized\n");
}

/* ================================================================
 * acpi_shutdown: Perform ACPI system shutdown (S5 state)
 * ACPI (v4.2.6)
 *
 * Writes SLP_TYPa | SLP_EN to the PM1a_CNT register.
 * SLP_TYPa is read from the \_S5 object in the DSDT, but we use
 * a common default (0x1C00) for S5 on most hardware.
 *
 * The PM1a_CNT register is an I/O port. Bits:
 *   [12:10] SLP_TYP (3 bits) — sleep type
 *   [13]    SLP_EN        — sleep enable (must be set)
 *
 * Returns: -1 if FADT not available; does not return on success.
 * ================================================================ */
int acpi_shutdown(void) {
    if (!g_fadt) {
        log_printf(LOG_LEVEL_ERR, "acpi: shutdown failed — no FADT\n");
        return -1;
    }

    uint16_t pm1a_cnt = (uint16_t)g_fadt->pm1a_cnt_blk;
    if (pm1a_cnt == 0) {
        log_printf(LOG_LEVEL_ERR, "acpi: shutdown failed — PM1a_CNT is 0\n");
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "acpi: shutting down via PM1a_CNT (port 0x%x)\n",
               pm1a_cnt);

    /*
     * SLP_TYP for S5 is typically 0x7 (111b).  Combined with SLP_EN (bit 13):
     *   0x2000 | 0x1C00 = 0x3C00
     * SLP_TYPa = 0x1C00 (bits 12:10 = 111 = S5)
     * SLP_EN   = 0x2000 (bit 13)
     */
    uint16_t slp_val = ACPI_PM1_SLP_EN | (7 << ACPI_PM1_SLP_TYP_SHIFT);

    /*
     * Also write to PM1b_CNT if available (for dual-chipset systems).
     * PM1a_CNT must be written first, then PM1b_CNT.
     */
    outw(pm1a_cnt, slp_val);

    uint16_t pm1b_cnt = (uint16_t)g_fadt->pm1b_cnt_blk;
    if (pm1b_cnt != 0) {
        outw(pm1b_cnt, slp_val);
    }

    /* Should never reach here */
    log_printf(LOG_LEVEL_ERR, "acpi: shutdown failed — hardware did not respond\n");
    return -1;
}

/* ================================================================
 * write_io_addr: Write a value to an address described by GAS
 * ACPI (v4.2.6)
 *
 * Supports System I/O and System Memory address spaces.
 * ================================================================ */
static void acpi_write_gas(const uint8_t *gas, uint8_t value) {
    struct acpi_gas *g = (struct acpi_gas *)gas;

    if (g->address_space_id == 1) {
        /* System I/O */
        uint16_t port = (uint16_t)(g->address & 0xFFFF);
        if (port) {
            outb(port, value);
        }
    } else if (g->address_space_id == 0) {
        /* System Memory */
        uint64_t addr = g->address;
        if (addr && addr < KERNEL_PHYS_MAX) {
            volatile uint8_t *ptr = (volatile uint8_t *)(uintptr_t)addr;
            *ptr = value;
        }
    }
}

/* ================================================================
 * acpi_reboot: Perform ACPI system reset
 * ACPI (v4.2.6)
 *
 * 1. Try ACPI RESET_REG (from FADT) — write reset_value to reset_reg
 * 2. Fallback: keyboard controller reset (write 0xFE to port 0x64)
 * 3. Last resort: triple fault (load null IDT and trigger interrupt)
 * ================================================================ */
void acpi_reboot(void) {
    log_printf(LOG_LEVEL_INFO, "acpi: rebooting...\n");

    /* Method 1: ACPI reset via RESET_REG */
    if (g_fadt) {
        uint8_t reset_val = g_fadt->reset_value;
        struct acpi_gas *reset_gas = (struct acpi_gas *)g_fadt->reset_reg;

        if (reset_gas->address_space_id != 0 || reset_gas->address != 0) {
            log_printf(LOG_LEVEL_INFO, "acpi: reset via RESET_REG (addr=0x%llx, "
                       "val=0x%x, space=%d)\n",
                       (unsigned long long)reset_gas->address,
                       reset_val, reset_gas->address_space_id);

            acpi_write_gas(g_fadt->reset_reg, reset_val);

            /* Short delay for reset to take effect */
            for (volatile int i = 0; i < 100000; i++) {
                asm volatile ("" ::: "memory");
            }
        }
    }

    /* Method 2: Keyboard controller reset (write 0xFE to port 0x64) */
    log_printf(LOG_LEVEL_INFO, "acpi: keyboard controller reset fallback\n");

    /*
     * Wait for keyboard controller input buffer to be empty.
     * Read port 0x64, test bit 1 (input buffer full).
     * Timeout after ~1 second to avoid infinite loop.
     */
    int timeout = 100000;
    while (timeout > 0) {
        uint8_t status = inb(0x64);
        if ((status & 0x02) == 0) break;
        timeout--;
        asm volatile ("pause" ::: "memory");
    }

    if (timeout > 0) {
        outb(0x64, 0xFE);  /* system reset pulse */
    }

    /* Short delay for reset */
    for (volatile int i = 0; i < 100000; i++) {
        asm volatile ("" ::: "memory");
    }

    /* Method 3: Triple fault (last resort) */
    log_printf(LOG_LEVEL_ERR, "acpi: reset failed, triggering triple fault\n");

    /* Load a null IDT descriptor and trigger an interrupt */
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idtr = { 0, 0 };

    asm volatile (
        "lidt %0\n"
        "int3\n"
        :
        : "m"(null_idtr)
        : "memory"
    );
}
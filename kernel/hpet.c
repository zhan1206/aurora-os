/*
 * hpet.c - High Precision Event Timer implementation
 * FIXED (v4.3.3): HPET-001 — Implement HPET timer.
 * Previously acpi.c could parse HPET table but never used it as a timer.
 * Now implements HPET as a high-precision timer (default: PIT, fallback: HPET).
 */

#include "include/hpet.h"
#include "include/log.h"
#include "acpi.h"
#include "pagetable.h"
#include <stdint.h>

/* FIXED (v4.3.3): HPET-001 — HPET ACPI table structure */
struct acpi_hpet {
    struct acpi_sdt_header h;
    uint32_t event_timer_block_id;
    uint8_t  address_space_id;
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  reserved;
    uint64_t address;
    uint8_t  hpet_number;
    uint16_t minimum_tick;
    uint8_t  page_protection;
} __attribute__((packed));

/* FIXED (v4.3.3): HPET-001 — Global HPET state */
uint64_t hpet_base_addr = 0;
static uint64_t hpet_period_fs = 0;  /* femtoseconds per tick */

/* HPET registers */
#define HPET_CAP     0x00
#define HPET_CFG     0x10
#define HPET_COUNTER 0xF0

/* FIXED (v4.3.3): HPET-001 — Initialize HPET from ACPI table */
int hpet_init(void) {
    /* FIXED (v4.3.3): HPET-001 — Parse HPET from ACPI */
    struct acpi_sdt_header *hdr = acpi_find_table("HPET");
    if (!hdr) {
        log_printf(LOG_LEVEL_INFO, "hpet: no HPET table found, using PIT\n");
        return -1;
    }

    struct acpi_hpet *hpet_tbl = (struct acpi_hpet *)hdr;
    hpet_base_addr = hpet_tbl->address;

    if (!hpet_base_addr) {
        log_printf(LOG_LEVEL_INFO, "hpet: HPET address is 0, using PIT\n");
        return -1;
    }

    if (hpet_base_addr >= KERNEL_PHYS_MAX) {
        log_printf(LOG_LEVEL_WARN, "hpet: HPET address 0x%llx out of identity-mapped range\n",
                   (unsigned long long)hpet_base_addr);
        hpet_base_addr = 0;
        return -1;
    }

    volatile uint64_t *hpet = (volatile uint64_t *)phys_to_virt(hpet_base_addr);

    /* Read capabilities */
    uint64_t cap = hpet[HPET_CAP / 8];
    hpet_period_fs = (cap >> 32) & 0xFFFFFFFF;
    uint8_t num_timers = ((cap >> 8) & 0x1F) + 1;
    log_printf(LOG_LEVEL_INFO, "hpet: period=%lu fs, timers=%u\n",
               hpet_period_fs, num_timers);

    /* Enable HPET */
    hpet[HPET_CFG / 8] |= 0x01;  /* ENABLE_CNF */
    hpet[HPET_COUNTER / 8] = 0;  /* Reset counter */

    log_printf(LOG_LEVEL_INFO, "hpet: initialized at %p\n", (void*)hpet_base_addr);
    return 0;
}

/* FIXED (v4.3.3): HPET-001 — Get HPET counter value */
uint64_t hpet_get_counter(void) {
    if (!hpet_base_addr) return 0;
    volatile uint64_t *hpet = (volatile uint64_t *)phys_to_virt(hpet_base_addr);
    return hpet[HPET_COUNTER / 8];
}

/* FIXED (v4.3.3): HPET-001 — Convert HPET ticks to nanoseconds */
uint64_t hpet_ticks_to_ns(uint64_t ticks) {
    if (!hpet_period_fs) return 0;
    return (ticks * hpet_period_fs) / 1000000;  /* fs -> ns */
}
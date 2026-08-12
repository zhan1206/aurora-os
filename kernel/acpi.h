/*
 * acpi.h - ACPI power management definitions and interface
 * ACPI (v4.2.6)
 *
 * Provides:
 *   - RSDP descriptor and SDT header structures
 *   - MADT (Multiple APIC Description Table) for CPU/IOAPIC discovery
 *   - FADT (Fixed ACPI Description Table) for power management
 *   - acpi_init() / acpi_find_table() / acpi_shutdown() / acpi_reboot()
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>  /* FIXED (v4.4.2): BUILD-02 — for size_t */

/* ================================================================
 * MADT entry types
 * ================================================================ */
#define ACPI_MADT_TYPE_LAPIC             0
#define ACPI_MADT_TYPE_IOAPIC            1
#define ACPI_MADT_TYPE_ISO               2
#define ACPI_MADT_TYPE_NMI               4
#define ACPI_MADT_TYPE_LAPIC_ADDR_OVERRIDE 5

/* ================================================================
 * RSDP (Root System Description Pointer) — ACPI (v4.2.6)
 * ================================================================ */
struct rsdp_descriptor {
    char     signature[8];       /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;       /* 32-bit RSDT pointer */
    /* Extended fields (ACPI 2.0+, revision >= 2) */
    uint32_t length;
    uint64_t xsdt_address;       /* 64-bit XSDT pointer */
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* ================================================================
 * ACPI SDT (System Description Table) header — ACPI (v4.2.6)
 * ================================================================ */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* ================================================================
 * RSDT / XSDT — ACPI (v4.2.6)
 * ================================================================ */
struct acpi_rsdt {
    struct acpi_sdt_header h;
    uint32_t entries[];          /* array of 32-bit SDT pointers */
} __attribute__((packed));

struct acpi_xsdt {
    struct acpi_sdt_header h;
    uint64_t entries[];          /* array of 64-bit SDT pointers */
} __attribute__((packed));

/* ================================================================
 * MADT (Multiple APIC Description Table) — ACPI (v4.2.6)
 * ================================================================ */
struct acpi_madt {
    struct acpi_sdt_header h;
    uint32_t lapic_address;      /* local APIC MMIO base */
    uint32_t flags;              /* bit 0 = PC-AT dual 8259 */
    uint8_t  entries[];          /* variable-length entry list */
} __attribute__((packed));

/* MADT entry header */
struct madt_entry_hdr {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

/* MADT LAPIC (Local APIC) entry */
struct madt_lapic_entry {
    struct madt_entry_hdr h;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;              /* bit 0 = processor enabled */
} __attribute__((packed));

/* MADT IOAPIC entry */
struct madt_ioapic_entry {
    struct madt_entry_hdr h;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed));

/* MADT ISO (Interrupt Source Override) entry */
struct madt_iso_entry {
    struct madt_entry_hdr h;
    uint8_t  bus;
    uint8_t  source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed));

/* ================================================================
 * FADT (Fixed ACPI Description Table) — ACPI (v4.2.6)
 * ================================================================ */
struct acpi_fadt {
    struct acpi_sdt_header h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
    uint16_t boot_arch_flags;
    uint8_t  reserved1;
    uint32_t flags;
    /* 12-byte generic address structures */
    uint8_t  reset_reg[12];      /* Generic Address Structure */
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    uint8_t  x_pm1a_evt_blk[12];
    uint8_t  x_pm1b_evt_blk[12];
    uint8_t  x_pm1a_cnt_blk[12];
    uint8_t  x_pm1b_cnt_blk[12];
    uint8_t  x_pm2_cnt_blk[12];
    uint8_t  x_pm_tmr_blk[12];
    uint8_t  x_gpe0_blk[12];
    uint8_t  x_gpe1_blk[12];
    uint8_t  sleep_control_reg[12];
    uint8_t  sleep_status_reg[12];
    uint64_t hypervisor_id;
} __attribute__((packed));

/* Generic Address Structure (GAS) */
struct acpi_gas {
    uint8_t  address_space_id;   /* 0=SystemMemory, 1=SystemIO, 2=PCI, 3=Embedded, 4=SMBus */
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* PM1a_CNT bit definitions */
#define ACPI_PM1_SLP_EN     (1 << 13)  /* Sleep Enable */
#define ACPI_PM1_SLP_TYP_SHIFT 10
#define ACPI_PM1_SLP_TYP_MASK  (7 << 10)  /* Sleep Type bits */

/* ================================================================
 * ACPI API — ACPI (v4.2.6)
 * ================================================================ */

/*
 * acpi_init: Initialize the ACPI subsystem.
 * Scans for RSDP, parses RSDT/XSDT, caches FADT and MADT.
 * Must be called after phys_mem_init (identity-mapped memory).
 */
void acpi_init(void);

/*
 * acpi_find_table: Locate an ACPI table by its 4-character signature.
 * @sig: signature string (e.g., "APIC", "FACP", "DSDT").
 * Returns: pointer to the table's SDT header, or NULL if not found.
 */
struct acpi_sdt_header *acpi_find_table(const char *sig);

/*
 * acpi_shutdown: Perform ACPI system shutdown (S5 state).
 * Writes SLP_TYPa | SLP_EN to PM1a_CNT.SLP_EN bit.
 * Returns: does not return on success; returns -1 if FADT not found.
 */
int acpi_shutdown(void);

/*
 * acpi_reboot: Perform ACPI system reset.
 * Writes value to RESET_REG if available; falls back to keyboard
 * controller reset (write 0xFE to port 0x64).
 */
void acpi_reboot(void);

/*
 * acpi_get_rsdp: Return the cached RSDP pointer (for SMP use).
 */
struct rsdp_descriptor *acpi_get_rsdp(void);

/*
 * acpi_get_madt: Return the cached MADT pointer (for SMP use).
 */
struct acpi_madt *acpi_get_madt(void);

/*
 * acpi_checksum: Validate an ACPI table checksum.
 * @table: pointer to the table.
 * @length: length of the table in bytes.
 * Returns: 1 if checksum is valid (sum of all bytes == 0), 0 otherwise.
 */
int acpi_checksum(void *table, uint32_t length);

/* ACPI_DSDT (v4.2.7) - DSDT/SSDT _S5 parser */
/*
 * acpi_parse_s5: Scan AML bytecode for the \_S5 object and extract
 * SLP_TYPa and SLP_TYPb values for ACPI shutdown.
 *
 * @aml:          pointer to DSDT or SSDT AML bytecode.
 * @len:          length of the AML bytecode in bytes.
 * @slp_typa_out: output — SLP_TYPa value (for PM1a_CNT).
 * @slp_typb_out: output — SLP_TYPb value (for PM1b_CNT).
 *
 * Returns 0 on success, -1 if _S5 not found or AML is malformed.
 */
int acpi_parse_s5(uint8_t *aml, size_t len, int *slp_typa_out, int *slp_typb_out);

#endif /* ACPI_H */
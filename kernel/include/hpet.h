/*
 * hpet.h - High Precision Event Timer interface
 * FIXED (v4.3.3): HPET-001
 */

#ifndef HPET_H
#define HPET_H

#include <stdint.h>

/* Global HPET base address (set by hpet_init) */
extern uint64_t hpet_base_addr;

/* Initialize HPET timer from ACPI HPET table */
int hpet_init(void);

/* Get current HPET counter value */
uint64_t hpet_get_counter(void);

/* Convert HPET ticks to nanoseconds */
uint64_t hpet_ticks_to_ns(uint64_t ticks);

#endif /* HPET_H */
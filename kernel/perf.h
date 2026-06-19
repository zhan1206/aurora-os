/*
 * perf.h - Performance monitoring definitions for AuroraOS kernel
 *
 * Provides lightweight performance counters for profiling kernel
 * operations. Uses RDTSC-based timing for latency measurements.
 *
 * Inspired by CoolPotOS's IRQ tracking and kernel monitoring.
 */

#ifndef PERF_H
#define PERF_H

#include <stdint.h>

/* ================================================================
 * Perf event types
 * ================================================================ */
enum {
    PERF_CTX_SWITCHES,
    PERF_SYSCALL_COUNT,
    PERF_SYSCALL_LATENCY,
    PERF_PAGE_FAULTS,
    PERF_COW_COUNT,
    PERF_MALLOC_COUNT,
    PERF_FREE_COUNT,
    PERF_IRQ_COUNT,
    PERF_VRUNTIME_UPDATES,
    PERF_MAX
};

/* ================================================================
 * IRQ counter tracking (like CoolPotOS /proc/interrupts)
 * ================================================================ */
#define IRQ_MAX_VECTORS 256
struct irq_counter {
    uint64_t count;
    const char *name;
};

/* ================================================================
 * Perf counter structure
 * ================================================================ */
struct perf_counter {
    uint64_t count;
    uint64_t total_latency;   /* cumulative latency in nanoseconds */
    uint64_t max_latency;     /* maximum observed latency */
    uint64_t min_latency;     /* minimum observed latency */
    const char *name;
};

/* ================================================================
 * Perf global statistics
 * ================================================================ */
struct perf_stats {
    struct perf_counter counters[PERF_MAX];
    uint64_t boot_time;       /* TSC value at boot */
    uint64_t uptime_ticks;    /* cached uptime (updated by timer) */
    struct irq_counter irq_counts[IRQ_MAX_VECTORS];  /* per-IRQ vector counts */
};

/* ================================================================
 * Public API
 * ================================================================ */
void perf_init(void);
void perf_inc(int event);
void perf_add_latency(int event, uint64_t latency_ns);
void perf_dump(void);
void perf_reset(void);
uint64_t perf_get_ticks(void);
uint64_t perf_ticks_to_ns(uint64_t ticks);
uint64_t perf_tsc_to_ns(uint64_t tsc);

/* IRQ counter API */
void perf_irq_inc(int vector, const char *name);
void perf_irq_dump(void);

/* Global perf stats instance */
extern struct perf_stats perf;

#endif /* PERF_H */
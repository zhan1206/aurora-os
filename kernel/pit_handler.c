/*
 * pit_handler.c - PIT interrupt handler (FIXED)
 *
 * Fix: Instead of calling schedule() directly from interrupt context
 * (which breaks the iretq return path), set a need_resched flag.
 * The flag is checked at safe points: after iretq returns to the
 * interrupted context, and in yield() / explicit reschedule points.
 *
 * (Report §5.2 + §9.2: schedule() in interrupt context causes
 *  stack frame mismatch with iretq)
 */
#include "include/log.h"

/* Reschedule flag: set by interrupt handlers, checked at safe points */
volatile int need_resched = 0;

void pit_irq_c_handler(void *rsp) {
    (void)rsp;
    /* Set flag instead of calling schedule() directly.
     * The interrupted code will check need_resched at its next
     * safe point (iretq return path in the IRQ wrapper). */
    need_resched = 1;
}

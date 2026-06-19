/*
 * portio.h - I/O port access primitives
 *
 * Provides inline outb/inb for all kernel components.
 * Eliminates the 5x code duplication across print/irq/keyboard/pit/console.
 */
#ifndef PORTIO_H
#define PORTIO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#endif /* PORTIO_H */

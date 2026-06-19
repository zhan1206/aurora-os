/*
 * kstdio.h - Kernel formatting utilities
 *
 * Provides itoa/uitoa for number formatting, eliminating
 * the duplicated manual itoa in main.c, shell.c, explain.c, panic.c.
 */
#ifndef KSTDIO_H
#define KSTDIO_H

#include <stdint.h>
#include <stddef.h>

/* Convert unsigned integer to decimal string. Returns length. */
static inline int uitoa(uint64_t val, char *buf, size_t bufsz) {
    int n = 0;
    if (val == 0) { if (n < (int)bufsz-1) buf[n++] = '0'; }
    char tmp[21];
    int tn = 0;
    while (val > 0 && tn < 20) { tmp[tn++] = '0' + (int)(val % 10); val /= 10; }
    for (int i = tn - 1; i >= 0 && n < (int)bufsz-1; i--) buf[n++] = tmp[i];
    buf[n] = '\0';
    return n;
}

/* Convert signed integer to decimal string. Returns length. */
static inline int itoa(int val, char *buf, size_t bufsz) {
    int n = 0;
    if (val < 0) { if (n < (int)bufsz-1) buf[n++] = '-'; val = -val; }
    n += uitoa((uint64_t)val, buf + n, bufsz - (size_t)n);
    return n;
}

/* Convert uint64 to hex string (16 chars). Returns pointer to start of hex digits. */
static inline const char *uitoa_hex(uint64_t val, char *buf, size_t bufsz) {
    if (bufsz < 17) return "";
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        uint8_t nib = (val >> (i * 4)) & 0xF;
        buf[15-i] = nib < 10 ? '0' + (char)nib : 'a' + (char)(nib - 10);
    }
    return buf;
}

#endif /* KSTDIO_H */

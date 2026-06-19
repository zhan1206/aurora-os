#include "include/log.h"
#include "include/portio.h"
#include <stdint.h>

void pit_init(uint32_t freq_hz) {
    if (freq_hz == 0) freq_hz = 100;  /* default to 100 Hz */
    uint32_t divisor = 1193180 / freq_hz;
    outb(0x43, 0x36); /* channel 0, lobyte/hibyte, mode 3 */
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

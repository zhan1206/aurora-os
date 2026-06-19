#ifndef IDT_H
#define IDT_H

#include <stdint.h>

void load_idt(void);
void irq_init(void);
void pf_handler_c(uint64_t error_code);

#endif

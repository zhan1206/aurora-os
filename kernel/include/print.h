/*
 * print.h - Serial and VGA output interface
 */
#ifndef PRINT_H
#define PRINT_H

/* Initialize serial port (COM1 at 0x3F8, 38400 8N1) */
void serial_init(void);

/* Print a null-terminated string to both serial and VGA */
void printk(const char *s);

/* Signal that the console is ready for VGA output */
void printk_console_ready(void);

#endif /* PRINT_H */

/*
 * log.h - Kernel logging interface
 */
#ifndef LOG_H
#define LOG_H

#include <stdarg.h>

enum { LOG_LEVEL_ERR = 0, LOG_LEVEL_WARN = 1, LOG_LEVEL_INFO = 2, LOG_LEVEL_DEBUG = 3 };

void log_set_level(int level);
void log_printf(int level, const char *fmt, ...);
void log_vprintf(int level, const char *fmt, va_list ap);

/*
 * log_ring_dump: Iterate over the log ring buffer.
 * Calls emit(c) for each character from oldest to newest.
 * Useful for panic-time dump or syslog export.
 */
void log_ring_dump(void (*emit)(char c));

#endif /* LOG_H */

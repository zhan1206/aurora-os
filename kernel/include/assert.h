#ifndef ASSERT_H
#define ASSERT_H

#include "log.h"

/* Forward declaration */
extern void panic(const char *fmt, ...);

#define ASSERT(cond) do { \
    if (!(cond)) { \
        log_printf(LOG_LEVEL_ERR, "Assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        panic("Assertion failed: %s\n", #cond); \
    } \
} while (0)

#endif /* ASSERT_H */

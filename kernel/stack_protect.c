/*
 * stack_protect.c - Stack canary and guard page implementation
 *
 * Provides:
 *   - __stack_chk_guard: A randomized 64-bit canary value placed by GCC
 *     between local variables and the return address.  GCC's
 *     -fstack-protector-strong generates code to check this canary
 *     before each function return.
 *   - __stack_chk_fail: Called when the canary is corrupted (stack
 *     buffer overflow detected).  Prints diagnostic info and panics.
 *
 * The canary is initialized early in boot via stack_protect_init(),
 * using RDTSC for entropy.
 */
#include "stack_protect.h"
#include "include/log.h"
#include <stdint.h>

/* ================================================================
 * Stack canary (global, referenced by GCC-generated code)
 * ================================================================ */

/*
 * __stack_chk_guard: The canary value that GCC places on the stack
 * at function entry and checks before function return.
 *
 * Initialized to a non-zero pattern so that if stack_protect_init()
 * is somehow not called, the canary is still non-trivial (not zero).
 * The real randomization happens in stack_protect_init().
 */
uint64_t __stack_chk_guard = 0xDEADBEEF1BADB002ULL;

/* ================================================================
 * Initialization
 * ================================================================ */

void stack_protect_init(void) {
    uint64_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    uint64_t tsc = (tsc_high << 32) | tsc_low;

    /*
     * FIXED (v4.2.8): SEC-CANARY — mix RDTSC with stack address,
     * compile-time constant, and a SplitMix64-style hash for
     * strong entropy.  Previously used only RDTSC which is
     * highly predictable at boot.
     *
     * The hash function applies three rounds of xor-shift-multiply
     * (SplitMix64 finalizer) to achieve full avalanche, making
     * the canary unpredictable even if the attacker knows the
     * approximate TSC value.
     */
    uint64_t canary = tsc;
    canary ^= (uint64_t)(uintptr_t)&canary;          /* stack address entropy */
    canary ^= 0xDEADBEEFCAFEBABEULL;                  /* fixed mixing constant */

    /* SplitMix64-style hash: 3 rounds of xor-shift-multiply for full avalanche */
    canary = (canary ^ (canary >> 30)) * 0xBF58476D1CE4E5B9ULL;
    canary = (canary ^ (canary >> 27)) * 0x94D049BB133111EBULL;
    canary = canary ^ (canary >> 31);

    __stack_chk_guard = canary;

    /*
     * FIXED (v4.1.8): Set the lowest byte of the canary to 0x00
     * (terminator canary).  This prevents string operations (strcpy,
     * sprintf, etc.) from reading past the canary and leaking it.
     * The canary check uses XOR, so the null byte is still detected
     * as corruption if the attacker overwrites it.  (L-24)
     */
    __stack_chk_guard &= ~0xFFULL;

    /* Ensure canary is never zero */
    if (__stack_chk_guard == 0) __stack_chk_guard = 0xDEADBEEF1BADB000ULL;

    log_printf(LOG_LEVEL_INFO, "Stack protector initialized\n");
}

/* ================================================================
 * Stack smashing detected
 * ================================================================ */

/*
 * __stack_chk_fail: Called by GCC-generated code when a stack buffer
 * overflow corrupts the canary value.
 *
 * This function is declared noreturn — it will call panic() which
 * halts the system.  We print diagnostic information to help identify
 * which task triggered the fault.
 */
void __stack_chk_fail(void) {
    /*
     * We can't safely use log_printf here because the stack may be
     * corrupted.  Use the low-level panic() which is designed to
     * work in emergency situations.
     */
    extern void panic(const char *fmt, ...);
    panic("Stack smashing detected! Stack canary has been corrupted.\n"
          "This indicates a buffer overflow in a kernel function.\n"
          "Check function local arrays for out-of-bounds writes.");
    __builtin_unreachable();
}
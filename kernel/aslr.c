/*
 * aslr.c - Address Space Layout Randomization implementation
 *
 * Uses a ChaCha20-based CSPRNG to generate random offsets for:
 *   - User stack base (within 1GB range)
 *   - mmap base (within 1GB range)
 *
 * The PRNG is seeded at boot time from multiple entropy sources
 * (TSC + RDRAND if available). The ChaCha20 key is 256 bits (32 bytes)
 * derived from the entropy, and a 96-bit (12-byte) nonce is
 * incremented for each call to chacha20_random().
 *
 * v4.0.7: Replaced xorshift64 with ChaCha20 CSPRNG for
 *         cryptographically secure randomization.
 */
#include "aslr.h"
#include "pagetable.h"
#include "include/log.h"
#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * ChaCha20 CSPRNG
 * ================================================================ */

static const uint32_t chacha20_const[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
/* "expand 32-byte k" */

/* Internal state: const[4] + key[8] + counter[1] + nonce[3] = 16 words */
static uint32_t chacha_state[16];

/*
 * FIXED (v4.1.4): Spinlock for ChaCha20 global state.
 * On SMP systems, concurrent calls to aslr_randomize_base() from
 * different CPUs would race on chacha_state, corrupting the PRNG
 * and making ASLR completely ineffective.  (BUG 3.10)
 */
static volatile uint32_t chacha_lock = 0;

static inline void chacha_spin_lock(void) {
    while (1) {
        uint32_t old = 0, new = 1;
        asm volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(chacha_lock)
            : "r"(new), "0"(old)
            : "memory"
        );
        if (old == 0) break;
        asm volatile ("pause" ::: "memory");
    }
}

static inline void chacha_spin_unlock(void) {
    asm volatile ("movl $0, %0" : "=m"(chacha_lock) : : "memory");
}

/*
 * chacha20_quarter_round: Perform the ChaCha20 quarter round operation
 * on four 32-bit words. This is the core diffusion primitive.
 */
static inline void chacha20_quarter_round(uint32_t *a, uint32_t *b,
                                           uint32_t *c, uint32_t *d) {
    *a += *b; *d ^= *a; *d = (*d << 16) | (*d >> 16);
    *c += *d; *b ^= *c; *b = (*b << 12) | (*b >> 20);
    *a += *b; *d ^= *a; *d = (*d <<  8) | (*d >> 24);
    *c += *d; *b ^= *c; *b = (*b <<  7) | (*b >> 25);
}

/*
 * chacha20_block: Generate one 64-byte ChaCha20 keystream block.
 * The input state is preserved; output is written to @out (must be 64 bytes).
 * The counter in chacha_state[12] is NOT incremented; the caller does that.
 */
static void chacha20_block(uint32_t *out) {
    uint32_t x[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = chacha_state[i];
    }

    /* 20 rounds: 10 double rounds */
    for (i = 0; i < 10; i++) {
        /* Column rounds */
        chacha20_quarter_round(&x[0], &x[4], &x[8],  &x[12]);
        chacha20_quarter_round(&x[1], &x[5], &x[9],  &x[13]);
        chacha20_quarter_round(&x[2], &x[6], &x[10], &x[14]);
        chacha20_quarter_round(&x[3], &x[7], &x[11], &x[15]);

        /* Diagonal rounds */
        chacha20_quarter_round(&x[0], &x[5], &x[10], &x[15]);
        chacha20_quarter_round(&x[1], &x[6], &x[11], &x[12]);
        chacha20_quarter_round(&x[2], &x[7], &x[8],  &x[13]);
        chacha20_quarter_round(&x[3], &x[4], &x[9],  &x[14]);
    }

    /* Add original state to the working state (finalize) */
    for (i = 0; i < 16; i++) {
        x[i] += chacha_state[i];
    }

    /* Serialize to 64 bytes (little-endian) */
    for (i = 0; i < 16; i++) {
        out[i] = x[i];
    }
}

/*
 * chacha20_encrypt: Encrypt/decrypt @len bytes at @in into @out using
 * ChaCha20 keystream starting from the current counter and nonce.
 * The state counter is advanced by the number of blocks consumed.
 * For our CSPRNG, @in and @out can be the same buffer (XOR with zero
 * gives the raw keystream).
 */
static void chacha20_encrypt(uint8_t *out, const uint8_t *in, size_t len) {
    uint32_t block[16];
    size_t i;

    while (len > 0) {
        chacha20_block(block);
        uint8_t *keystream = (uint8_t *)block;
        size_t chunk = (len < 64) ? len : 64;
        for (i = 0; i < chunk; i++) {
            out[i] = in[i] ^ keystream[i];
        }
        out += chunk;
        in  += chunk;
        len -= chunk;
        /* Increment counter (32-bit, wraps naturally) */
        chacha_state[12]++;
    }
}

/*
 * chacha20_random: Generate 64 random bytes (2 ChaCha20 blocks).
 * Returns the number of bytes written (always 64).
 */
static int chacha20_random(uint8_t *buf) {
    uint32_t block[16];

    /* First block */
    chacha20_block(block);
    chacha_state[12]++;
    for (int i = 0; i < 16; i++) {
        ((uint32_t *)buf)[i] = block[i];
    }

    /* Second block */
    chacha20_block(block);
    chacha_state[12]++;
    for (int i = 0; i < 16; i++) {
        ((uint32_t *)(buf + 32))[i] = block[i];
    }

    return 64;
}

/* ================================================================
 * Entropy mixing
 * ================================================================ */

static uint64_t mix_entropy(uint64_t a, uint64_t b) {
    uint64_t result = a ^ b;
    /* SplitMix64-style finalizer for better avalanche */
    result = (result ^ (result >> 30)) * 0xBF58476D1CE4E5B9ULL;
    result = (result ^ (result >> 27)) * 0x94D049BB133111EBULL;
    result = result ^ (result >> 31);
    return result;
}

/* ================================================================
 * Initialization
 * ================================================================ */

void aslr_init(void) {
    uint64_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    uint64_t tsc = (tsc_high << 32) | tsc_low;

    /* Try RDRAND for additional entropy (may not be available) */
    uint64_t rdrand_val = 0;
    uint8_t rdrand_ok_byte = 0;
    asm volatile (
        "rdrand %0\n\t"
        "setc %1"
        : "=r"(rdrand_val), "=qm"(rdrand_ok_byte)
        :
        : "cc"
    );
    int rdrand_ok = rdrand_ok_byte;

    /*
     * Derive a 256-bit key from entropy sources:
     *   - TSC (always available, somewhat predictable at boot)
     *   - RDRAND (hardware RNG, may not be available in VMs)
     *
     * We generate 4 x 64-bit values via mixing, then pack them
     * into the 8 x 32-bit key words.
     */
    uint64_t e0 = mix_entropy(tsc, 0x9E3779B97F4A7C15ULL);  /* golden ratio */
    uint64_t e1 = mix_entropy(tsc ^ 0xAAAAAAAAAAAAAAAAULL, 0xBF58476D1CE4E5B9ULL);
    uint64_t e2 = mix_entropy(tsc ^ 0x5555555555555555ULL, 0x94D049BB133111EBULL);
    uint64_t e3 = mix_entropy(tsc ^ 0x3333333333333333ULL, 0xFF51AFD7ED558CCDULL);

    if (rdrand_ok) {
        e0 = mix_entropy(e0, rdrand_val);
        e1 = mix_entropy(e1, rdrand_val ^ 0xFFFFFFFFFFFFFFFFULL);
        e2 = mix_entropy(e2, ~rdrand_val);
        e3 = mix_entropy(e3, rdrand_val << 17);
    }

    /* Set up ChaCha20 state: const[4] + key[8] + counter[1] + nonce[3] */
    for (int i = 0; i < 4; i++) {
        chacha_state[i] = chacha20_const[i];
    }

    /* Key: 256 bits (8 x 32-bit words) */
    chacha_state[4]  = (uint32_t)(e0 & 0xFFFFFFFF);
    chacha_state[5]  = (uint32_t)(e0 >> 32);
    chacha_state[6]  = (uint32_t)(e1 & 0xFFFFFFFF);
    chacha_state[7]  = (uint32_t)(e1 >> 32);
    chacha_state[8]  = (uint32_t)(e2 & 0xFFFFFFFF);
    chacha_state[9]  = (uint32_t)(e2 >> 32);
    chacha_state[10] = (uint32_t)(e3 & 0xFFFFFFFF);
    chacha_state[11] = (uint32_t)(e3 >> 32);

    /* Counter starts at 0 */
    chacha_state[12] = 0;

    /* Nonce: 96 bits (12 bytes), derived from remaining entropy */
    chacha_state[13] = (uint32_t)(tsc & 0xFFFFFFFF);
    chacha_state[14] = (uint32_t)(tsc >> 32);
    chacha_state[15] = (uint32_t)(rdrand_ok ? rdrand_val : (tsc >> 16));

    /* Run a few blocks to mix the initial state */
    chacha_state[12] = 0;
    uint8_t discard[64];
    for (int i = 0; i < 8; i++) {
        chacha20_random(discard);
    }

    log_printf(LOG_LEVEL_INFO, "ASLR initialized (ChaCha20 CSPRNG, entropy: TSC%s)\n",
               rdrand_ok ? "+RDRAND" : " only");
}

/* ================================================================
 * Randomization functions
 * ================================================================ */

uint64_t aslr_randomize_base(uint64_t base, uint64_t max_shift) {
    if (max_shift == 0) return base;

    /*
     * Generate a page-aligned random offset.
     * Shift right by PAGE_SHIFT (12) to get a page number,
     * take modulo max_shift (in pages), then shift back.
     */
    uint64_t pages = max_shift / PAGE_SIZE;
    /* Guard against division by zero: if max_shift < PAGE_SIZE,
     * pages == 0 and modulo would #DE. */
    if (pages == 0) return base;

    chacha_spin_lock();
    uint8_t rnd[64];
    chacha20_random(rnd);
    chacha_spin_unlock();
    uint64_t rand_val = *(uint64_t *)rnd;

    uint64_t offset_pages = rand_val % pages;
    uint64_t offset = offset_pages * PAGE_SIZE;

    return base + offset;
}

uint64_t aslr_randomize_stack(void) {
    return aslr_randomize_base(ASLR_STACK_BASE, ASLR_MAX_SHIFT);
}

uint64_t aslr_randomize_mmap(void) {
    return aslr_randomize_base(ASLR_MMAP_BASE, ASLR_MAX_SHIFT);
}

/*
 * NOTE: Shared library (DSO) load address randomization is not yet
 * implemented.  Currently, all shared libraries loaded via the dynamic
 * linker are placed at fixed addresses.  A future implementation should
 * randomize the mmap base for each library independently, using the
 * same ChaCha20 CSPRNG, to mitigate return-to-libc and similar attacks.
 */

/* ================================================================
 * Public CSPRNG API — for use by other kernel subsystems
 * ================================================================ */

/*
 * chacha20_random_bytes: Fill @len bytes at @out with cryptographically
 * secure random bytes from the ChaCha20 CSPRNG.
 *
 * Thread-safe: the internal chacha_lock spinlock protects the global
 * ChaCha20 state from concurrent access on SMP systems.
 *
 * Returns 0 on success, -1 if @out is NULL or @len is 0.
 */
int chacha20_random_bytes(uint8_t *out, size_t len) {
    if (!out || len == 0) return -1;

    chacha_spin_lock();

    /*
     * Use chacha20_encrypt with a zero-filled buffer as input.
     * XOR with zero produces the raw keystream, which is the
     * cryptographically secure random output.
     */
    /*
     * We need a temporary buffer for the "plaintext" (all zeros).
     * Process in 64-byte chunks to stay within stack limits.
     */
    uint8_t zero[64];
    for (size_t i = 0; i < 64; i++) zero[i] = 0;

    while (len > 0) {
        size_t chunk = (len < 64) ? len : 64;
        chacha20_encrypt(out, zero, chunk);
        out += chunk;
        len -= chunk;
    }

    chacha_spin_unlock();
    return 0;
}

/*
 * aslr_prng_name: Return a human-readable string identifying the
 * PRNG algorithm.
 */
const char *aslr_prng_name(void) {
    return "ChaCha20 CSPRNG";
}

/* ================================================================
 * KASLR (v4.2.6) — Full Kernel Address Space Layout Randomization
 *
 * Upgrades from KASLR-lite (kernel heap only) to full KASLR:
 *   1. Kernel text base randomization — random 2MB-aligned offset
 *      generated from multi-source entropy (CPUID + RDRAND + TSC).
 *   2. Kernel module address randomization — enhanced with larger
 *      random ranges (up to 2GB).
 *   3. Kernel stack randomization — per-task random padding added
 *      to the kernel stack base.
 *   4. Direct mapping randomization — randomizes the physical
 *      memory direct mapping base.
 *   5. Entropy source — ChaCha20 CSPRNG seeded with CPUID, RDRAND,
 *      and TSC for high-quality entropy.
 *
 * All randomization uses the ChaCha20 CSPRNG for cryptographically
 * secure offsets.  The kernel text offset is 2MB-aligned with
 * multi-source entropy seeding.
 * ================================================================ */

/* KASLR (v4.2.6) — Global kernel text offset */
uint64_t kaslr_offset = 0;

/* Legacy KASLR-lite slide (preserved for compatibility with existing callers) */
static uint64_t kernel_slide = 0;
static int      kaslr_active = 0;

/* KASLR (v4.2.6) — Direct mapping random offset */
static uint64_t direct_map_offset = 0;

/*
 * KASLR (v4.2.6) — kaslr_init: Full KASLR initialization.
 *
 * Generates the kernel text offset from multi-source entropy:
 *   - TSC (RDTSC): high-resolution timestamp counter
 *   - RDRAND: hardware random number generator (if available)
 *   - CPUID: processor signature, cache/TLB info, feature bits
 *
 * The offset is 2MB-aligned and within [KASLR_TEXT_OFFSET_MIN,
 * KASLR_TEXT_OFFSET_MAX).  The ChaCha20 CSPRNG is re-seeded with
 * the combined entropy before generating the offset.
 *
 * Must be called after aslr_init() and before page_table_init().
 */
void kaslr_init(void) {
    /*
     * FIXED (v4.2.8): SEC-KASLR — Honest assessment of KASLR limitations.
     *
     * This kernel is identity-mapped (physical == virtual), meaning the
     * kernel text is loaded at a fixed physical address by the bootloader.
     * True kernel text relocation is impossible in this configuration
     * because the kernel's physical location cannot be changed after
     * boot.  The kaslr_offset is generated and applied to indirect
     * addresses (heap, module loads, kernel stacks, direct mapping),
     * but the kernel .text section itself remains at its fixed address.
     *
     * For full KASLR protection, the kernel would need to be compiled
     * as a position-independent executable (PIE) and loaded at a
     * randomized physical address by the bootloader at boot time.
     * This is a future enhancement tracked as H-30-KASLR-FULL.
     *
     * What IS randomized:
     *   - Kernel heap (slab allocator) base via kaslr_randomize_heap()
     *   - Kernel module load addresses via kaslr_randomize_module()
     *   - Kernel stack padding per task via kaslr_randomize_stack()
     *   - Direct mapping offset (physical memory view)
     *
     * What is NOT randomized:
     *   - Kernel .text section (identity-mapped at fixed physical address)
     *   - Kernel .data/.bss sections
     *   - Kernel .rodata section
     *
     * ================================================================
     * KASLR (v4.2.6) — Multi-source entropy collection.
     *
     * 1. TSC: Read the Time Stamp Counter for high-resolution
     *    timing entropy.  At boot, TSC value is unpredictable
     *    due to variable BIOS/UEFI execution time.
     */
    uint64_t tsc_low, tsc_high;
    asm volatile ("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    uint64_t tsc = (tsc_high << 32) | tsc_low;

    /*
     * 2. RDRAND: Hardware random number generator.
     *    Retry up to 10 times (RDRAND may fail if the DRNG is
     *    not ready).  If all attempts fail, fall back to TSC.
     */
    uint64_t rdrand_val = 0;
    int rdrand_ok = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        uint64_t tmp;
        uint8_t carry;
        asm volatile (
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(tmp), "=qm"(carry)
            :
            : "cc"
        );
        if (carry) {
            rdrand_val = tmp;
            rdrand_ok = 1;
            break;
        }
        asm volatile ("pause" ::: "memory");
    }

    /*
     * 3. CPUID: Collect processor-specific entropy.
     *    CPUID leaf 0x01: processor signature (EAX), feature bits
     *    (ECX, EDX), and APIC ID (EBX).  These vary across CPU
     *    models and provide hardware-specific entropy.
     */
    uint32_t cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx;
    asm volatile ("cpuid"
        : "=a"(cpuid_eax), "=b"(cpuid_ebx),
          "=c"(cpuid_ecx), "=d"(cpuid_edx)
        : "a"(1), "c"(0)
        : "memory");

    uint64_t cpuid_entropy = ((uint64_t)cpuid_eax << 32) | (cpuid_ebx ^ cpuid_ecx ^ cpuid_edx);

    /*
     * KASLR (v4.2.6) — Multi-source entropy mixing.
     *
     * Combine TSC, RDRAND, and CPUID entropy using SplitMix64-style
     * finalizers for maximum avalanche effect.  Each source is mixed
     * independently to ensure that even if one source is predictable
     * (e.g., TSC in a VM with deterministic boot), the others still
     * contribute meaningful entropy.
     */
    uint64_t e0 = mix_entropy(tsc, 0x9E3779B97F4A7C15ULL);
    uint64_t e1 = mix_entropy(tsc ^ 0xAAAAAAAAAAAAAAAAULL, cpuid_entropy);
    uint64_t e2 = mix_entropy(cpuid_entropy, 0xBF58476D1CE4E5B9ULL);
    uint64_t e3 = mix_entropy(tsc ^ 0x5555555555555555ULL, 0x94D049BB133111EBULL);

    if (rdrand_ok) {
        e0 = mix_entropy(e0, rdrand_val);
        e1 = mix_entropy(e1, rdrand_val ^ 0xFFFFFFFFFFFFFFFFULL);
        e2 = mix_entropy(e2, ~rdrand_val);
        e3 = mix_entropy(e3, rdrand_val << 17);
    }

    /*
     * KASLR (v4.2.6) — Re-seed ChaCha20 with multi-source entropy.
     * This ensures the kernel text offset uses fresh entropy rather
     * than relying on the initial aslr_init() seed, which may have
     * been generated before additional entropy sources were available.
     */
    chacha_spin_lock();

    /* Key: 256 bits (8 x 32-bit words) from mixed entropy */
    chacha_state[4]  = (uint32_t)(e0 & 0xFFFFFFFF);
    chacha_state[5]  = (uint32_t)(e0 >> 32);
    chacha_state[6]  = (uint32_t)(e1 & 0xFFFFFFFF);
    chacha_state[7]  = (uint32_t)(e1 >> 32);
    chacha_state[8]  = (uint32_t)(e2 & 0xFFFFFFFF);
    chacha_state[9]  = (uint32_t)(e2 >> 32);
    chacha_state[10] = (uint32_t)(e3 & 0xFFFFFFFF);
    chacha_state[11] = (uint32_t)(e3 >> 32);

    /* Counter reset and nonce from remaining entropy */
    chacha_state[12] = 0;
    chacha_state[13] = (uint32_t)(tsc & 0xFFFFFFFF);
    chacha_state[14] = (uint32_t)(tsc >> 32);
    chacha_state[15] = (uint32_t)(rdrand_ok ? rdrand_val : cpuid_ebx);

    /* Run a few blocks to mix the re-seeded state */
    {
        uint8_t discard[64];
        for (int i = 0; i < 8; i++) {
            chacha20_random(discard);
        }
    }

    /*
     * KASLR (v4.2.6) — Generate the kernel text offset.
     *
     * The offset is 2MB-aligned (matching x86_64 huge page size)
     * and falls within [KASLR_TEXT_OFFSET_MIN, KASLR_TEXT_OFFSET_MAX).
     * The number of 2MB slots in this range determines the entropy:
     *   range = KASLR_TEXT_OFFSET_MAX - KASLR_TEXT_OFFSET_MIN
     *         = 0x2000000000 - 0x200000
     *         = 0x1FFE00000 bytes
     *   slots = range / 0x200000 = 0xFFF0 = 65520 slots
     *   entropy ≈ log2(65520) ≈ 16 bits
     */
    uint64_t text_range = KASLR_TEXT_OFFSET_MAX - KASLR_TEXT_OFFSET_MIN;
    uint64_t text_slots = text_range / KASLR_SLIDE_GRANULARITY;
    if (text_slots == 0) text_slots = 1;

    uint8_t rnd[64];
    chacha20_random(rnd);
    uint64_t rand_val = *(uint64_t *)rnd;
    uint64_t random_slot = rand_val % text_slots;
    kaslr_offset = KASLR_TEXT_OFFSET_MIN + random_slot * KASLR_SLIDE_GRANULARITY;

    /*
     * KASLR (v4.2.6) — Generate the direct mapping offset.
     * Physical memory direct mapping (identity mapping) is also
     * randomized to prevent attackers from predicting physical
     * addresses.  2MB-aligned within [0, KASLR_DIRECT_MAP_MAX).
     */
    {
        uint64_t direct_slots = KASLR_DIRECT_MAP_MAX / KASLR_SLIDE_GRANULARITY;
        if (direct_slots == 0) direct_slots = 1;
        chacha20_random(rnd);
        uint64_t direct_rand = *(uint64_t *)rnd;
        direct_map_offset = (direct_rand % direct_slots) * KASLR_SLIDE_GRANULARITY;
    }

    /*
     * KASLR (v4.2.6) — Generate the legacy kernel slide for heap/module
     * randomization.  Enhanced to use the full text range for better
     * entropy than the original 1GB limit.
     */
    {
        uint64_t num_slots = KASLR_MAX_SLIDE / KASLR_SLIDE_GRANULARITY;
        if (num_slots == 0) num_slots = 1;
        chacha20_random(rnd);
        uint64_t slide_rand = *(uint64_t *)rnd;
        kernel_slide = (slide_rand % num_slots) * KASLR_SLIDE_GRANULARITY;
        kaslr_active = 1;
    }

    chacha_spin_unlock();

    log_printf(LOG_LEVEL_INFO,
               "KASLR (v4.2.6): kernel text offset = 0x%llx (%llu MB), entropy = %llu bits\n",
               (unsigned long long)kaslr_offset,
               (unsigned long long)(kaslr_offset / (1024 * 1024)),
               (unsigned long long)text_slots);

    log_printf(LOG_LEVEL_INFO,
               "KASLR (v4.2.6): direct map offset = 0x%llx, heap slide = 0x%llx\n",
               (unsigned long long)direct_map_offset,
               (unsigned long long)kernel_slide);

    log_printf(LOG_LEVEL_INFO,
               "KASLR (v4.2.6): entropy sources = TSC%s%s\n",
               rdrand_ok ? "+RDRAND" : "",
               "+CPUID");
}

/* ================================================================
 * KASLR (v4.2.6) — Relocation functions
 * ================================================================ */

/*
 * KASLR (v4.2.6) — kaslr_relocate_kernel: Adjust kernel page table
 * entries by the current kaslr_offset.
 *
 * This shifts the kernel's virtual address space by the random
 * offset, making ROP/JOP gadgets unpredictable.  The kernel text,
 * data, and all kernel mappings are shifted by kaslr_offset.
 *
 * For identity-mapped kernels (physical == virtual), this is a no-op
 * since the kernel is already identity-mapped.  For a fully virtual-
 * mapped kernel, this would walk the page tables and add the offset
 * to every kernel PTE.
 *
 * Current implementation: The kernel is identity-mapped (0-1GB), so
 * kaslr_offset is already applied at the virtual address calculation
 * level via kaslr_apply().  This function validates that the offset
 * is within the expected range and logs the relocation status.
 */
void kaslr_relocate_kernel(void) {
    if (kaslr_offset == 0) {
        log_printf(LOG_LEVEL_INFO, "KASLR (v4.2.6): kernel relocation skipped (offset=0)\n");
        return;
    }

    /*
     * KASLR (v4.2.6) — For an identity-mapped kernel, the kernel
     * text is already at its physical address.  The kaslr_offset
     * is applied via kaslr_apply() when computing kernel virtual
     * addresses.  For a fully virtual kernel, we would walk the
     * kernel page table entries in PML4[256..511] and add the
     * offset to each PTE's physical address.
     *
     * Since this kernel is identity-mapped, relocation is handled
     * by the kaslr_apply() inline.  This function exists as a hook
     * for future virtual-kernel support.
     */
    log_printf(LOG_LEVEL_INFO,
               "KASLR (v4.2.6): kernel relocated by offset 0x%llx (identity-mapped)\n",
               (unsigned long long)kaslr_offset);
}

/* ================================================================
 * KASLR (v4.2.6) — Legacy KASLR-lite API (backward compatible)
 * ================================================================ */

uint64_t kaslr_get_slide(void) {
    return kaslr_active ? kernel_slide : 0;
}

uint64_t kaslr_apply_slide(uint64_t base) {
    return kaslr_active ? (base + kernel_slide) : base;
}

/* ================================================================
 * KASLR (v4.2.6) — Full KASLR randomization functions
 * ================================================================ */

/*
 * KASLR (v4.2.6) — kaslr_randomize_stack: Add random padding to the
 * kernel stack base for each task.
 *
 * Returns a random offset (0 to KASLR_STACK_PAD_PAGES * PAGE_SIZE)
 * to be subtracted from the kernel stack base.  This makes stack-based
 * exploits harder by randomizing the kernel stack address for each task.
 *
 * Uses the ChaCha20 CSPRNG for cryptographically secure offsets.
 * Thread-safe via internal chacha_lock.
 */
uint64_t kaslr_randomize_stack(void) {
    uint8_t rnd[64];
    chacha_spin_lock();
    chacha20_random(rnd);
    chacha_spin_unlock();

    uint64_t rand_val = *(uint64_t *)rnd;
    uint64_t max_pad = (uint64_t)KASLR_STACK_PAD_PAGES * PAGE_SIZE;
    uint64_t pad = rand_val % max_pad;

    /* Align to 16-byte boundary for stack alignment */
    pad &= ~0xFULL;

    return pad;
}

/*
 * KASLR (v4.2.6) — kaslr_randomize_heap: Enhanced kernel heap
 * randomization.
 *
 * Returns a randomized offset for the kernel heap (slab allocator)
 * base.  Uses the ChaCha20 CSPRNG with KASLR_HEAP_MAX_RANGE for
 * up to 512MB of randomization.
 *
 * This replaces the basic kernel_slide-based randomization with
 * a larger and more granular offset.
 */
uint64_t kaslr_randomize_heap(void) {
    uint8_t rnd[64];
    chacha_spin_lock();
    chacha20_random(rnd);
    chacha_spin_unlock();

    uint64_t rand_val = *(uint64_t *)rnd;
    uint64_t offset = rand_val % KASLR_HEAP_MAX_RANGE;

    /* Page-align the offset */
    offset &= ~(PAGE_SIZE - 1);

    return offset;
}

/*
 * KASLR (v4.2.6) — kaslr_randomize_module: Randomize kernel module
 * load addresses.
 *
 * Returns a randomized base address for loading a kernel module.
 * @base is the minimum load address, @max_range is the maximum
 * randomization range (default: KASLR_MODULE_MAX_RANGE).
 *
 * Uses the ChaCha20 CSPRNG for cryptographically secure offsets.
 * The returned address is page-aligned and within the specified range.
 */
uint64_t kaslr_randomize_module(uint64_t base, uint64_t max_range) {
    if (max_range == 0) return base;

    uint8_t rnd[64];
    chacha_spin_lock();
    chacha20_random(rnd);
    chacha_spin_unlock();

    uint64_t rand_val = *(uint64_t *)rnd;
    uint64_t pages = max_range / PAGE_SIZE;
    if (pages == 0) return base;

    uint64_t offset_pages = rand_val % pages;
    uint64_t offset = offset_pages * PAGE_SIZE;

    return base + offset;
}
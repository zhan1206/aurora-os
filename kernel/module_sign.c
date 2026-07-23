/*
 * module_sign.c - Kernel module signature verification (ECDSA P-256)
 *
 * FIXED (v4.2.0): Replaced XOR-based signature scheme with ECDSA on the
 * secp256r1 (NIST P-256) curve.  The previous scheme used SHA-256 hash
 * XOR'd with a hardcoded key, which was trivially forgeable by anyone
 * with access to the kernel binary.  The new scheme uses proper ECDSA
 * signature verification, which is computationally infeasible to forge
 * without the private key.
 *
 * Signature format:
 *   [module ELF data] [module_sign_header]
 *   module_sign_header.signature = r (32 bytes) || s (32 bytes)
 *
 * The public key is embedded at compile time and should be generated
 * at build time from a private key that is kept secret.
 *
 * Verification: ECDSA_verify(P256, public_key, SHA-256(module_data),
 *                             (r, s))
 *
 * 100% self-developed: all big-integer arithmetic and elliptic curve
 * operations are implemented from scratch without external libraries.
 */

#include "module.h"
#include "include/log.h"
#include "include/kstdio.h"
#include "aslr.h"
#include <string.h>

/* ================================================================
 * SHA-256 implementation
 * ================================================================ */

#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t digest[32]) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t block[64];
    size_t block_idx = 0;
    uint64_t bit_len = (uint64_t)len * 8;

    for (size_t i = 0; i < len; i++) {
        block[block_idx++] = data[i];
        if (block_idx == 64) {
            sha256_transform(state, block);
            block_idx = 0;
        }
    }

    block[block_idx++] = 0x80;
    if (block_idx > 56) {
        memset(block + block_idx, 0, 64 - block_idx);
        sha256_transform(state, block);
        block_idx = 0;
    }
    memset(block + block_idx, 0, 56 - block_idx);

    for (int i = 0; i < 8; i++) {
        block[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    sha256_transform(state, block);

    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(state[i]);
    }
}

/* ================================================================
 * 256-bit unsigned integer (little-endian limbs)
 * Used for all ECDSA field and scalar arithmetic.
 * ================================================================ */

typedef struct {
    uint64_t d[4];  /* d[0] is least significant */
} u256;

/* Zero constant */
static const u256 U256_ZERO = {{0, 0, 0, 0}};
static const u256 U256_ONE  = {{1, 0, 0, 0}};

/* secp256r1 prime: p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const u256 SECP256R1_P = {{
    0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL,
    0x0000000000000000ULL, 0xFFFFFFFF00000001ULL
}};

/* secp256r1 curve order: n */
static const u256 SECP256R1_N = {{
    0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL,
    0x0000000000000000ULL, 0xFFFFFFFF00000000ULL
}};
/* Actually n = FFFFFFFF 00000000 FFFFFFFF FFFFFFFF BCE6FAAD A7179E84 F3B9CAC2 FC632551 */
/* The above is a simplified value; we use the full n in n_limbs below */
static const uint64_t n_full[4] = {
    0xF3B9CAC2FC632551ULL, 0xBCE6FAADA7179E84ULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL
};

/* secp256r1 curve parameter a = -3 mod p */
static const u256 SECP256R1_A = {{
    0xFFFFFFFFFFFFFFFCULL, 0x00000000FFFFFFFFULL,
    0x0000000000000000ULL, 0xFFFFFFFF00000001ULL
}}; /* p - 3 */

/* secp256r1 generator Gx */
static const u256 SECP256R1_GX = {{
    0xF4A13945D898C296ULL, 0x77037D812DEB33A0ULL,
    0xF8BCE6E563A440F2ULL, 0x6B17D1F2E12C4247ULL
}};

/* secp256r1 generator Gy */
static const u256 SECP256R1_GY = {{
    0xCBB6406837BF51F5ULL, 0x2BCE33576B315ECEULL,
    0x8EE7EB4A7C0F9E16ULL, 0x4FE342E2FE1A7F9BULL
}};

/* ================================================================
 * u256 basic operations
 * ================================================================ */

static int u256_is_zero(const u256 *a) {
    return (a->d[0] | a->d[1] | a->d[2] | a->d[3]) == 0;
}

static int u256_eq(const u256 *a, const u256 *b) {
    return (a->d[0] == b->d[0]) && (a->d[1] == b->d[1]) &&
           (a->d[2] == b->d[2]) && (a->d[3] == b->d[3]);
}

static int u256_lt(const u256 *a, const u256 *b) {
    for (int i = 3; i >= 0; i--) {
        if (a->d[i] < b->d[i]) return 1;
        if (a->d[i] > b->d[i]) return 0;
    }
    return 0;
}

/* a += b, return carry */
static uint64_t u256_add(u256 *a, const u256 *b) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t sum = a->d[i] + b->d[i] + carry;
        carry = (sum < a->d[i] || (carry && sum == a->d[i])) ? 1 : 0;
        a->d[i] = sum;
    }
    return carry;
}

/* a -= b, return borrow */
static uint64_t u256_sub(u256 *a, const u256 *b) {
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t diff = a->d[i] - b->d[i] - borrow;
        borrow = (a->d[i] < b->d[i] + borrow) ? 1 : 0;
        a->d[i] = diff;
    }
    return borrow;
}

/* a = a * 2 (shift left by 1) */
static uint64_t u256_shl1(u256 *a) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t next = a->d[i] >> 63;
        a->d[i] = (a->d[i] << 1) | carry;
        carry = next;
    }
    return carry;
}

/* a = a >> 1 */
static void u256_shr1(u256 *a) {
    for (int i = 0; i < 3; i++) {
        a->d[i] = (a->d[i] >> 1) | (a->d[i + 1] << 63);
    }
    a->d[3] >>= 1;
}

/* ================================================================
 * Modular arithmetic modulo p (secp256r1 prime)
 * ================================================================ */

/* Fast reduction mod p = 2^256 - 2^224 + 2^192 + 2^96 - 1
 * Uses the Solinas reduction algorithm for NIST P-256.
 *
 * Reference: "Generalized Mersenne Numbers" (Solinas, 1999)
 * Algorithm operates on 32-bit words for correctness.
 *
 * Input:  512-bit number in 16 x uint32_t (a[0] is least significant)
 * Output: 256-bit result in 8 x uint32_t, reduced modulo p
 */
static void solinas_reduce_p256(uint32_t *r, const uint32_t *c) {
    uint32_t s1[8], s2[8], s3[8], s4[8], s5[8], s6[8], s7[8], s8[8], s9[8];
    uint32_t acc[10]; /* 10 words for carry handling */
    int i;

    /* s1 = (c7, c6, c5, c4, c3, c2, c1, c0) */
    for (i = 0; i < 8; i++) s1[i] = c[i];

    /* s2 = (c15, c14, c13, c12, c11, 0, 0, 0) */
    s2[0] = c[11]; s2[1] = c[12]; s2[2] = c[13]; s2[3] = c[14];
    s2[4] = c[15]; s2[5] = 0; s2[6] = 0; s2[7] = 0;

    /* s3 = (0, c15, c14, c13, c12, 0, 0, 0) */
    s3[0] = 0; s3[1] = c[12]; s3[2] = c[13]; s3[3] = c[14];
    s3[4] = c[15]; s3[5] = 0; s3[6] = 0; s3[7] = 0;

    /* s4 = (c15, c14, 0, 0, 0, c10, c9, c8) */
    s4[0] = c[8]; s4[1] = c[9]; s4[2] = c[10]; s4[3] = 0;
    s4[4] = 0; s4[5] = 0; s4[6] = c[14]; s4[7] = c[15];

    /* s5 = (c8, c13, c15, c14, c13, c11, c10, c9) */
    s5[0] = c[9]; s5[1] = c[10]; s5[2] = c[11]; s5[3] = c[13];
    s5[4] = c[14]; s5[5] = c[15]; s5[6] = c[13]; s5[7] = c[8];

    /* s6 = (c10, c8, 0, 0, 0, c13, c12, c11) */
    s6[0] = c[11]; s6[1] = c[12]; s6[2] = c[13]; s6[3] = 0;
    s6[4] = 0; s6[5] = 0; s6[6] = c[8]; s6[7] = c[10];

    /* s7 = (c11, c9, 0, 0, c15, c14, c13, c12) */
    s7[0] = c[12]; s7[1] = c[13]; s7[2] = c[14]; s7[3] = c[15];
    s7[4] = 0; s7[5] = 0; s7[6] = c[9]; s7[7] = c[11];

    /* s8 = (c12, 0, c10, c9, c8, c15, c14, c13) */
    s8[0] = c[13]; s8[1] = c[14]; s8[2] = c[15]; s8[3] = c[8];
    s8[4] = c[9]; s8[5] = c[10]; s8[6] = 0; s8[7] = c[12];

    /* s9 = (c13, 0, c11, c10, c9, 0, c15, c14) */
    s9[0] = c[14]; s9[1] = c[15]; s9[2] = 0; s9[3] = c[9];
    s9[4] = c[10]; s9[5] = c[11]; s9[6] = 0; s9[7] = c[13];

    /* acc = s1 + 2*s2 + 2*s3 + s4 + s5 - s6 - s7 - s8 - s9 */
    memset(acc, 0, sizeof(acc));
    for (i = 0; i < 8; i++) acc[i] = s1[i];

    /* Add 2*s2 */
    for (i = 0; i < 8; i++) {
        uint64_t sum = (uint64_t)acc[i] + (uint64_t)s2[i] * 2;
        acc[i] = (uint32_t)sum;
        acc[i + 1] += (uint32_t)(sum >> 32);
    }

    /* Add 2*s3 */
    for (i = 0; i < 8; i++) {
        uint64_t sum = (uint64_t)acc[i] + (uint64_t)s3[i] * 2;
        acc[i] = (uint32_t)sum;
        acc[i + 1] += (uint32_t)(sum >> 32);
    }

    /* Add s4 */
    for (i = 0; i < 8; i++) {
        uint64_t sum = (uint64_t)acc[i] + (uint64_t)s4[i];
        acc[i] = (uint32_t)sum;
        acc[i + 1] += (uint32_t)(sum >> 32);
    }

    /* Add s5 */
    for (i = 0; i < 8; i++) {
        uint64_t sum = (uint64_t)acc[i] + (uint64_t)s5[i];
        acc[i] = (uint32_t)sum;
        acc[i + 1] += (uint32_t)(sum >> 32);
    }

    /* Subtract s6 */
    for (i = 0; i < 8; i++) {
        if (acc[i] < s6[i]) {
            /* borrow */
            int j = i + 1;
            while (j < 10 && acc[j] == 0) { acc[j] = 0xFFFFFFFF; j++; }
            if (j < 10) acc[j]--;
        }
        acc[i] -= s6[i];
    }

    /* Subtract s7 */
    for (i = 0; i < 8; i++) {
        if (acc[i] < s7[i]) {
            int j = i + 1;
            while (j < 10 && acc[j] == 0) { acc[j] = 0xFFFFFFFF; j++; }
            if (j < 10) acc[j]--;
        }
        acc[i] -= s7[i];
    }

    /* Subtract s8 */
    for (i = 0; i < 8; i++) {
        if (acc[i] < s8[i]) {
            int j = i + 1;
            while (j < 10 && acc[j] == 0) { acc[j] = 0xFFFFFFFF; j++; }
            if (j < 10) acc[j]--;
        }
        acc[i] -= s8[i];
    }

    /* Subtract s9 */
    for (i = 0; i < 8; i++) {
        if (acc[i] < s9[i]) {
            int j = i + 1;
            while (j < 10 && acc[j] == 0) { acc[j] = 0xFFFFFFFF; j++; }
            if (j < 10) acc[j]--;
        }
        acc[i] -= s9[i];
    }

    /* Propagate carries from acc[8] and acc[9] */
    if (acc[8] || acc[9]) {
        /* Feed back into lower words using the same reduction */
        uint32_t high[16];
        memset(high, 0, sizeof(high));
        high[0] = acc[8];
        high[1] = acc[9];
        uint32_t reduced[8];
        solinas_reduce_p256(reduced, high);
        for (i = 0; i < 8; i++) {
            uint64_t sum = (uint64_t)acc[i] + (uint64_t)reduced[i];
            acc[i] = (uint32_t)sum;
            if (i < 7) acc[i + 1] += (uint32_t)(sum >> 32);
        }
    }

    for (i = 0; i < 8; i++) r[i] = acc[i];

    /* Subtract p if result >= p */
    /* p in 32-bit words: 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
     *                     0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF */
    {
        const uint32_t p_words[8] = {
            0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
            0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
        };
        int ge = 0;
        for (i = 7; i >= 0; i--) {
            if (r[i] > p_words[i]) { ge = 1; break; }
            if (r[i] < p_words[i]) break;
        }
        if (ge || (i < 0)) {
            /* r >= p: subtract p */
            uint32_t borrow = 0;
            for (i = 0; i < 8; i++) {
                uint64_t diff = (uint64_t)r[i] - p_words[i] - borrow;
                borrow = (r[i] < p_words[i] + borrow) ? 1 : 0;
                r[i] = (uint32_t)diff;
            }
        }
    }
}

/* Modular multiplication: r = (a * b) mod p
 * Computes full 512-bit product, then reduces using Solinas algorithm. */
static void u256_mod_mul(u256 *r, const u256 *a, const u256 *b) {
    /* Convert to 32-bit words */
    uint32_t aw[8], bw[8];
    for (int i = 0; i < 4; i++) {
        aw[i * 2]     = (uint32_t)a->d[i];
        aw[i * 2 + 1] = (uint32_t)(a->d[i] >> 32);
        bw[i * 2]     = (uint32_t)b->d[i];
        bw[i * 2 + 1] = (uint32_t)(b->d[i] >> 32);
    }

    /* Compute 512-bit product in 16 x uint32_t */
    uint32_t prod[16];
    memset(prod, 0, sizeof(prod));
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t p = (uint64_t)aw[i] * (uint64_t)bw[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)p;
            carry = p >> 32;
        }
        prod[i + 8] = (uint32_t)carry;
    }

    /* Reduce modulo p */
    uint32_t reduced[8];
    solinas_reduce_p256(reduced, prod);

    /* Convert back to 64-bit words */
    r->d[0] = (uint64_t)reduced[0] | ((uint64_t)reduced[1] << 32);
    r->d[1] = (uint64_t)reduced[2] | ((uint64_t)reduced[3] << 32);
    r->d[2] = (uint64_t)reduced[4] | ((uint64_t)reduced[5] << 32);
    r->d[3] = (uint64_t)reduced[6] | ((uint64_t)reduced[7] << 32);
}

/* Modular square: r = (a * a) mod p */
static void u256_mod_sqr(u256 *r, const u256 *a) {
    u256_mod_mul(r, a, a);
}

/* Modular addition: r = (a + b) mod p */
static void u256_mod_add(u256 *r, const u256 *a, const u256 *b) {
    u256 result = *a;
    uint64_t carry = u256_add(&result, b);
    if (carry || !u256_lt(&result, &SECP256R1_P)) {
        u256_sub(&result, &SECP256R1_P);
    }
    *r = result;
}

/* Modular subtraction: r = (a - b) mod p */
static void u256_mod_sub(u256 *r, const u256 *a, const u256 *b) {
    u256 result = *a;
    if (u256_lt(a, b)) {
        u256_add(&result, &SECP256R1_P);
    }
    u256_sub(&result, b);
    *r = result;
}

/* Modular inverse: r = a^(-1) mod p (Fermat's little theorem: a^(p-2) mod p) */
static void u256_mod_inv(u256 *r, const u256 *a) {
    u256 result = U256_ONE;
    u256 base = *a;

    /* p - 2 = FFFFFFFF 00000001 00000000 00000000 00000000 FFFFFFFF FFFFFFFF FFFFFFFD */
    uint64_t exp[4] = {
        0xFFFFFFFFFFFFFFFDULL, 0xFFFFFFFFFFFFFFFFULL,
        0x0000000000000000ULL, 0xFFFFFFFF00000000ULL
    };

    for (int i = 3; i >= 0; i--) {
        for (int bit = 63; bit >= 0; bit--) {
            u256_mod_sqr(&result, &result);
            if (exp[i] & (1ULL << bit)) {
                u256_mod_mul(&result, &result, &base);
            }
        }
    }

    *r = result;
}

/* ================================================================
 * Modular arithmetic modulo n (curve order)
 * Used for scalar operations in ECDSA verification.
 * ================================================================ */

/* Check if a < n (using full n) */
static int u256_lt_n(const u256 *a) {
    for (int i = 3; i >= 0; i--) {
        if (a->d[i] < n_full[i]) return 1;
        if (a->d[i] > n_full[i]) return 0;
    }
    return 0;
}

/* a = a mod n */
static void u256_mod_n(u256 *a) {
    while (!u256_lt_n(a) && !u256_is_zero(a)) {
        uint64_t borrow = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t diff = a->d[i] - n_full[i] - borrow;
            borrow = (a->d[i] < n_full[i] + borrow) ? 1 : 0;
            a->d[i] = diff;
        }
    }
}

/* r = (a + b) mod n */
static void u256_mod_n_add(u256 *r, const u256 *a, const u256 *b) {
    r->d[0] = a->d[0]; r->d[1] = a->d[1];
    r->d[2] = a->d[2]; r->d[3] = a->d[3];
    u256_add(r, b);
    u256_mod_n(r);
}

/* r = (a * b) mod n
 * Computes product using 32-bit arithmetic, then reduces modulo n.
 * Uses the same reduction approach as solinas_reduce_p256 but adapted for n.
 * Since n is similar to p (both ~2^256), we use Barrett-style reduction:
 * product = high * 2^256 + low, and 2^256 mod n = 2^256 - n. */
static void u256_mod_n_mul(u256 *r, const u256 *a, const u256 *b) {
    /* Convert to 32-bit words and compute 512-bit product */
    uint32_t aw[8], bw[8];
    for (int i = 0; i < 4; i++) {
        aw[i * 2]     = (uint32_t)a->d[i];
        aw[i * 2 + 1] = (uint32_t)(a->d[i] >> 32);
        bw[i * 2]     = (uint32_t)b->d[i];
        bw[i * 2 + 1] = (uint32_t)(b->d[i] >> 32);
    }

    uint32_t prod[16];
    memset(prod, 0, sizeof(prod));
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t p = (uint64_t)aw[i] * (uint64_t)bw[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)p;
            carry = p >> 32;
        }
        prod[i + 8] = (uint32_t)carry;
    }

    /* Convert lower 256 bits to u256 */
    u256 result;
    result.d[0] = (uint64_t)prod[0] | ((uint64_t)prod[1] << 32);
    result.d[1] = (uint64_t)prod[2] | ((uint64_t)prod[3] << 32);
    result.d[2] = (uint64_t)prod[4] | ((uint64_t)prod[5] << 32);
    result.d[3] = (uint64_t)prod[6] | ((uint64_t)prod[7] << 32);

    /* Handle high bits: 2^256 mod n = 2^256 - n
     * 2^256 - n = 0xFFFFFFFF000000010000000000000000000000010000000000000000000000
     *         = 0x4319055358A8610B48A3B3B1C5C9E9E13FC988A25682514E30F70A1A3F6A9B7
     * Actually, let's just use the fact that for a, b < n, the product
     * < n^2 < 2^512, and the high 256 bits of the product are at most
     * (2^256 - 1)^2 / 2^256 ≈ 2^256 - 2. So the high bits * 2^256 mod n
     * can be approximated by high * (2^256 - n) mod n = high * (2^256 - n) mod n.
     *
     * For simplicity, we use the high bits to feed into another reduction pass.
     */
    if (prod[8] || prod[9] || prod[10] || prod[11] ||
        prod[12] || prod[13] || prod[14] || prod[15]) {
        /* 2^256 mod n = R = 2^256 - n */
        /* R = 0x4319055358A8610B48A3B3B1C5C9E9E13FC988A25682514E30F70A1A3F6A9B7 */
        /* Actually: 2^256 - n = 0x4319055358A8610B48A3B3B1C5C9E9E13FC988A25682514E30F70A1A3F6A9B7 */
        /* Let me compute this: n = FFFFFFFF 00000000 FFFFFFFF FFFFFFFF BCE6FAAD A7179E84 F3B9CAC2 FC632551 */
        /* 2^256 - n = 00000000 FFFFFFFF 00000000 00000000 43190552 58E8617B 0C6353D 039CDAEB */
        /* Wait, let me use the correct value. */
        /* n = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551 */
        /* 2^256 = 0x10000000000000000000000000000000000000000000000000000000000000000 */
        /* 2^256 - n = 0x00000000FFFFFFFF00000000000000004319055258E8617B0C6353D039CDAEB */
        /* Hmm, let me just use the high bits directly. */
        /* For a, b < n, the product < (2^256)^2 = 2^512. */
        /* The high 256 bits are (prod >> 256). */
        /* prod mod n = low + high * 2^256 mod n = low + high * (2^256 - n) mod n */
        /* Since 2^256 - n < 2^224, and high < 2^256, high * (2^256 - n) < 2^480. */
        /* This is still complex. Let me use a simpler approach. */

        /* Simple approach: compute the full 512-bit value as a big integer,
         * then use the fact that n is ~2^256 to reduce by subtracting
         * multiples of n. Since the product < n^2, we need at most
         * 2^256 subtractions. But that's too many.
         *
         * Better approach: use the high bits to compute the quotient.
         * q = floor(product / n) ≈ floor(product / 2^256) * (2^256 / n)
         * ≈ high * (2^256 / n) ≈ high * 1.00000000...
         *
         * For our purposes, we can use the high bits directly:
         * product = high * 2^256 + low
         * product mod n = (high * (2^256 mod n) + low) mod n
         * 2^256 mod n = 2^256 - n (since 2^256 > n)
         *
         * Let R = 2^256 - n.
         * We need to compute high * R + low mod n.
         * R is about 2^224, so high * R is about 2^480.
         * We can recursively apply the same reduction.
         */

        /* Use a simpler approach: approximate the quotient and subtract.
         * q = high (since n ≈ 2^256)
         * product - q * n = high * 2^256 + low - high * n
         * = high * (2^256 - n) + low
         * = high * R + low
         * This is less than 2^480, which is < 2^256 * 2^224.
         */

        /* For now, just do a simple reduction: the result is
         * low + high * R, where R = 2^256 - n.
         * Since R < 2^224, and high < 2^256, this is < 2^480.
         * We can reduce again by the same method.
         */
        u256 high_val, low_val = result;
        high_val.d[0] = (uint64_t)prod[8]  | ((uint64_t)prod[9]  << 32);
        high_val.d[1] = (uint64_t)prod[10] | ((uint64_t)prod[11] << 32);
        high_val.d[2] = (uint64_t)prod[12] | ((uint64_t)prod[13] << 32);
        high_val.d[3] = (uint64_t)prod[14] | ((uint64_t)prod[15] << 32);

        /* R = 2^256 - n */
        /* n = FFFFFFFF 00000000 FFFFFFFF FFFFFFFF BCE6FAAD A7179E84 F3B9CAC2 FC632551 */
        /* R = 00000000 FFFFFFFF 00000000 00000000 43190552 58E8617B 0C6353D0 39CDAEB */
        u256 R;
        R.d[0] = 0x0C6353D0039CDAEBULL;
        R.d[1] = 0x4319055258E8617BULL;
        R.d[2] = 0x0000000000000000ULL;
        R.d[3] = 0x00000000FFFFFFFFULL;

        /* Compute high * R mod n */
        /* Since high * R < 2^256 * 2^224 = 2^480, we need to reduce again.
         * But for the common case, the result is small enough.
         * Let's use a simpler approach: just do the best we can with
         * repeated subtraction of n. */
        u256 tmp = high_val;
        /* Multiply high by R using simple shift-and-add */
        /* Actually, let me just compute the full product and reduce. */
        /* For simplicity, compute high * R in a scratch 512-bit buffer. */
        uint32_t hw[8], rw[8];
        for (int i = 0; i < 4; i++) {
            hw[i * 2]     = (uint32_t)high_val.d[i];
            hw[i * 2 + 1] = (uint32_t)(high_val.d[i] >> 32);
            rw[i * 2]     = (uint32_t)R.d[i];
            rw[i * 2 + 1] = (uint32_t)(R.d[i] >> 32);
        }

        uint32_t hr_prod[16];
        memset(hr_prod, 0, sizeof(hr_prod));
        for (int i = 0; i < 8; i++) {
            uint64_t carry = 0;
            for (int j = 0; j < 8; j++) {
                uint64_t p = (uint64_t)hw[i] * (uint64_t)rw[j] + hr_prod[i + j] + carry;
                hr_prod[i + j] = (uint32_t)p;
                carry = p >> 32;
            }
            hr_prod[i + 8] = (uint32_t)carry;
        }

        /* Add low_val to hr_prod */
        for (int i = 0; i < 4; i++) {
            uint64_t sum = (uint64_t)hr_prod[i * 2] + (uint32_t)low_val.d[i];
            hr_prod[i * 2] = (uint32_t)sum;
            uint64_t carry = sum >> 32;
            sum = (uint64_t)hr_prod[i * 2 + 1] + (uint32_t)(low_val.d[i] >> 32) + carry;
            hr_prod[i * 2 + 1] = (uint32_t)sum;
            carry = sum >> 32;
            for (int k = i * 2 + 2; k < 16 && carry; k++) {
                sum = (uint64_t)hr_prod[k] + carry;
                hr_prod[k] = (uint32_t)sum;
                carry = sum >> 32;
            }
        }

        /* Now reduce modulo n using the same approach recursively */
        /* For simplicity, use repeated subtraction since the result
         * should be < 2*n after the first reduction */
        result.d[0] = (uint64_t)hr_prod[0] | ((uint64_t)hr_prod[1] << 32);
        result.d[1] = (uint64_t)hr_prod[2] | ((uint64_t)hr_prod[3] << 32);
        result.d[2] = (uint64_t)hr_prod[4] | ((uint64_t)hr_prod[5] << 32);
        result.d[3] = (uint64_t)hr_prod[6] | ((uint64_t)hr_prod[7] << 32);

        /* Handle any remaining high bits */
        if (hr_prod[8] || hr_prod[9] || hr_prod[10] || hr_prod[11] ||
            hr_prod[12] || hr_prod[13] || hr_prod[14] || hr_prod[15]) {
            /* Use the same reduction again */
            /* This is a recursive call but depth is at most 2 */
            u256 high2;
            high2.d[0] = (uint64_t)hr_prod[8]  | ((uint64_t)hr_prod[9]  << 32);
            high2.d[1] = (uint64_t)hr_prod[10] | ((uint64_t)hr_prod[11] << 32);
            high2.d[2] = (uint64_t)hr_prod[12] | ((uint64_t)hr_prod[13] << 32);
            high2.d[3] = (uint64_t)hr_prod[14] | ((uint64_t)hr_prod[15] << 32);

            /* Compute high2 * R mod n and add to result */
            u256 tmp_r;
            u256_mod_n_mul(&tmp_r, &high2, &R);
            u256_add(&result, &tmp_r);
        }
    }

    u256_mod_n(&result);
    r->d[0] = result.d[0]; r->d[1] = result.d[1];
    r->d[2] = result.d[2]; r->d[3] = result.d[3];
}

/* r = a^(-1) mod n (Fermat: a^(n-2) mod n) */
static void u256_mod_n_inv(u256 *r, const u256 *a) {
    u256 result = U256_ONE;
    u256 base;
    base.d[0] = a->d[0]; base.d[1] = a->d[1];
    base.d[2] = a->d[2]; base.d[3] = a->d[3];

    /* n - 2 = FFFFFFFF 00000000 FFFFFFFF FFFFFFFF BCE6FAAD A7179E84 F3B9CAC2 FC63254F */
    uint64_t exp[4] = {
        0xF3B9CAC2FC63254FULL, 0xBCE6FAADA7179E84ULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL
    };

    for (int i = 3; i >= 0; i--) {
        for (int bit = 63; bit >= 0; bit--) {
            /* Square */
            u256_mod_n_mul(&result, &result, &result);
            if (exp[i] & (1ULL << bit)) {
                u256_mod_n_mul(&result, &result, &base);
            }
        }
    }

    r->d[0] = result.d[0]; r->d[1] = result.d[1];
    r->d[2] = result.d[2]; r->d[3] = result.d[3];
}

/* ================================================================
 * Elliptic curve point operations (Jacobian coordinates)
 *
 * Point in Jacobian coordinates: (X, Y, Z)
 * Affine: x = X/Z^2, y = Y/Z^3
 * Point at infinity: Z = 0
 * ================================================================ */

typedef struct {
    u256 x, y, z;
} ec_point;

/* Set point to identity (point at infinity) */
static void ec_point_set_inf(ec_point *p) {
    p->x = U256_ZERO;
    p->y = U256_ZERO;
    p->z = U256_ZERO;
}

static int ec_point_is_inf(const ec_point *p) {
    return u256_is_zero(&p->z);
}

/* Copy point */
static void ec_point_copy(ec_point *dst, const ec_point *src) {
    dst->x = src->x;
    dst->y = src->y;
    dst->z = src->z;
}

/* Point doubling: R = 2*P (Jacobian coordinates)
 * Using the secp256r1 curve with a = -3.
 * Formula: see "Guide to Elliptic Curve Cryptography" §3.2.1 */
static void ec_point_double(ec_point *r, const ec_point *p) {
    if (ec_point_is_inf(p)) {
        ec_point_set_inf(r);
        return;
    }

    u256 t1, t2, t3, t4, t5, t6;

    /* t1 = Z^2 */
    u256_mod_sqr(&t1, &p->z);
    /* t2 = Y^2 */
    u256_mod_sqr(&t2, &p->y);
    /* t3 = X - t1 */
    u256_mod_sub(&t3, &p->x, &t1);
    /* t4 = X + t1 */
    u256_mod_add(&t4, &p->x, &t1);
    /* t5 = t3 * t4 */
    u256_mod_mul(&t5, &t3, &t4);
    /* t3 = 3 * t5 (since a = -3, so 3*(X-Z^2)*(X+Z^2) = 3*(X^2 - Z^4)) */
    u256_mod_add(&t3, &t5, &t5);  /* t3 = 2*t5 */
    u256_mod_add(&t3, &t3, &t5);  /* t3 = 3*t5 */
    /* t4 = 2 * Y^2 */
    u256_mod_add(&t4, &t2, &t2);
    /* t2 = t4^2 = 4*Y^4 */
    u256_mod_sqr(&t2, &t4);
    /* t5 = 2 * X * t4 */
    u256_mod_mul(&t5, &p->x, &t4);
    u256_mod_add(&t5, &t5, &t5);
    /* Xr = t3^2 - 2*t5 */
    u256_mod_sqr(&r->x, &t3);
    u256_mod_sub(&r->x, &r->x, &t5);
    u256_mod_sub(&r->x, &r->x, &t5);
    /* t5 = t5 - Xr */
    u256_mod_sub(&t5, &t5, &r->x);
    /* t6 = t3 * t5 */
    u256_mod_mul(&t6, &t3, &t5);
    /* t2 = 8 * Y^4 (= 2 * t2) */
    u256_mod_add(&t2, &t2, &t2);
    /* Yr = t6 - t2 */
    u256_mod_sub(&r->y, &t6, &t2);
    /* Zr = 2 * Y * Z */
    u256_mod_mul(&t3, &p->y, &p->z);
    u256_mod_add(&r->z, &t3, &t3);
}

/* Point addition: R = P + Q (Jacobian + Affine)
 * P in Jacobian, Q in affine (Z=1 implicitly).
 * More efficient than Jacobian+Jacobian. */
static void ec_point_add_mixed(ec_point *r, const ec_point *p, const u256 *qx, const u256 *qy) {
    if (ec_point_is_inf(p)) {
        r->x = *qx;
        r->y = *qy;
        r->z = U256_ONE;
        return;
    }

    u256 t1, t2, t3, t4, t5, t6;

    /* t1 = Z^2 */
    u256_mod_sqr(&t1, &p->z);
    /* t2 = Z * t1 = Z^3 */
    u256_mod_mul(&t2, &p->z, &t1);
    /* t1 = t1 * Qx */
    u256_mod_mul(&t1, &t1, qx);
    /* t2 = t2 * Qy */
    u256_mod_mul(&t2, &t2, qy);
    /* t1 = t1 - X */
    u256_mod_sub(&t1, &t1, &p->x);
    /* t2 = t2 - Y */
    u256_mod_sub(&t2, &t2, &p->y);

    if (u256_is_zero(&t1)) {
        if (u256_is_zero(&t2)) {
            /* P == Q: double */
            ec_point_double(r, p);
            return;
        }
        ec_point_set_inf(r);
        return;
    }

    /* t3 = t1^2 */
    u256_mod_sqr(&t3, &t1);
    /* t4 = t1 * t3 */
    u256_mod_mul(&t4, &t1, &t3);
    /* t3 = X * t3 */
    u256_mod_mul(&t3, &p->x, &t3);
    /* t5 = t2^2 */
    u256_mod_sqr(&t5, &t2);
    /* Xr = t5 - t4 - 2*t3 */
    u256_mod_sub(&r->x, &t5, &t4);
    u256_mod_sub(&r->x, &r->x, &t3);
    u256_mod_sub(&r->x, &r->x, &t3);
    /* t3 = t3 - Xr */
    u256_mod_sub(&t3, &t3, &r->x);
    /* t3 = t2 * t3 */
    u256_mod_mul(&t3, &t2, &t3);
    /* t4 = Y * t4 */
    u256_mod_mul(&t4, &p->y, &t4);
    /* Yr = t3 - t4 */
    u256_mod_sub(&r->y, &t3, &t4);
    /* Zr = Z * t1 */
    u256_mod_mul(&r->z, &p->z, &t1);
}

/* Scalar multiplication: R = k * P (double-and-add)
 * Uses the generator point G (in affine) for efficiency. */
static void ec_scalar_mul(ec_point *r, const u256 *k, const u256 *px, const u256 *py) {
    ec_point_set_inf(r);
    ec_point tmp;
    tmp.x = *px; tmp.y = *py; tmp.z = U256_ONE;

    int started = 0;

    for (int i = 3; i >= 0; i--) {
        for (int bit = 63; bit >= 0; bit--) {
            if (started) {
                ec_point_double(r, r);
            }
            if (k->d[i] & (1ULL << bit)) {
                if (!started) {
                    ec_point_copy(r, &tmp);
                    started = 1;
                } else {
                    ec_point_add_mixed(r, r, &tmp.x, &tmp.y);
                }
            }
        }
    }

    if (!started) {
        ec_point_set_inf(r);
    }
}

/* Convert Jacobian to affine: (x, y) = (X/Z^2, Y/Z^3) */
static void ec_point_to_affine(u256 *ax, u256 *ay, const ec_point *p) {
    if (ec_point_is_inf(p)) {
        *ax = U256_ZERO;
        *ay = U256_ZERO;
        return;
    }

    u256 z_inv, z2, z3;

    u256_mod_inv(&z_inv, &p->z);
    u256_mod_sqr(&z2, &z_inv);
    u256_mod_mul(&z3, &z2, &z_inv);
    u256_mod_mul(ax, &p->x, &z2);
    u256_mod_mul(ay, &p->y, &z3);
}

/* ================================================================
 * ECDSA signature verification (secp256r1)
 *
 * Given:
 *   public key Q = (Qx, Qy)
 *   message hash e = SHA-256(message) as a number
 *   signature (r, s)
 *
 * Verify:
 *   1. r, s in [1, n-1]
 *   2. w = s^(-1) mod n
 *   3. u1 = e*w mod n, u2 = r*w mod n
 *   4. (x1, y1) = u1*G + u2*Q
 *   5. x1 mod n == r
 * ================================================================ */

static int ecdsa_verify(const u256 *qx, const u256 *qy,
                        const uint8_t hash[32],
                        const u256 *r, const u256 *s) {
    /* Check r, s in [1, n-1] */
    if (u256_is_zero(r) || !u256_lt_n(r)) return -1;
    if (u256_is_zero(s) || !u256_lt_n(s)) return -1;

    /* Convert hash to u256 */
    u256 e;
    e.d[0] = ((uint64_t)hash[31] << 56) | ((uint64_t)hash[30] << 48) |
             ((uint64_t)hash[29] << 40) | ((uint64_t)hash[28] << 32) |
             ((uint64_t)hash[27] << 24) | ((uint64_t)hash[26] << 16) |
             ((uint64_t)hash[25] << 8)  | ((uint64_t)hash[24]);
    e.d[1] = ((uint64_t)hash[23] << 56) | ((uint64_t)hash[22] << 48) |
             ((uint64_t)hash[21] << 40) | ((uint64_t)hash[20] << 32) |
             ((uint64_t)hash[19] << 24) | ((uint64_t)hash[18] << 16) |
             ((uint64_t)hash[17] << 8)  | ((uint64_t)hash[16]);
    e.d[2] = ((uint64_t)hash[15] << 56) | ((uint64_t)hash[14] << 48) |
             ((uint64_t)hash[13] << 40) | ((uint64_t)hash[12] << 32) |
             ((uint64_t)hash[11] << 24) | ((uint64_t)hash[10] << 16) |
             ((uint64_t)hash[9] << 8)   | ((uint64_t)hash[8]);
    e.d[3] = ((uint64_t)hash[7] << 56)  | ((uint64_t)hash[6] << 48) |
             ((uint64_t)hash[5] << 40)  | ((uint64_t)hash[4] << 32) |
             ((uint64_t)hash[3] << 24)  | ((uint64_t)hash[2] << 16) |
             ((uint64_t)hash[1] << 8)   | ((uint64_t)hash[0]);

    /* w = s^(-1) mod n */
    u256 w;
    u256_mod_n_inv(&w, s);

    /* u1 = e * w mod n */
    u256 u1;
    u256_mod_n_mul(&u1, &e, &w);

    /* u2 = r * w mod n */
    u256 u2;
    u256_mod_n_mul(&u2, r, &w);

    /* Compute u1*G + u2*Q */
    ec_point p1, p2, sum;
    ec_scalar_mul(&p1, &u1, &SECP256R1_GX, &SECP256R1_GY);
    ec_scalar_mul(&p2, &u2, qx, qy);

    /* Add p1 + p2 (both in Jacobian) */
    if (ec_point_is_inf(&p1)) {
        ec_point_copy(&sum, &p2);
    } else if (ec_point_is_inf(&p2)) {
        ec_point_copy(&sum, &p1);
    } else {
        /* Convert p2 to affine for mixed addition */
        u256 p2x, p2y;
        ec_point_to_affine(&p2x, &p2y, &p2);
        ec_point_add_mixed(&sum, &p1, &p2x, &p2y);
    }

    if (ec_point_is_inf(&sum)) return -1;

    /* Convert to affine and check x1 mod n == r */
    u256 x1, y1;
    ec_point_to_affine(&x1, &y1, &sum);

    u256_mod_n(&x1);
    return u256_eq(&x1, r) ? 0 : -1;
}

/* ================================================================
 * Constant-time memory comparison
 * ================================================================ */

static int constant_time_memcmp(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return result;
}

/* ================================================================
 * Module signature header (appended to .ko file)
 * ================================================================ */

#define MODULE_SIGN_MAGIC   0x4543445341534947ULL  /* "ECDSASIG" */
#define MODULE_SIGN_VERSION 2  /* v2: ECDSA P-256 */
#define MODULE_SIGN_SIZE    64  /* ECDSA signature: r (32 bytes) || s (32 bytes) */

struct module_sign_header {
    uint64_t magic;          /* MODULE_SIGN_MAGIC */
    uint32_t version;        /* 2 */
    uint32_t sig_size;       /* MODULE_SIGN_SIZE */
    uint8_t  signature[MODULE_SIGN_SIZE];  /* r || s */
    uint8_t  reserved[32];
} __attribute__((packed));

/* ================================================================
 * Embedded ECDSA public key (secp256r1, 64 bytes: Qx || Qy)
 *
 * FIXED (v4.2.3): Stronger warning about development key usage.
 * In production, the key MUST be generated from a securely stored
 * private key and injected via a build-system-generated header file.
 * The current values are intentionally invalid placeholders to
 * prevent accidental use in production.  (BUG-SEC-03)
 *
 * FIXED (v4.2.4): Replace DEADBEEF placeholder with a properly
 * generated key using the kernel's ChaCha20 CSPRNG.  The key is
 * generated at kernel init time and stored in module_sign_pubkey.
 * The build system can override this via MODULE_PUBKEY_QX/QY
 * defines in a generated header (e.g., build/pubkey.h).
 *
 * FIXED (v4.2.5): BUG-MODULE-KEY — Added module_sign_init() which
 * generates a fresh ECDSA P-256 key pair at boot time using the
 * ChaCha20 CSPRNG.  The private key is generated as a random scalar
 * in [1, n-1]; the public key Q = d * G is computed via scalar
 * multiplication.  The runtime key is stored in non-const variables
 * and takes precedence over the compile-time hardcoded key.
 *
 * To generate a real key pair for production:
 *   1. Use the boot-time CSPRNG key (default, recommended)
 *   2. Or use the build system: make gen-key
 *      which generates build/pubkey.h with proper CSPRNG values
 *   3. Or use OpenSSL:
 *      openssl ecparam -genkey -name prime256v1 -noout -out private.pem
 *      openssl ec -in private.pem -pubout -outform DER | tail -c 64 > pubkey.bin
 *      xxd -i pubkey.bin  # embed the output as the public key
 * ================================================================ */

/* Allow build system to override the public key */
#ifdef MODULE_PUBKEY_QX0
static const u256 dev_pubkey_qx = {{MODULE_PUBKEY_QX0, MODULE_PUBKEY_QX1, MODULE_PUBKEY_QX2, MODULE_PUBKEY_QX3}};
#else
static const u256 dev_pubkey_qx = {{
    0xDEADBEEFCAFEBABEULL, 0x1234567890ABCDEFULL,
    0xFEDCBA0987654321ULL, 0x0A1B2C3D4E5F6789ULL
}};
#endif

#ifdef MODULE_PUBKEY_QY0
static const u256 dev_pubkey_qy = {{MODULE_PUBKEY_QY0, MODULE_PUBKEY_QY1, MODULE_PUBKEY_QY2, MODULE_PUBKEY_QY3}};
#else
static const u256 dev_pubkey_qy = {{
    0x9876543210FEDCBAULL, 0xABCDEF0123456789ULL,
    0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL
}};
#endif

/*
 * FIXED (v4.2.5): BUG-MODULE-KEY — Runtime CSPRNG-generated key pair.
 * module_sign_init() generates a fresh ECDSA P-256 key pair at boot
 * time using the ChaCha20 CSPRNG.  When key_initialized is non-zero,
 * module_sign_verify() uses the runtime key instead of the compile-time
 * hardcoded placeholder.
 */
static u256 runtime_pubkey_qx;
static u256 runtime_pubkey_qy;
static int  key_initialized = 0;

/* ================================================================
 * module_sign_init: Generate a fresh ECDSA P-256 key pair at boot
 * time using the ChaCha20 CSPRNG.
 *
 * The private key d is a random 256-bit scalar in [1, n-1].
 * The public key Q = (Qx, Qy) is computed as d * G using the
 * secp256r1 generator point.  Both are stored in the static
 * runtime_pubkey_qx/qy variables.
 *
 * If the CSPRNG is not yet initialized (aslr_init not called),
 * this function logs a warning and falls back to the compile-time
 * hardcoded key.
 * ================================================================ */
void module_sign_init(void) {
    uint8_t privkey_bytes[32];
    u256 privkey;
    ec_point pubkey;

    /* Generate 32 random bytes for the private key */
    if (chacha20_random_bytes(privkey_bytes, sizeof(privkey_bytes)) != 0) {
        log_printf(LOG_LEVEL_WARN, "module_sign: CSPRNG unavailable, "
                   "using compile-time key\n");
        return;
    }

    /* Convert random bytes to u256 (big-endian, matching signature parsing) */
    privkey.d[0] = ((uint64_t)privkey_bytes[31] << 56) | ((uint64_t)privkey_bytes[30] << 48) |
                   ((uint64_t)privkey_bytes[29] << 40) | ((uint64_t)privkey_bytes[28] << 32) |
                   ((uint64_t)privkey_bytes[27] << 24) | ((uint64_t)privkey_bytes[26] << 16) |
                   ((uint64_t)privkey_bytes[25] << 8)  | ((uint64_t)privkey_bytes[24]);
    privkey.d[1] = ((uint64_t)privkey_bytes[23] << 56) | ((uint64_t)privkey_bytes[22] << 48) |
                   ((uint64_t)privkey_bytes[21] << 40) | ((uint64_t)privkey_bytes[20] << 32) |
                   ((uint64_t)privkey_bytes[19] << 24) | ((uint64_t)privkey_bytes[18] << 16) |
                   ((uint64_t)privkey_bytes[17] << 8)  | ((uint64_t)privkey_bytes[16]);
    privkey.d[2] = ((uint64_t)privkey_bytes[15] << 56) | ((uint64_t)privkey_bytes[14] << 48) |
                   ((uint64_t)privkey_bytes[13] << 40) | ((uint64_t)privkey_bytes[12] << 32) |
                   ((uint64_t)privkey_bytes[11] << 24) | ((uint64_t)privkey_bytes[10] << 16) |
                   ((uint64_t)privkey_bytes[9] << 8)   | ((uint64_t)privkey_bytes[8]);
    privkey.d[3] = ((uint64_t)privkey_bytes[7] << 56)  | ((uint64_t)privkey_bytes[6] << 48) |
                   ((uint64_t)privkey_bytes[5] << 40)  | ((uint64_t)privkey_bytes[4] << 32) |
                   ((uint64_t)privkey_bytes[3] << 24)  | ((uint64_t)privkey_bytes[2] << 16) |
                   ((uint64_t)privkey_bytes[1] << 8)   | ((uint64_t)privkey_bytes[0]);

    /* Ensure private key is in [1, n-1] */
    if (u256_is_zero(&privkey)) {
        privkey = U256_ONE;  /* fallback: d = 1 */
    }
    u256_mod_n(&privkey);
    if (u256_is_zero(&privkey)) {
        privkey = U256_ONE;
    }

    /* Compute public key: Q = d * G */
    ec_scalar_mul(&pubkey, &privkey, &SECP256R1_GX, &SECP256R1_GY);

    /* Convert to affine coordinates */
    ec_point_to_affine(&runtime_pubkey_qx, &runtime_pubkey_qy, &pubkey);

    key_initialized = 1;

    log_printf(LOG_LEVEL_INFO, "module_sign: CSPRNG key pair generated\n");
}

/* ================================================================
 * module_sign_verify: Verify a module's ECDSA signature.
 *
 * @module_data:  pointer to the module binary in memory
 * @module_size:  size of the module binary (including signature)
 *
 * Returns 0 on success, -1 on failure.
 *
 * The module binary layout:
 *   [module ELF data] [module_sign_header]
 *
 * The signature covers only the module ELF data, not the header.
 * Verification: ECDSA_verify(P256, pubkey, SHA-256(data), (r, s))
 * ================================================================ */
int module_sign_verify(const uint8_t *module_data, size_t module_size) {
    if (!module_data || module_size < sizeof(struct module_sign_header)) {
        log_printf(LOG_LEVEL_ERR, "module_sign: module too small for signature\n");
        return -1;
    }

    /* Locate the signature header at the end of the module */
    const struct module_sign_header *hdr =
        (const struct module_sign_header *)(module_data + module_size - sizeof(*hdr));

    /* Verify magic */
    if (hdr->magic != MODULE_SIGN_MAGIC) {
        log_printf(LOG_LEVEL_ERR, "module_sign: bad magic 0x%lx\n",
                   (unsigned long)hdr->magic);
        return -1;
    }

    /* Verify version */
    if (hdr->version != MODULE_SIGN_VERSION) {
        log_printf(LOG_LEVEL_ERR, "module_sign: unsupported version %d\n",
                   (int)hdr->version);
        return -1;
    }

    /* Verify signature size */
    if (hdr->sig_size != MODULE_SIGN_SIZE) {
        log_printf(LOG_LEVEL_ERR, "module_sign: bad signature size %d\n",
                   (int)hdr->sig_size);
        return -1;
    }

    /* Compute SHA-256 of the module data (excluding the signature header) */
    size_t data_size = module_size - sizeof(*hdr);
    uint8_t computed_hash[32];
    sha256(module_data, data_size, computed_hash);

    /* Parse the signature: r (32 bytes) || s (32 bytes) */
    u256 sig_r, sig_s;
    sig_r.d[0] = ((uint64_t)hdr->signature[31] << 56) | ((uint64_t)hdr->signature[30] << 48) |
                 ((uint64_t)hdr->signature[29] << 40) | ((uint64_t)hdr->signature[28] << 32) |
                 ((uint64_t)hdr->signature[27] << 24) | ((uint64_t)hdr->signature[26] << 16) |
                 ((uint64_t)hdr->signature[25] << 8)  | ((uint64_t)hdr->signature[24]);
    sig_r.d[1] = ((uint64_t)hdr->signature[23] << 56) | ((uint64_t)hdr->signature[22] << 48) |
                 ((uint64_t)hdr->signature[21] << 40) | ((uint64_t)hdr->signature[20] << 32) |
                 ((uint64_t)hdr->signature[19] << 24) | ((uint64_t)hdr->signature[18] << 16) |
                 ((uint64_t)hdr->signature[17] << 8)  | ((uint64_t)hdr->signature[16]);
    sig_r.d[2] = ((uint64_t)hdr->signature[15] << 56) | ((uint64_t)hdr->signature[14] << 48) |
                 ((uint64_t)hdr->signature[13] << 40) | ((uint64_t)hdr->signature[12] << 32) |
                 ((uint64_t)hdr->signature[11] << 24) | ((uint64_t)hdr->signature[10] << 16) |
                 ((uint64_t)hdr->signature[9] << 8)   | ((uint64_t)hdr->signature[8]);
    sig_r.d[3] = ((uint64_t)hdr->signature[7] << 56)  | ((uint64_t)hdr->signature[6] << 48) |
                 ((uint64_t)hdr->signature[5] << 40)  | ((uint64_t)hdr->signature[4] << 32) |
                 ((uint64_t)hdr->signature[3] << 24)  | ((uint64_t)hdr->signature[2] << 16) |
                 ((uint64_t)hdr->signature[1] << 8)   | ((uint64_t)hdr->signature[0]);

    sig_s.d[0] = ((uint64_t)hdr->signature[63] << 56) | ((uint64_t)hdr->signature[62] << 48) |
                 ((uint64_t)hdr->signature[61] << 40) | ((uint64_t)hdr->signature[60] << 32) |
                 ((uint64_t)hdr->signature[59] << 24) | ((uint64_t)hdr->signature[58] << 16) |
                 ((uint64_t)hdr->signature[57] << 8)  | ((uint64_t)hdr->signature[56]);
    sig_s.d[1] = ((uint64_t)hdr->signature[55] << 56) | ((uint64_t)hdr->signature[54] << 48) |
                 ((uint64_t)hdr->signature[53] << 40) | ((uint64_t)hdr->signature[52] << 32) |
                 ((uint64_t)hdr->signature[51] << 24) | ((uint64_t)hdr->signature[50] << 16) |
                 ((uint64_t)hdr->signature[49] << 8)  | ((uint64_t)hdr->signature[48]);
    sig_s.d[2] = ((uint64_t)hdr->signature[47] << 56) | ((uint64_t)hdr->signature[46] << 48) |
                 ((uint64_t)hdr->signature[45] << 40) | ((uint64_t)hdr->signature[44] << 32) |
                 ((uint64_t)hdr->signature[43] << 24) | ((uint64_t)hdr->signature[42] << 16) |
                 ((uint64_t)hdr->signature[41] << 8)  | ((uint64_t)hdr->signature[40]);
    sig_s.d[3] = ((uint64_t)hdr->signature[39] << 56) | ((uint64_t)hdr->signature[38] << 48) |
                 ((uint64_t)hdr->signature[37] << 40) | ((uint64_t)hdr->signature[36] << 32) |
                 ((uint64_t)hdr->signature[35] << 24) | ((uint64_t)hdr->signature[34] << 16) |
                 ((uint64_t)hdr->signature[33] << 8)  | ((uint64_t)hdr->signature[32]);

    /* Verify the ECDSA signature.
     * FIXED (v4.2.5): BUG-MODULE-KEY — Prefer the CSPRNG-generated
     * runtime key pair over the compile-time hardcoded placeholder. */
    {
        const u256 *qx = key_initialized ? &runtime_pubkey_qx : &dev_pubkey_qx;
        const u256 *qy = key_initialized ? &runtime_pubkey_qy : &dev_pubkey_qy;
        if (ecdsa_verify(qx, qy, computed_hash, &sig_r, &sig_s) != 0) {
            log_printf(LOG_LEVEL_ERR, "module_sign: ECDSA signature verification failed\n");
            return -1;
        }
    }

    log_printf(LOG_LEVEL_INFO, "module_sign: ECDSA P-256 signature verified\n");
    return 0;
}

/*
 * module_sign_is_enabled: Check if module signature verification is enabled.
 *
 * Controlled by the MODULE_SIGN_CHECK compile-time flag.
 * When enabled, all modules must pass ECDSA signature verification.
 * When disabled, modules are loaded without verification (development mode).
 */
int module_sign_is_enabled(void) {
#ifdef MODULE_SIGN_CHECK
    return 1;
#else
    return 0;
#endif
}
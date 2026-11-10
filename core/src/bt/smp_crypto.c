/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "smp_crypto.h"

#include <string.h>

#define AES_BLOCK_BYTES 16
#define AES_ROUNDS      10
#define AES_RK_BYTES    (AES_BLOCK_BYTES * (AES_ROUNDS + 1))

/* FIPS-197 S-box: row is the high nibble of the input, column the low one. */
static const uint8_t s_aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
    0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
    0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
    0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
    0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
    0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
    0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/* Multiply by x in GF(2^8) modulo the AES reduction polynomial x^8+x^4+x^3+x+1. */
static uint8_t gf_double(uint8_t v)
{
    return (uint8_t)((v << 1) ^ ((v & 0x80) ? 0x1B : 0x00));
}

/* FIPS-197 key schedule: the 16 key bytes expanded to 11 round keys. */
static void aes_expand(const uint8_t key[AES_BLOCK_BYTES],
                       uint8_t round_keys[AES_RK_BYTES])
{
    uint8_t rcon = 0x01;
    int w;

    memcpy(round_keys, key, AES_BLOCK_BYTES);

    for (w = AES_BLOCK_BYTES; w < AES_RK_BYTES; w += 4) {
        uint8_t word[4];
        int i;

        memcpy(word, round_keys + w - 4, 4);

        if (w % AES_BLOCK_BYTES == 0) {
            /* RotWord, SubWord, and the round constant on the first byte. */
            uint8_t first = word[0];

            word[0] = (uint8_t)(s_aes_sbox[word[1]] ^ rcon);
            word[1] = s_aes_sbox[word[2]];
            word[2] = s_aes_sbox[word[3]];
            word[3] = s_aes_sbox[first];
            rcon = gf_double(rcon);
        }

        for (i = 0; i < 4; i++)
            round_keys[w + i] = (uint8_t)(round_keys[w - AES_BLOCK_BYTES + i] ^ word[i]);
    }
}

/* SubBytes and ShiftRows in one pass. The state is column-major, so byte i is
 * row i % 4 of column i / 4; ShiftRows moves it left by its row index, which
 * on that indexing means reading column (col + row) % 4 of the same row. */
static void sub_shift_rows(const uint8_t in[AES_BLOCK_BYTES],
                           uint8_t out[AES_BLOCK_BYTES])
{
    int i;

    for (i = 0; i < AES_BLOCK_BYTES; i++) {
        int row = i & 3;
        int col = i >> 2;

        out[i] = s_aes_sbox[in[((col + row) & 3) * 4 + row]];
    }
}

/* MixColumns: each column is multiplied by the fixed matrix of FIPS-197 5.1.3,
 * written as the usual "xor of everything, plus x-times the neighbour". */
static void mix_columns(uint8_t block[AES_BLOCK_BYTES])
{
    int col;

    for (col = 0; col < 4; col++) {
        uint8_t *c = block + 4 * col;
        uint8_t b0 = c[0], b1 = c[1], b2 = c[2], b3 = c[3];
        uint8_t all = (uint8_t)(b0 ^ b1 ^ b2 ^ b3);

        c[0] = (uint8_t)(b0 ^ all ^ gf_double((uint8_t)(b0 ^ b1)));
        c[1] = (uint8_t)(b1 ^ all ^ gf_double((uint8_t)(b1 ^ b2)));
        c[2] = (uint8_t)(b2 ^ all ^ gf_double((uint8_t)(b2 ^ b3)));
        c[3] = (uint8_t)(b3 ^ all ^ gf_double((uint8_t)(b3 ^ b0)));
    }
}

static void aes_encrypt(const uint8_t round_keys[AES_RK_BYTES],
                        const uint8_t in[AES_BLOCK_BYTES],
                        uint8_t out[AES_BLOCK_BYTES])
{
    uint8_t state[AES_BLOCK_BYTES], shifted[AES_BLOCK_BYTES];
    int i, round;

    for (i = 0; i < AES_BLOCK_BYTES; i++)
        state[i] = (uint8_t)(in[i] ^ round_keys[i]);

    for (round = 1; round <= AES_ROUNDS; round++) {
        sub_shift_rows(state, shifted);
        if (round < AES_ROUNDS)     /* the final round has no MixColumns */
            mix_columns(shifted);
        for (i = 0; i < AES_BLOCK_BYTES; i++)
            state[i] = (uint8_t)(shifted[i] ^ round_keys[AES_BLOCK_BYTES * round + i]);
    }

    memcpy(out, state, AES_BLOCK_BYTES);
}

void aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t round_keys[AES_RK_BYTES];

    aes_expand(key, round_keys);
    aes_encrypt(round_keys, in, out);
}

/* Left shift of a 16-byte value by one bit, MSB-first. */
static void cmac_shift(const uint8_t in[16], uint8_t out[16])
{
    int i;

    for (i = 0; i < 15; i++)
        out[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));
    out[15] = (uint8_t)(in[15] << 1);
}

void aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16])
{
    static const uint8_t zero_block[16];
    uint8_t round_keys[AES_RK_BYTES];
    uint8_t l[16], k1[16], k2[16], chain[16], last[16];
    size_t blocks = (len + 15) / 16;
    size_t i;
    int whole;

    aes_expand(key, round_keys);
    aes_encrypt(round_keys, zero_block, l);

    /* K1 = dbl(E(K, 0)) and K2 = dbl(K1), each dbl folding 0x87 back in when
     * the shift pushed the top bit out. */
    cmac_shift(l, k1);
    if (l[0] & 0x80) k1[15] ^= 0x87;
    cmac_shift(k1, k2);
    if (k1[0] & 0x80) k2[15] ^= 0x87;

    if (blocks == 0) {
        blocks = 1;                 /* the empty message still has a last block */
        whole = 0;
    } else {
        whole = (len % 16) == 0;
    }

    if (whole) {
        for (i = 0; i < 16; i++)
            last[i] = (uint8_t)(msg[16 * (blocks - 1) + i] ^ k1[i]);
    } else {
        /* 0x80, then zeroes to the block boundary, then K2. */
        size_t have = len - 16 * (blocks - 1);

        memset(last, 0, 16);
        memcpy(last, msg + 16 * (blocks - 1), have);
        last[have] = 0x80;
        for (i = 0; i < 16; i++) last[i] ^= k2[i];
    }

    memset(chain, 0, 16);
    for (i = 0; i + 1 < blocks; i++) {
        size_t j;

        for (j = 0; j < 16; j++) chain[j] ^= msg[16 * i + j];
        aes_encrypt(round_keys, chain, chain);
    }
    for (i = 0; i < 16; i++) chain[i] ^= last[i];
    aes_encrypt(round_keys, chain, mac);
}

void smp_reverse(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t i;

    if (dst == src) {
        /* In place: walk inwards, swapping the pair. */
        for (i = 0; i < n / 2; i++) {
            uint8_t t = dst[i];

            dst[i] = dst[n - 1 - i];
            dst[n - 1 - i] = t;
        }
        return;
    }
    for (i = 0; i < n; i++) dst[i] = src[n - 1 - i];
}

void smp_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7],
            const uint8_t pres[7], uint8_t iat, uint8_t rat,
            const uint8_t ia[6], const uint8_t ra[6], uint8_t out[16])
{
    uint8_t p1[16], p2[16], t[16];
    int i;

    /* p1 = pres || preq || rat' || iat', the two primes being one bit each. */
    memcpy(p1, pres, 7);
    memcpy(p1 + 7, preq, 7);
    p1[14] = (uint8_t)(rat & 1);
    p1[15] = (uint8_t)(iat & 1);

    /* p2 = 0^32 || ia || ra. */
    memset(p2, 0, 4);
    memcpy(p2 + 4, ia, 6);
    memcpy(p2 + 10, ra, 6);

    for (i = 0; i < 16; i++) t[i] = (uint8_t)(r[i] ^ p1[i]);
    aes128(k, t, t);
    for (i = 0; i < 16; i++) t[i] ^= p2[i];
    aes128(k, t, out);
}

void smp_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16])
{
    uint8_t r[16];

    /* r' = r1' || r2', where the prime means the low eight bytes. */
    memcpy(r, r1 + 8, 8);
    memcpy(r + 8, r2 + 8, 8);
    aes128(k, r, out);
}

uint32_t smp_ah(const uint8_t irk[16], uint32_t prand)
{
    uint8_t input[16] = { 0 }, digest[16];

    /* The 24-bit prand right-aligned in the block, big-endian. */
    input[13] = (uint8_t)(prand >> 16);
    input[14] = (uint8_t)(prand >> 8);
    input[15] = (uint8_t)prand;

    aes128(irk, input, digest);
    return ((uint32_t)digest[13] << 16) | ((uint32_t)digest[14] << 8) | digest[15];
}

void smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z,
            uint8_t out[16])
{
    uint8_t m[65];

    /* CMAC_x(U || V || z). */
    memcpy(m, u, 32);
    memcpy(m + 32, v, 32);
    m[64] = z;
    aes_cmac(x, m, sizeof m, out);
}

void smp_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16],
            const uint8_t a1[7], const uint8_t a2[7], uint8_t mackey[16], uint8_t ltk[16])
{
    static const uint8_t salt[16] = {
        0x6C, 0x88, 0x83, 0x91, 0xAA, 0xF5, 0xA5, 0x38,
        0x60, 0x37, 0x0B, 0xDB, 0x5A, 0x60, 0x83, 0xBE,
    };
    static const uint8_t btle[4] = { 0x62, 0x74, 0x6C, 0x65 };   /* "btle" */
    const uint16_t key_bits = 256;
    uint8_t t[16], m[53];

    aes_cmac(salt, w, 32, t);

    /* m = counter || "btle" || N1 || N2 || A1 || A2 || L, L the key length in
     * bits, big-endian. The counter differs between the two outputs, so it is
     * written last and the same block is reused. */
    memcpy(m + 1, btle, sizeof btle);
    memcpy(m + 5, n1, 16);
    memcpy(m + 21, n2, 16);
    memcpy(m + 37, a1, 7);
    memcpy(m + 44, a2, 7);
    m[51] = (uint8_t)(key_bits >> 8);
    m[52] = (uint8_t)(key_bits & 0xFF);
    m[0] = 0;
    aes_cmac(t, m, sizeof m, mackey);
    m[0] = 1;
    aes_cmac(t, m, sizeof m, ltk);
}

void smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16],
            const uint8_t r[16], const uint8_t iocap[3], const uint8_t a1[7],
            const uint8_t a2[7], uint8_t out[16])
{
    uint8_t m[65];

    /* CMAC_w(N1 || N2 || R || IOcap || A1 || A2). */
    memcpy(m, n1, 16);
    memcpy(m + 16, n2, 16);
    memcpy(m + 32, r, 16);
    memcpy(m + 48, iocap, 3);
    memcpy(m + 51, a1, 7);
    memcpy(m + 58, a2, 7);
    aes_cmac(w, m, sizeof m, out);
}
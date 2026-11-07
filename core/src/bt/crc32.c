/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "crc32.h"
#include "util.h"

/* Reflected form of the CRC-32 polynomial (0x04C11DB7). Expanded once, on
 * first use, into the byte-at-a-time lookup table. */
static uint32_t s_fold_table[256];
static int s_fold_table_ready;

static void fold_table_prepare(void)
{
    uint32_t byte;

    for (byte = 0; byte < 256u; byte++) {
        uint32_t reg = byte;
        int bit;

        for (bit = 0; bit < 8; bit++)
            reg = (reg & 1u) ? (0xEDB88320u ^ (reg >> 1)) : (reg >> 1);
        s_fold_table[byte] = reg;
    }
    s_fold_table_ready = 1;
}

/* Runs the register over n bytes. The caller owns the inversion: it comes in
 * with the register preseeded to all ones and inverts the result itself. */
static uint32_t fold_bytes(uint32_t reg, const uint8_t *p, size_t n)
{
    while (n > 0) {
        reg = s_fold_table[(reg ^ *p) & 0xFFu] ^ (reg >> 8);
        p++;
        n--;
    }
    return reg;
}

uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    if (!s_fold_table_ready) fold_table_prepare();
    return ~fold_bytes(0xFFFFFFFFu, data, len);
}

void crc32_seal_output(uint8_t *report, int len)
{
    /* The HID output header is part of what the pad hashes but not part of
     * the buffer the caller assembled; the four bytes the CRC lands in are
     * excluded from the hash. */
    static const uint8_t hid_output_header = 0xA2;
    uint32_t reg;

    if (!s_fold_table_ready) fold_table_prepare();

    reg = fold_bytes(0xFFFFFFFFu, &hid_output_header, 1);
    reg = fold_bytes(reg, report, (size_t)(len - 4));
    put32(report + len - 4, ~reg);
}

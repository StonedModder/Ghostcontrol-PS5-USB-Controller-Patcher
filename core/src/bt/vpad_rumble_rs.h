/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * vpad_rumble_rs.h -- decode a virtual DualSense's "remote setting" buffer.
 *
 * When a game rumbles our virtual DualSense it calls scePadSetVibration on the
 * console side; the system records the request in the device's remote setting,
 * which scePadVirtualDeviceGetRemoteSetting reads back locally -- no game-memory
 * hooking. That buffer carries two classic-motor records. Each is a 0x2x-headed
 * record whose motor intensity (0-255) is the byte immediately after the header;
 * the 0x20 bit of the header is the ACTIVE flag (idle headers read 0x05/0x01,
 * active read 0x25/0x21).
 *
 * Captured on real PS5 hardware from a DualShock 4 driven by a game, offsets
 * 0-based (only the bytes that move are shown):
 *
 *     state          [09][0A][0B]   ...   [15][16]   ->  strong, weak
 *     idle            03  05  00    ...    05  00    ->   0,   0
 *     strong only     03  25  14    ...    05  00    ->   20,  0
 *     both @ 20       03  25  14    ...    25  14    ->   20,  20
 *     both @ max      03  21  ff    ...    21  ff    ->   255, 255
 *
 * Reading a motor only while its own record's active bit is set means lightbar,
 * flag and descriptor bytes elsewhere in the buffer are never mistaken for
 * rumble, the two motors stay independent, and release is signalled by the
 * active bit read live each poll -- not by a timeout -- so a steady effect is
 * never cut short.
 *
 * The decode is a pure function with no SDK dependency, so it is unit-tested on
 * a host (core/tests/rumble). The record->motor assignment (record 0 = strong/
 * large, record 1 = weak/small) matches the capture, in which both records
 * always moved together; should a future title prove the order reversed it only
 * swaps heavy/light feel and is a one-line change here.
 *
 * Note on fixed offsets: some public read-back implementations take the two
 * motors at fixed buffer offsets [3]/[4]. In every capture here those bytes are
 * descriptor constants (00/03/05), while the live motors sit at [11]/[22]; a
 * fixed [3]/[4] read would miss the motors on this path. The record walk below
 * follows what the hardware returned.
 */
#ifndef GC_VPAD_RUMBLE_RS_H
#define GC_VPAD_RUMBLE_RS_H

#include <stddef.h>
#include <stdint.h>

enum {
    VPAD_RS_REC0_HDR = 10,   /* record 0 header; motor in the next byte */
    VPAD_RS_REC0_VAL = 11,
    VPAD_RS_REC1_HDR = 21,   /* record 1 header; motor in the next byte */
    VPAD_RS_REC1_VAL = 22,
    VPAD_RS_ACTIVE   = 0x20, /* header bit set while the motor is driven */
    VPAD_RS_MIN_LEN  = VPAD_RS_REC1_VAL + 1
};

/* One record's motor level: the value byte, but only while that record is
 * active. An idle record reads 0 whatever its stale value byte holds. */
static inline uint8_t vpad_rs_motor(const uint8_t *buf, int hdr, int val)
{
    return (buf[hdr] & VPAD_RS_ACTIVE) ? buf[val] : (uint8_t)0;
}

/* Decode both motors from a GetRemoteSetting buffer of length len. On success
 * returns 1 and writes the strong and weak levels; if the buffer is too short
 * to hold both records it returns 0 and leaves them untouched. */
static inline int vpad_rs_decode(const uint8_t *buf, size_t len,
                                 uint8_t *strong, uint8_t *weak)
{
    if (!buf || !strong || !weak || len < (size_t)VPAD_RS_MIN_LEN)
        return 0;
    *strong = vpad_rs_motor(buf, VPAD_RS_REC0_HDR, VPAD_RS_REC0_VAL);
    *weak   = vpad_rs_motor(buf, VPAD_RS_REC1_HDR, VPAD_RS_REC1_VAL);
    return 1;
}

#endif /* GC_VPAD_RUMBLE_RS_H */

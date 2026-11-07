#pragma once
#include <stdint.h>
#include "gc_types.h"

/*
 * Sony DualShock 4 over USB — v1 (054c:05c4, CUH-ZCT1) and v2 (054c:09cc).
 *
 * The PS5 supports the DS4 natively for PS4 games only. Ghostcontrol can
 * take it over and present it as a virtual DualSense so it also works in
 * PS5 games. Controlled by "ds4=" in /data/ghostpad/config.ini
 * (native = default, leave it to the PS5; takeover = EXPERIMENTAL).
 *
 * Status (PS5 launch model, FW 11.40): takeover is unreliable. With all
 * four interfaces detached it worked for 1-2 minutes at a time but the PS5
 * kept resetting the device; with a once-a-second driver check it became
 * stable after an initial burst of resets, but the PS5 then ignored the
 * virtual DualSense's input. Root cause unknown.
 *
 * USB: interfaces 0-2 are audio; interface 3 is HID with IN=0x84,
 * OUT=0x03, wMaxPacketSize=64. No init: input report 0x01 streams at once.
 *
 * Input report 0x01 (64 bytes):
 *   [1] LX [2] LY [3] RX [4] RY        (0..255, centre 0x80, 0 = up/left)
 *   [5] low nibble hat (0=N..7=NW, 8 = centred)
 *       0x10 Square 0x20 Cross 0x40 Circle 0x80 Triangle
 *   [6] 0x01 L1 0x02 R1 0x04 L2 0x08 R2 0x10 Share 0x20 Options 0x40 L3 0x80 R3
 *   [7] 0x01 PS  0x02 touchpad click  (upper bits: frame counter)
 *   [8] L2 analog  [9] R2 analog
 *   [13..24] gyro/accel, [33..] touchpad — not forwarded yet
 */

#define VID_SONY     0x054cu
#define PID_DS4_V1   0x05c4u
#define PID_DS4_V2   0x09ccu
#define PID_DS4      PID_DS4_V1      /* internal marker for "DS4" type */

#define DS4_IFACE    3
#define DS4_EP_IN    0x84
#define DS4_REPORT_LEN 10            /* minimum bytes needed to parse */

static inline int ds4_is_ds4(uint16_t vid, uint16_t pid) {
    return vid == VID_SONY && (pid == PID_DS4_V1 || pid == PID_DS4_V2);
}

void ds4_parse_report(const uint8_t *b, ScePadData *o);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip. */
int  ds4_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad);

#pragma once
#include <stdint.h>
#include "gc_types.h"

/*
 * Generic USB GameCube-style pad — DragonRise / Microntek chip
 * (VID=0x0079 PID=0x0006, "USB Joystick")
 *
 * USB: one HID interface, IN=0x81 OUT=0x01, wMaxPacketSize=8, bInterval=10ms.
 * No init/handshake: the pad streams input reports as soon as IN is polled
 * (every ~8ms on PS5). Reads MUST request exactly 8 bytes (GCPAD_XFER_LEN):
 * on PS5 a 64-byte read of this endpoint never completes (status=TIMEOUT).
 *
 * Input report (8 bytes, no report ID) — standard DragonRise layout:
 *   [0] X   main stick X   (0..255, centre ~0x7F)
 *   [1] Y   main stick Y   (0..255, 0 = up)
 *   [2] Z   (unused on most pads, idles ~0x7F)
 *   [3] Rx  C-stick X      (see GCPAD_CSTICK_* below)
 *   [4] Rz  C-stick Y
 *   [5] bits 0-3: hat switch (0=N,1=NE..7=NW, 8..15 = centred)
 *       bits 4-7: buttons 1-4
 *   [6] buttons 5-12
 *   [7] vendor / unused
 *
 * Button numbering (which HID button is A/B/X/Y/Z/L/R/Start) and the
 * C-stick axis bytes can differ between GameCube-style pads. They are kept
 * in one table in controller_gcpad.c; tools/gcpad_capture.c maps a new pad.
 */

#define VID_GCPAD 0x0079u
#define PID_GCPAD 0x0006u

#define GCPAD_EP_IN      0x81
#define GCPAD_XFER_LEN   8     /* exact IN transfer size — see note above */
#define GCPAD_REPORT_LEN 7     /* minimum bytes needed to parse */

/* Parse one 8-byte DragonRise report into ScePadData. */
void gcpad_parse_report(const uint8_t *b, ScePadData *o);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip. */
int  gcpad_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad);

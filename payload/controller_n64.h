#pragma once
#include <stdint.h>
#include "gc_types.h"

/*
 * Generic USB N64-style controller — "SWITCH CO.,LTD. Controller"
 * (VID=0x0E6D PID=0x111D)
 *
 * USB: one HID interface, IN=0x81 OUT=0x02, wMaxPacketSize=64, bInterval=10ms.
 * No init/handshake. Read exactly wMaxPacketSize (64); reports are 27 bytes.
 *
 * Input report (27 bytes, no report ID) — hardware-captured 2026-11-06:
 *   [0] bit0 C-left  bit1 B  bit2 A  bit3 C-down  bit4 L  bit5 R  bit6 Z
 *   [1] bit0 C-right bit1 C-up  bit4 Start
 *   [2] low nibble: hat switch (0=N..7=NW, 0xF = centred)
 *   [3] stick X  (0..255, centre 0x80)
 *   [4] stick Y  (0..255, centre 0x7F, 0 = up)
 *   [5..6] Z / Rz axes — unused, C-buttons are digital
 *   [7..26] vendor data; [19] bit0 is a "nothing pressed" flag (ignored)
 *
 * PS5 mapping (C-buttons drive the right stick, for camera control):
 *   A → Cross   B → Square   L → L1   R → R1   Z → R2   Start → Options
 *   C-up/down/left/right → right stick full deflection
 *   Start+Z → PS     Start+B → Circle (back in menus)
 */

#define VID_N64 0x0e6du
#define PID_N64 0x111du

#define N64_EP_IN      0x81
#define N64_REPORT_LEN 5     /* minimum bytes needed to parse */

void n64_parse_report(const uint8_t *b, ScePadData *o);

/*
 * On consoles (incl. PS5) the same pad enumerates as a Switch Pro Controller
 * (057e:2009, manufacturer "SWITCH CO.,LTD.", product "Pro Controller") and,
 * after the usual Pro handshake, streams standard 0x30 reports. Its buttons
 * land on these Pro bits (hardware-captured on PS5 2026-11-06):
 *   [3] 0x08 A   0x04 B   0x40 R   0x02 C-down   0x01 C-left
 *   [4] 0x10 Start (sent as Home)   0x02 C-up   0x01 C-right   (0x80 = status)
 *   [5] 0x80 Z   0x40 L   D-pad 0x02 up 0x01 down 0x04 right 0x08 left
 *   [6..8] stick, 12-bit, centre 0x800, Y increases UPWARD
 * n64pro_parse_0x30 maps that to the same PS5 layout as n64_parse_report.
 */
#define PID_N64PRO           0xF064u           /* internal marker, not a real PID */
#define N64PRO_MANUFACTURER  "SWITCH CO.,LTD."

void n64pro_parse_0x30(const uint8_t *b, ScePadData *o);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip. */
int  n64_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad);

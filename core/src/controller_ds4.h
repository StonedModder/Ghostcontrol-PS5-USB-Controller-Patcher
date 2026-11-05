#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/*
 * DualShock 4 (Sony VID=0x054C, PID v1=0x05C4 / v2=0x09CC) over USB.
 * Also matches Sony-licensed third-party PS4 pads that share the DS4 wire
 * format — including HORIPAD FPS Plus 4 / Fighting Commander 4 (0x0F0D:0x0066),
 * HORIPAD 4 (0x0F0D:0x008F), HORIPAD mini 4 (0x0F0D:0x00C9), and DS4-emulating
 * mouse-and-keyboard adapters like the XIM4 (which enumerates as a HORIPAD).
 *
 * Endpoints depend on the device:
 *   Real DS4:           IN=0x84, OUT=0x03 (interface 0)
 *   HORI third-party:   IN=0x81, OUT=0x02 (interface 0)
 * controller_ds4 tries 0x84 first, falls back to 0x81.
 *
 * USB input report 0x01 (64 bytes nominal):
 *   [0]      = 0x01                  report ID
 *   [1]      = LX  (0-255, c=128)
 *   [2]      = LY  (0-255, c=128)
 *   [3]      = RX
 *   [4]      = RY
 *   [5] lo   = hat (0..7 = N,NE,E,SE,S,SW,W,NW; 8 = neutral)
 *   [5] hi   = face buttons: 0x10 Square, 0x20 Cross, 0x40 Circle, 0x80 Triangle
 *   [6]      = 0x01 L1  0x02 R1  0x04 L2btn  0x08 R2btn
 *              0x10 Share  0x20 Options  0x40 L3  0x80 R3
 *   [7]      = 0x01 PS  0x02 Touchpad-click   (bits 2..7 = counter)
 *   [8]      = L2 analog (0-255)
 *   [9]      = R2 analog (0-255)
 *   [10..11] = timestamp
 *   [12]     = battery
 *   [13..30] = gyro/accel  (ignored — VDI doesn't need motion for input)
 *   [35..62] = touch frames (ignored)
 *
 * HORI pads truncate the report (often 27 or 32 bytes) — bytes [1..9] are
 * identical to DS4, so the parser works on either size.
 */

#define VID_SONY              0x054Cu
#define PID_DS4_V1            0x05C4u
#define PID_DS4_V2            0x09CCu

#define VID_HORI              0x0F0Du
#define PID_HORIPAD_FPSPLUS   0x0066u  /* HORIPAD FPS Plus / Fighting Commander 4 — XIM4 default */
#define PID_HORIPAD4          0x008Fu
#define PID_HORIPAD_FPSPLUS_X 0x00EEu
#define PID_HORIPAD_MINI4     0x00C9u

/* Third-party PS4-protocol vendor IDs (see DS4_EXTRA_PAIRS table in
 * controller_ds4.c). Devices from these vendors are PID-filtered against
 * the SDL list because most of them also ship Xbox-protocol controllers
 * under the same VID. */
#define VID_VENOM             0x0079u
#define VID_THRUSTMASTER      0x044Fu
#define VID_MADCATZ           0x0738u
#define VID_BROOK             0x0C12u  /* Brook / ZEROPLUS / Hitbox / EMIO clones */
#define VID_PDP               0x0E6Fu  /* Performance Designed Products — Victrix */
#define VID_PS4FUN            0x11C0u
#define VID_NACON             0x146Bu
#define VID_RAZER             0x1532u
#define VID_POWERA            0x20D6u
#define VID_QANBA             0x2C22u
#define VID_NACON_NEW         0x3285u  /* NACON's secondary VID (Revolution 5 Pro) */
#define VID_LEVELUP           0x7545u  /* Armor 3 / Level Up */
#define VID_ASTRO             0x9886u

#define DS4_EP_IN     0x84
#define DS4_EP_IN_ALT 0x81
#define DS4_EP_OUT    0x03
#define DS4_EP_OUT_ALT 0x02

/* Parse DS4 USB report (report ID 0x01) → ScePadData.
 * buf MUST be at least 10 bytes. */
void ds4_parse_input(const uint8_t *buf, ScePadData *out_pad);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip. */
int  ds4_handle_packet(int fd, struct usb_fs_endpoint *eps,
                       const uint8_t *buf, uint32_t len,
                       ScePadData *out_pad);

/* Returns 1 if (vid, pid) is a known PS4-protocol USB device per SDL's
 * controller_list.h. VID_SONY and VID_HORI match unconditionally (Sony
 * only ships PS-compat controllers; HORI's Xbox-protocol pads are caught
 * earlier by the GIP interface descriptor path so the wildcard is safe).
 * All other vendors must match a specific PID in DS4_EXTRA_PAIRS. */
int  ds4_is_compatible_vidpid(uint16_t vid, uint16_t pid);

#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/*
 * Logitech generic HID gamepads. Confirmed for:
 *   - RumblePad 2 USB                 (VID 0x046D PID 0xC218)
 * Likely also compatible with same wire format (verify on hardware):
 *   - Cordless RumblePad 2            (0xC219)
 *   - Dual Action                     (0xC216)
 *   - F310 / F510 / F710 in DInput mode (0xC216 / 0xC219 / 0xC21F)
 *   - WingMan RumblePad               (0xC20E)
 *
 * No Sony/Microsoft signature in the interface descriptor — these are plain
 * HID class 0x03 SubClass 0x00 Protocol 0x00 gamepads. VID:PID match is the
 * only identification path.
 *
 * Endpoints: IN=0x81 (interrupt, 8-byte maxpkt). No handshake required —
 * reports stream immediately on opening IN. OUT endpoint exists on most
 * Logitech pads for rumble commands but Ghostcontrol does not drive rumble
 * back to the controller, so OUT is optional.
 *
 * USB input report (8 bytes, no report ID prefix):
 *   [0] LX  (0..255, center 128)
 *   [1] LY  (0..255, center 128; 0 = up, 255 = down — already PS5-orientation)
 *   [2] RX
 *   [3] RY
 *   [4] hat (low nibble, 0..7 = N/NE/E/SE/S/SW/W/NW, 8 = neutral)
 *       + face buttons (high nibble):
 *         0x10 = button 1 — leftmost  → Square
 *         0x20 = button 2 — bottom    → Cross
 *         0x40 = button 3 — right     → Circle
 *         0x80 = button 4 — top       → Triangle
 *   [5] shoulders / system / sticks:
 *         0x01 = L1   (button 5 / LB)
 *         0x02 = R1   (button 6 / RB)
 *         0x04 = L2   (button 7 — digital on Rumblepad 2)
 *         0x08 = R2   (button 8 — digital)
 *         0x10 = Back/Select   → Share
 *         0x20 = Start         → Options
 *         0x40 = L3   (left-stick click)
 *         0x80 = R3   (right-stick click)
 *   [6] mode/vibration LED state (ignored)
 *   [7] reserved / zero
 *
 * Triggers on Rumblepad 2 are digital. When pressed, analog L2/R2 is set
 * to 0xFF so games that read the analog value still see "fully pressed".
 */

#define VID_LOGITECH         0x046Du
#define PID_WINGMAN          0xC20Eu
#define PID_DUAL_ACTION      0xC216u
#define PID_RUMBLEPAD2       0xC218u
#define PID_CORDLESS_RP2     0xC219u
#define PID_F710_DINPUT      0xC21Fu

#define LOGI_EP_IN           0x81

int logitech_is_compatible_vidpid(uint16_t vid, uint16_t pid);

void logitech_parse_input(const uint8_t *buf, ScePadData *out_pad);

int  logitech_handle_packet(int fd, struct usb_fs_endpoint *eps,
                            const uint8_t *buf, uint32_t len,
                            ScePadData *out_pad);

/* Send HID SET_IDLE(0,0) + SET_PROTOCOL(report=1) class requests on the
 * selected interface to wake the device and force report-mode streaming. Many
 * generic HID gamepads (Logitech included) don't emit reports until
 * these have been issued. Safe to call unconditionally — devices that
 * already stream just ignore the requests. Returns 0 if either request was
 * accepted, otherwise -1. */
int logitech_wake_up(int fd, uint8_t interface_number);

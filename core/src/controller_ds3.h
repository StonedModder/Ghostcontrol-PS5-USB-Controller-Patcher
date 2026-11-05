#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/*
 * DualShock 3 / SIXAXIS (Sony VID=0x054C PID=0x0268) over USB.
 *
 * Unlike DS4, the DS3 does NOT stream input reports after enumeration.
 * The official Sony driver wakes it up with two HID class GET_REPORT
 * control transfers on interface 0 (a third interrupt OUT "ping" is
 * optional and triggers rumble on SHANWAN clones, so we skip it).
 *
 * Sequence (canonical, from Linux drivers/hid/hid-sony.c
 * sixaxis_set_operational_usb):
 *
 *   1.  GET_REPORT(Feature, id=0xF2, len=17)   -- returns BT MAC + paired host
 *   2.  GET_REPORT(Feature, id=0xF5, len=8)    -- returns config blob
 *
 * After step 2 the device begins emitting 49-byte interrupt IN reports
 * with report ID 0x01 at ~250 Hz on endpoint 0x81.
 *
 * Input report 0x01 layout (USB):
 *   [0]      = 0x01                    report ID
 *   [1]      = 0x00                    reserved
 *   [2]      = digital buttons A:
 *                bit0 Select, bit1 L3, bit2 R3, bit3 Start,
 *                bit4 Up,     bit5 Right, bit6 Down, bit7 Left
 *   [3]      = digital buttons B:
 *                bit0 L2,    bit1 R2,    bit2 L1,    bit3 R1,
 *                bit4 Triangle, bit5 Circle, bit6 Cross, bit7 Square
 *   [4]      = bit0 PS button (other bits reserved)
 *   [5]      = 0x00
 *   [6]      = LX  (0-255, c=128)
 *   [7]      = LY
 *   [8]      = RX
 *   [9]      = RY
 *   [10..13] = pressure-sensitive dpad (Up, Right, Down, Left) -- unused here
 *   [14..17] = pressure-sensitive L2/R2/L1/R1                  -- unused here
 *   [18]     = L2 analog (0-255)
 *   [19]     = R2 analog (0-255)
 *   [20..40] = pressure-sensitive face buttons + telemetry     -- unused here
 *   [41..46] = accel X/Z/Y (uint16 LE pairs)                   -- unused here
 */

#define PID_DS3                0x0268u

#define DS3_EP_IN              0x81
#define DS3_REPORT_F2_SIZE     17
#define DS3_REPORT_F5_SIZE     8

/* Returns 1 if (vid, pid) is a known PS3-protocol USB gamepad. Combines
 * Linux's SIXAXIS_CONTROLLER_USB quirks list with PS3-protocol third-party
 * pads catalogued by public protocol research. Unlike the DS4 module, no
 * vendor is wildcarded -- Sony 0x054C and HORI 0x0F0D both ship DS4-protocol
 * pads too, so every PID is listed explicitly. Instrument controllers
 * (Guitar Hero, Rock Band) are excluded. */
int  ds3_is_compatible_vidpid(uint16_t vid, uint16_t pid);

/* Send the two interface-recipient GET_REPORT control transfers that put the
 * DS3 into operational mode. Returns 0 on success, negative errno on failure. */
int  ds3_set_operational_usb(int fd, uint8_t interface_number);

/* Parse DS3 USB report (report ID 0x01) -> ScePadData.
 * buf MUST be at least 20 bytes. */
void ds3_parse_input(const uint8_t *buf, ScePadData *out_pad);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip. */
int  ds3_handle_packet(int fd, struct usb_fs_endpoint *eps,
                       const uint8_t *buf, uint32_t len,
                       ScePadData *out_pad);

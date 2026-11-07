#pragma once
#include <stdint.h>
#include "gc_types.h"

/*
 * Sony DualShock 3 / Sixaxis over USB (054c:0268).
 *
 * EXPERIMENTAL, off by default ("ds3=" in /data/ghostpad/config.ini).
 * On a PS5 launch model, FW 11.40, input works for ~30-40 s, then the PS5
 * shows "Device not supported" and resets the device (the DualShock 4
 * behaves similarly; non-Sony pads never have). Root cause unknown.
 *
 * USB: one HID interface (0), IN=0x81, OUT=0x02, wMaxPacketSize=64.
 * Start-up that made it stream on PS5: GET_REPORT feature 0xF2 (17 bytes,
 * as Linux hid-sony), GET_REPORT 0xF5 (8 bytes), SET_REPORT feature 0xF4
 * = 42 0c 00 00 (the PS3's "enable"), and the LED-1 output report sent
 * both via SET_REPORT and on interrupt OUT 0x02. With only 0xF2/0xF5 and
 * the SET_REPORT LED, every IN read timed out.
 *
 * Input report 0x01 (49 bytes):
 *   [2] 0x01 Select 0x02 L3 0x04 R3 0x08 Start
 *       0x10 Up 0x20 Right 0x40 Down 0x80 Left
 *   [3] 0x01 L2 0x02 R2 0x04 L1 0x08 R1
 *       0x10 Triangle 0x20 Circle 0x40 Cross 0x80 Square
 *   [4] 0x01 PS
 *   [6] LX [7] LY [8] RX [9] RY        (0..255, centre 0x80, 0 = up/left)
 *   [18] L2 pressure  [19] R2 pressure (0..255)
 *
 * PS5 mapping is one-to-one; Select → Share, Start → Options.
 */

#define VID_DS3 0x054cu
#define PID_DS3 0x0268u

#define DS3_EP_IN      0x81
#define DS3_REPORT_LEN 20     /* minimum bytes needed to parse */

/* 17-byte buffer for the GET_REPORT 0xF2 "operational" request */
#define DS3_F2_LEN 17

/* Output report 0x01 that turns on LED 1 and no rumble (36 bytes incl. ID).
 * Layout per Linux hid-sony's sixaxis_output_report. */
#define DS3_LED_REPORT_LEN 36
extern const uint8_t ds3_led1_report[DS3_LED_REPORT_LEN];

void ds3_parse_report(const uint8_t *b, ScePadData *o);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip. */
int  ds3_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad);

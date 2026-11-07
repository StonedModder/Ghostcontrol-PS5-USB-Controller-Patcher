/* controller_ds3.c — DualShock 3 (USB) → virtual DualSense
 * See controller_ds3.h for the report layout and init sequence.
 */

#include "controller_ds3.h"
#include <string.h>

const uint8_t ds3_led1_report[DS3_LED_REPORT_LEN] = {
    0x01,                               /* report ID */
    0x00, 0x00, 0x00, 0x00, 0x00,       /* rumble: off */
    0x00, 0x00, 0x00, 0x00,             /* padding */
    0x02,                               /* LED bitmap: 0x02 = LED 1 */
    0xff, 0x27, 0x10, 0x00, 0x32,       /* LED 4 timing */
    0xff, 0x27, 0x10, 0x00, 0x32,       /* LED 3 timing */
    0xff, 0x27, 0x10, 0x00, 0x32,       /* LED 2 timing */
    0xff, 0x27, 0x10, 0x00, 0x32,       /* LED 1 timing */
    0x00, 0x00, 0x00, 0x00, 0x00,
};

void ds3_parse_report(const uint8_t *b, ScePadData *o) {
    uint8_t b2 = b[2], b3 = b[3], b4 = b[4];
    uint32_t btn = 0;

    if (b2 & 0x01) btn |= SCE_PAD_BUTTON_SHARE;      /* Select */
    if (b2 & 0x02) btn |= SCE_PAD_BUTTON_L3;
    if (b2 & 0x04) btn |= SCE_PAD_BUTTON_R3;
    if (b2 & 0x08) btn |= SCE_PAD_BUTTON_OPTIONS;    /* Start */
    if (b2 & 0x10) btn |= SCE_PAD_BUTTON_UP;
    if (b2 & 0x20) btn |= SCE_PAD_BUTTON_RIGHT;
    if (b2 & 0x40) btn |= SCE_PAD_BUTTON_DOWN;
    if (b2 & 0x80) btn |= SCE_PAD_BUTTON_LEFT;

    if (b3 & 0x01) btn |= SCE_PAD_BUTTON_L2;
    if (b3 & 0x02) btn |= SCE_PAD_BUTTON_R2;
    if (b3 & 0x04) btn |= SCE_PAD_BUTTON_L1;
    if (b3 & 0x08) btn |= SCE_PAD_BUTTON_R1;
    if (b3 & 0x10) btn |= SCE_PAD_BUTTON_TRIANGLE;
    if (b3 & 0x20) btn |= SCE_PAD_BUTTON_CIRCLE;
    if (b3 & 0x40) btn |= SCE_PAD_BUTTON_CROSS;
    if (b3 & 0x80) btn |= SCE_PAD_BUTTON_SQUARE;

    if (b4 & 0x01) btn |= SCE_PAD_BUTTON_PS;

    o->buttons          = btn;
    o->leftStick.x      = b[6];
    o->leftStick.y      = b[7];
    o->rightStick.x     = b[8];
    o->rightStick.y     = b[9];
    o->analogButtons.l2 = b[18];
    o->analogButtons.r2 = b[19];
    o->connected        = 1;
    o->quat.w           = 1.0f;
}

int ds3_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad) {
    if (len < DS3_REPORT_LEN || buf[0] != 0x01) return 0;   /* input report 0x01 only */
    ds3_parse_report(buf, out_pad);
    return 1;
}

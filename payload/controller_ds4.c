/* controller_ds4.c — DualShock 4 (USB) → virtual DualSense
 * See controller_ds4.h for the report layout.
 */

#include "controller_ds4.h"
#include <string.h>

static uint32_t hat_to_dpad(uint8_t hat) {
    static const uint32_t U = SCE_PAD_BUTTON_UP,   R = SCE_PAD_BUTTON_RIGHT;
    static const uint32_t D = SCE_PAD_BUTTON_DOWN, L = SCE_PAD_BUTTON_LEFT;
    static const uint32_t tbl[8] = { U, U|R, R, D|R, D, D|L, L, U|L };
    return (hat < 8) ? tbl[hat] : 0;
}

void ds4_parse_report(const uint8_t *b, ScePadData *o) {
    uint8_t b5 = b[5], b6 = b[6], b7 = b[7];
    uint32_t btn = hat_to_dpad(b5 & 0x0F);

    if (b5 & 0x10) btn |= SCE_PAD_BUTTON_SQUARE;
    if (b5 & 0x20) btn |= SCE_PAD_BUTTON_CROSS;
    if (b5 & 0x40) btn |= SCE_PAD_BUTTON_CIRCLE;
    if (b5 & 0x80) btn |= SCE_PAD_BUTTON_TRIANGLE;

    if (b6 & 0x01) btn |= SCE_PAD_BUTTON_L1;
    if (b6 & 0x02) btn |= SCE_PAD_BUTTON_R1;
    if (b6 & 0x04) btn |= SCE_PAD_BUTTON_L2;
    if (b6 & 0x08) btn |= SCE_PAD_BUTTON_R2;
    if (b6 & 0x10) btn |= SCE_PAD_BUTTON_SHARE;
    if (b6 & 0x20) btn |= SCE_PAD_BUTTON_OPTIONS;
    if (b6 & 0x40) btn |= SCE_PAD_BUTTON_L3;
    if (b6 & 0x80) btn |= SCE_PAD_BUTTON_R3;

    if (b7 & 0x01) btn |= SCE_PAD_BUTTON_PS;
    if (b7 & 0x02) btn |= SCE_PAD_BUTTON_TOUCH_PAD;

    o->buttons          = btn;
    o->leftStick.x      = b[1];
    o->leftStick.y      = b[2];
    o->rightStick.x     = b[3];
    o->rightStick.y     = b[4];
    o->analogButtons.l2 = b[8];
    o->analogButtons.r2 = b[9];
    o->connected        = 1;
    o->quat.w           = 1.0f;
}

int ds4_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad) {
    if (len < DS4_REPORT_LEN || buf[0] != 0x01) return 0;   /* input report 0x01 only */
    ds4_parse_report(buf, out_pad);
    return 1;
}

#include "controller_mamba.h"
#ifndef MAMBA_PARSER_TEST
#include "usb_helpers.h"
#endif

#define XINPUT_REPORT_ID  0x00u
#define XINPUT_REPORT_LEN 0x14u

int mamba_is_xinput_vidpid(uint16_t vid, uint16_t pid) {
    return (vid == MAMBA_XINPUT_VID && pid == MAMBA_XINPUT_PID) ||
           (vid == GC8BITDO_2C_XINPUT_VID && pid == GC8BITDO_2C_XINPUT_PID) ||
           (vid == GC_LIEFENG2_XINPUT_VID && pid == GC_LIEFENG2_XINPUT_PID);
}

int mamba_is_xinput_interface(uint8_t subclass, uint8_t protocol) {
    return subclass == MAMBA_XINPUT_SUBCLASS && protocol == MAMBA_XINPUT_PROTOCOL;
}

const char *mamba_name(uint16_t vid, uint16_t pid) {
    if (vid == GC_LIEFENG2_XINPUT_VID && pid == GC_LIEFENG2_XINPUT_PID)
        return "Mojiang Liefeng 2 (XInput)";
    if (vid == MAMBA_DONGLE_VID && pid == MAMBA_DONGLE_PID)
        return "Manba V2 NBJr receiver idle/update mode";
    if (vid == 0x3537u && pid == 0x100fu)
        return "GameSir Nova 2 Lite (XInput)";
    if (vid == 0x3537u && pid == 0x1004u)
        return "GameSir T4 Kaleid (XInput)";
    if (vid == GC8BITDO_2C_XINPUT_VID && pid == GC8BITDO_2C_XINPUT_PID)
        return "8BitDo Ultimate 2C Wireless (XInput)";
    if (mamba_is_xinput_vidpid(vid, pid))
        return "XUSB controller (including Manba V2 NBJr XInput mode)";
    return "Unknown XUSB controller";
}

static uint8_t stick_x(int16_t value) {
    return (uint8_t)((value + 32768) >> 8);
}

static uint8_t stick_y(int16_t value) {
    int32_t scaled = (32768 - (int32_t)value) >> 8;
    return (uint8_t)(scaled > 255 ? 255 : scaled);
}

void mamba_xinput_parse_input(const uint8_t *b, ScePadData *out) {
    uint8_t b2 = b[2];
    uint8_t b3 = b[3];
    uint8_t lt = b[4];
    uint8_t rt = b[5];
    int16_t lx = (int16_t)((uint16_t)b[6]  | ((uint16_t)b[7]  << 8));
    int16_t ly = (int16_t)((uint16_t)b[8]  | ((uint16_t)b[9]  << 8));
    int16_t rx = (int16_t)((uint16_t)b[10] | ((uint16_t)b[11] << 8));
    int16_t ry = (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8));

    out->leftStick.x = stick_x(lx);
    out->leftStick.y = stick_y(ly);
    out->rightStick.x = stick_x(rx);
    out->rightStick.y = stick_y(ry);
    out->analogButtons.l2 = lt;
    out->analogButtons.r2 = rt;

    uint32_t buttons = 0;
    if (b2 & 0x01u) buttons |= SCE_PAD_BUTTON_UP;
    if (b2 & 0x02u) buttons |= SCE_PAD_BUTTON_DOWN;
    if (b2 & 0x04u) buttons |= SCE_PAD_BUTTON_LEFT;
    if (b2 & 0x08u) buttons |= SCE_PAD_BUTTON_RIGHT;
    if (b2 & 0x10u) buttons |= SCE_PAD_BUTTON_OPTIONS;
    if (b2 & 0x20u) buttons |= SCE_PAD_BUTTON_SHARE;
    if (b2 & 0x40u) buttons |= SCE_PAD_BUTTON_L3;
    if (b2 & 0x80u) buttons |= SCE_PAD_BUTTON_R3;
    if (b3 & 0x01u) buttons |= SCE_PAD_BUTTON_L1;
    if (b3 & 0x02u) buttons |= SCE_PAD_BUTTON_R1;
    if (b3 & 0x04u) buttons |= SCE_PAD_BUTTON_PS;
    if (b3 & 0x10u) buttons |= SCE_PAD_BUTTON_CROSS;
    if (b3 & 0x20u) buttons |= SCE_PAD_BUTTON_CIRCLE;
    if (b3 & 0x40u) buttons |= SCE_PAD_BUTTON_SQUARE;
    if (b3 & 0x80u) buttons |= SCE_PAD_BUTTON_TRIANGLE;
    if (lt > 16u) buttons |= SCE_PAD_BUTTON_L2;
    if (rt > 16u) buttons |= SCE_PAD_BUTTON_R2;

    out->buttons = buttons;
    out->connected = 1;
    out->quat.w = 1.0f;
}

#ifndef MAMBA_PARSER_TEST
int mamba_xinput_send_enable(int fd, struct usb_fs_endpoint *eps) {
    static const uint8_t enable[] = {0x01, 0x03, 0x0e};
    if (!eps) return -1;
    return usb_send_out(fd, &eps[1], enable, sizeof(enable), "xusb-enable");
}
#endif

int mamba_xinput_handle_packet(int fd, struct usb_fs_endpoint *eps,
                               const uint8_t *buf, uint32_t len,
                               ScePadData *out_pad) {
    (void)fd;
    (void)eps;
    if (!buf || !out_pad || len < 14 || buf[0] != XINPUT_REPORT_ID ||
        buf[1] != XINPUT_REPORT_LEN)
        return 0;
    mamba_xinput_parse_input(buf, out_pad);
    return 1;
}

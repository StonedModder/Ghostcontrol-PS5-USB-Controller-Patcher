/* controller_mamba.c - Manba V2 NBJr support for Ghost-Control */

#include "controller_mamba.h"
#include "usb_helpers.h"

#define XINPUT_REPORT_ID  0x00u
#define XINPUT_REPORT_LEN 0x14u
#define DEADZONE          7849

int mamba_is_xinput_vidpid(uint16_t vid, uint16_t pid) {
    return (vid == MAMBA_XINPUT_VID && pid == MAMBA_XINPUT_PID) ||
           (vid == GC8BITDO_2C_XINPUT_VID && pid == GC8BITDO_2C_XINPUT_PID);
}

int mamba_is_switch_vidpid(uint16_t vid, uint16_t pid) {
    return (vid == MAMBA_SWITCH_VID && pid == MAMBA_SWITCH_PID);
}

int mamba_is_supported_vidpid(uint16_t vid, uint16_t pid) {
    return mamba_is_xinput_vidpid(vid, pid) || mamba_is_switch_vidpid(vid, pid);
}

int mamba_is_xinput_interface(uint8_t subclass, uint8_t protocol) {
    return (subclass == MAMBA_XINPUT_SUBCLASS &&
            protocol == MAMBA_XINPUT_PROTOCOL);
}

const char *mamba_name(uint16_t vid, uint16_t pid) {
    if (vid == MAMBA_DONGLE_VID && pid == MAMBA_DONGLE_PID)
        return "Manba V2 NBJr receiver idle/update mode";
    if (vid == GC8BITDO_2C_XINPUT_VID && pid == GC8BITDO_2C_XINPUT_PID)
        return "8BitDo Ultimate 2C Wireless (XInput)";
    if (mamba_is_xinput_vidpid(vid, pid))
        return "Manba V2 NBJr PC/XInput mode";
    if (mamba_is_switch_vidpid(vid, pid))
        return "Manba V2 NBJr Switch USB mode";
    return "Unknown";
}

void mamba_log_switch_packet(const uint8_t *buf, uint32_t len) {
    (void)buf;
    (void)len;
}

static uint8_t stick_x(int16_t v) {
    return (v > DEADZONE || v < -DEADZONE) ? (uint8_t)((v + 32768) >> 8) : 128u;
}

static uint8_t stick_y(int16_t v) {
    return (v > DEADZONE || v < -DEADZONE) ? (uint8_t)(255 - ((v + 32768) >> 8)) : 128u;
}

void mamba_xinput_parse_input(const uint8_t *b, ScePadData *o) {
    uint8_t b2 = b[2];
    uint8_t b3 = b[3];
    uint8_t lt = b[4];
    uint8_t rt = b[5];
    int16_t lx = (int16_t)((uint16_t)b[6]  | ((uint16_t)b[7]  << 8));
    int16_t ly = (int16_t)((uint16_t)b[8]  | ((uint16_t)b[9]  << 8));
    int16_t rx = (int16_t)((uint16_t)b[10] | ((uint16_t)b[11] << 8));
    int16_t ry = (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8));

    o->leftStick.x      = stick_x(lx);
    o->leftStick.y      = stick_y(ly);
    o->rightStick.x     = stick_x(rx);
    o->rightStick.y     = stick_y(ry);
    o->analogButtons.l2 = lt;
    o->analogButtons.r2 = rt;

    uint32_t btn = 0;

    if (b2 & 0x01u) btn |= SCE_PAD_BUTTON_UP;
    if (b2 & 0x02u) btn |= SCE_PAD_BUTTON_DOWN;
    if (b2 & 0x04u) btn |= SCE_PAD_BUTTON_LEFT;
    if (b2 & 0x08u) btn |= SCE_PAD_BUTTON_RIGHT;
    if (b2 & 0x10u) btn |= SCE_PAD_BUTTON_OPTIONS;
    if (b2 & 0x20u) btn |= SCE_PAD_BUTTON_SHARE;
    if (b2 & 0x40u) btn |= SCE_PAD_BUTTON_L3;
    if (b2 & 0x80u) btn |= SCE_PAD_BUTTON_R3;

    if (b3 & 0x01u) btn |= SCE_PAD_BUTTON_L1;
    if (b3 & 0x02u) btn |= SCE_PAD_BUTTON_R1;
    if (b3 & 0x04u) btn |= SCE_PAD_BUTTON_PS;
    if (b3 & 0x10u) btn |= SCE_PAD_BUTTON_CROSS;
    if (b3 & 0x20u) btn |= SCE_PAD_BUTTON_CIRCLE;
    if (b3 & 0x40u) btn |= SCE_PAD_BUTTON_SQUARE;
    if (b3 & 0x80u) btn |= SCE_PAD_BUTTON_TRIANGLE;

    if (lt > 16u) btn |= SCE_PAD_BUTTON_L2;
    if (rt > 16u) btn |= SCE_PAD_BUTTON_R2;

    o->buttons = btn;
    o->connected = 1;
    o->quat.w = 1.0f;
}

void mamba_xinput_send_enable(int fd, struct usb_fs_endpoint *eps) {
    static const uint8_t enable[] = {0x01, 0x03, 0x0e};
    usb_send_out(fd, &eps[1], enable, sizeof(enable), "mamba-enable");
}

int mamba_xinput_handle_packet(int fd, struct usb_fs_endpoint *eps,
                               const uint8_t *buf, uint32_t len,
                               ScePadData *out_pad) {
    (void)fd;
    (void)eps;

    if (len < 14) return 0;
    if (buf[0] != XINPUT_REPORT_ID || buf[1] != XINPUT_REPORT_LEN)
        return 0;

    mamba_xinput_parse_input(buf, out_pad);
    return 1;
}

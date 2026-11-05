#include "controller_nintendo.h"

#ifndef NINTENDO_PARSER_TEST
#include "usb_helpers.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __PROSPERO__
extern void ghostpad_status_log(const char *fmt, ...);
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif
#endif

static uint8_t ntoh_stick(uint16_t v) {
    if (v > 4095) v = 4095;
    return (uint8_t)((v * 255u) / 4095u);
}

static uint8_t invert_y(uint8_t value) {
    return (uint8_t)(255u - value);
}

static uint32_t hat_to_dpad(uint8_t hat) {
    switch (hat) {
    case 0: return SCE_PAD_BUTTON_UP;
    case 1: return SCE_PAD_BUTTON_UP   | SCE_PAD_BUTTON_RIGHT;
    case 2: return SCE_PAD_BUTTON_RIGHT;
    case 3: return SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_RIGHT;
    case 4: return SCE_PAD_BUTTON_DOWN;
    case 5: return SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT;
    case 6: return SCE_PAD_BUTTON_LEFT;
    case 7: return SCE_PAD_BUTTON_UP   | SCE_PAD_BUTTON_LEFT;
    default: return 0;
    }
}

/* Report 0x30: full 60Hz stream after handshake
 * [3]=right btns [4]=shared [5]=left btns [6-8]=lstick [9-11]=rstick */
void nintendo_parse_0x30(const uint8_t *b, ScePadData *o) {
    uint8_t br=b[3], bs=b[4], bl=b[5];
    uint16_t lx=(uint16_t)(b[6]|((b[7]&0xF)<<8));
    uint16_t ly=(uint16_t)((b[7]>>4)|((uint16_t)b[8]<<4));
    uint16_t rx=(uint16_t)(b[9]|((b[10]&0xF)<<8));
    uint16_t ry=(uint16_t)((b[10]>>4)|((uint16_t)b[11]<<4));
    uint32_t btn=0;
    if(br&0x04) btn|=SCE_PAD_BUTTON_CROSS;
    if(br&0x08) btn|=SCE_PAD_BUTTON_CIRCLE;
    if(br&0x01) btn|=SCE_PAD_BUTTON_SQUARE;
    if(br&0x02) btn|=SCE_PAD_BUTTON_TRIANGLE;
    if(bl&0x40) btn|=SCE_PAD_BUTTON_L1;
    if(bl&0x80) btn|=SCE_PAD_BUTTON_L2;
    if(br&0x40) btn|=SCE_PAD_BUTTON_R1;
    if(br&0x80) btn|=SCE_PAD_BUTTON_R2;
    if(bs&0x08) btn|=SCE_PAD_BUTTON_L3;
    if(bs&0x04) btn|=SCE_PAD_BUTTON_R3;
    if(bs&0x02) btn|=SCE_PAD_BUTTON_OPTIONS;
    if(bs&0x01) btn|=SCE_PAD_BUTTON_CREATE;
    if(bs&0x10) btn|=SCE_PAD_BUTTON_PS;
    if(bs&0x20) btn|=SCE_PAD_BUTTON_TOUCH_PAD;
    if(bl&0x02) btn|=SCE_PAD_BUTTON_UP;
    if(bl&0x01) btn|=SCE_PAD_BUTTON_DOWN;
    if(bl&0x04) btn|=SCE_PAD_BUTTON_RIGHT;
    if(bl&0x08) btn|=SCE_PAD_BUTTON_LEFT;
    o->buttons=btn;
    o->leftStick.x=ntoh_stick(lx); o->leftStick.y=invert_y(ntoh_stick(ly));
    o->rightStick.x=ntoh_stick(rx); o->rightStick.y=invert_y(ntoh_stick(ry));
    o->analogButtons.l2=(bl&0x80)?255:0;
    o->analogButtons.r2=(br&0x80)?255:0;
    o->connected=1; o->quat.w=1.0f;
}

/* Report 0x3f: simple button-change report
 * [1]=right btns [2]=shared [3]=HAT [4..7]=sticks [8]=L/ZL */
void nintendo_parse_0x3f(const uint8_t *b, ScePadData *o) {
    uint8_t b1=b[1],b2=b[2],hat=b[3],b8=b[8];
    uint32_t btn=0;
    if(b1&0x04) btn|=SCE_PAD_BUTTON_CROSS;
    if(b1&0x08) btn|=SCE_PAD_BUTTON_CIRCLE;
    if(b1&0x01) btn|=SCE_PAD_BUTTON_SQUARE;
    if(b1&0x02) btn|=SCE_PAD_BUTTON_TRIANGLE;
    if(b8&0x40) btn|=SCE_PAD_BUTTON_L1;
    if(b8&0x80) btn|=SCE_PAD_BUTTON_L2;
    if(b1&0x40) btn|=SCE_PAD_BUTTON_R1;
    if(b1&0x80) btn|=SCE_PAD_BUTTON_R2;
    if(b2&0x08) btn|=SCE_PAD_BUTTON_L3;
    if(b2&0x04) btn|=SCE_PAD_BUTTON_R3;
    if(b2&0x02) btn|=SCE_PAD_BUTTON_OPTIONS;
    if(b2&0x01) btn|=SCE_PAD_BUTTON_CREATE;
    if(b2&0x10) btn|=SCE_PAD_BUTTON_PS;
    if(b2&0x20) btn|=SCE_PAD_BUTTON_TOUCH_PAD;
    btn|=hat_to_dpad(hat);
    o->buttons=btn;
    o->leftStick.x=b[4]; o->leftStick.y=invert_y(b[5]);
    o->rightStick.x=b[6]; o->rightStick.y=invert_y(b[7]);
    o->analogButtons.l2=(b8&0x80)?255:0;
    o->analogButtons.r2=(b1&0x80)?255:0;
    o->connected=1; o->quat.w=1.0f;
}

#ifndef NINTENDO_PARSER_TEST
int nintendo_send_subcmd(int fd, struct usb_fs_endpoint *eps,
                         uint8_t *seq, uint8_t subcmd,
                         const uint8_t *data, uint32_t data_len) {
    static const uint8_t rumble[8] = {0x00,0x01,0x40,0x40,0x00,0x01,0x40,0x40};
    uint8_t buf[64];
    if (!eps || !seq || (data_len && !data)) return -EINVAL;
    uint32_t len = 11 + data_len;
    if (len > sizeof(buf)) return -1;
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01; buf[1] = *seq & 0x0f;
    memcpy(buf+2, rumble, 8);
    buf[10] = subcmd;
    if (data_len) memcpy(buf+11, data, data_len);
    char tag[16]; snprintf(tag, sizeof(tag), "sc%02x", subcmd);
    int result = usb_send_out(fd, &eps[1], buf, len, tag);
    if (result == 0) *seq = (uint8_t)((*seq+1) & 0x0f);
    return result;
}

int nintendo_handle_packet(int fd, struct usb_fs_endpoint *eps,
                           const uint8_t *buf, uint32_t len,
                           int *hs_state, uint8_t *seq,
                           ScePadData *out_pad) {
    if (!eps || !buf || !hs_state || !seq || !out_pad || len == 0)
        return -1;
    uint8_t rid = buf[0];
    if (!nintendo_report_length_valid(rid, len)) return 0;

    /* Data packets */
    if ((rid == 0x00 || rid == 0x30) && len >= 12) {
        if (rid == 0x00 && buf[1] == 0) return 0; /* all-zero artifact */
        if (*hs_state != HS_STREAMING) *hs_state = HS_STREAMING;
        nintendo_parse_0x30(buf, out_pad);
        return 1;
    }
    if (rid == 0x3f && len >= 9) {
        nintendo_parse_0x3f(buf, out_pad);
        return 1;
    }
    if (rid == 0x21 && len >= 13) {
        LOG("0x21 ACK subcmd=0x%02x hs=%d\n", (buf[12]&0x7f), *hs_state);
        if (*hs_state == HS_STREAMING) {
            nintendo_parse_0x30(buf, out_pad);
            return 1;
        }
        return 0;
    }
    if (rid == 0x81 && len >= 2) {
        if (*hs_state == HS_STREAMING) {
            LOG("0x81 sub=0x%02x while streaming — reconnect\n", buf[1]);
            *hs_state = HS_WAIT_81_01;
            return 0;
        }
        if (buf[1] == 0x01 && *hs_state == HS_WAIT_81_01) {
            if (usb_send_cmd(fd, &eps[1], 0x80, 0x02) != 0) return -1;
            LOG("0x81 0x01 → [80 02]\n");
            *hs_state = HS_WAIT_81_02;
        } else if (buf[1] == 0x02 && *hs_state <= HS_WAIT_81_02) {
            if (usb_send_cmd(fd, &eps[1], 0x80, 0x04) != 0) return -1;
            LOG("0x81 0x02 → [80 04] + subcmds\n");
            uint8_t d[]={0x01};
            if (nintendo_send_subcmd(fd,eps,seq,0x40,d,1) != 0) return -1;
            if (nintendo_send_subcmd(fd,eps,seq,0x48,d,1) != 0) return -1;
            if (nintendo_send_subcmd(fd,eps,seq,0x30,d,1) != 0) return -1;
            uint8_t d2[]={0x30};
            if (nintendo_send_subcmd(fd,eps,seq,0x03,d2,1) != 0) return -1;
            *hs_state = HS_STREAMING;
        }
        return 0;
    }
    return 0;
}
#endif

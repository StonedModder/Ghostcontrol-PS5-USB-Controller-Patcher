/* controller_ds3.c -- DualShock 3 / SIXAXIS for Ghost-Control
 *
 * Protocol reference: linux/drivers/hid/hid-sony.c sixaxis_set_operational_usb.
 *
 * Unlike DS4, the DS3 stays silent on the interrupt IN endpoint until
 * the host sends two HID class GET_REPORT control transfers on EP0
 * (feature reports 0xF2 and 0xF5). After those, it streams 49-byte
 * report-ID-0x01 packets on ep 0x81.
 */

#include "controller_ds3.h"
#include "controller_ds4.h"   /* for VID_SONY, VID_HORI shared with DS4 module */
#include <dev/usb/usb_endian.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

extern void ghostpad_status_log(const char *fmt, ...);
#define DS3_LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)

/* HID class control-request constants */
#define HID_BMRT_DEV_TO_HOST_IFACE  0xA1u
#define HID_GET_REPORT              0x01u
#define HID_FEATURE_REPORT_TYPE     0x03u  /* high byte of wValue */

/* Third-party PS3-protocol PID table, compiled from public protocol
 * research and cross-checked against linux/drivers/hid/hid-sony.c
 * sony_devices and hid-ids.h. Excluded categories:
 *
 *   - Sony Rhythm VID 0x12ba (Guitar Hero / Rock Band instruments) -- these
 *     use the PS3 bus class but have instrument-specific report layouts
 *     incompatible with ds3_parse_input.
 *   - 0x1BAD Wii Rock Band instruments -- same reason.
 *   - 0x0f0d:0x0086 HORI Fighting Commander PC -- Xbox 360 protocol despite
 *     the PS3 buttons; controller_xbox handles it.
 *
 * Sony 0x054C (VID_SONY) and HORI 0x0F0D (VID_HORI) are NOT wildcarded here
 * the way the DS4 module wildcards them, because both vendors also ship
 * DS4-protocol pads. Each DS3-protocol PID is listed explicitly. */
typedef struct { uint16_t vid; uint16_t pid; } ds3_vidpid_t;

static const ds3_vidpid_t DS3_EXTRA_PAIRS[] = {
    { 0x0079, 0x181a },  /* Venom Arcade Stick */
    { 0x044f, 0xb315 },  /* Thrustmaster Firestorm Dual Analog 3 */
    { 0x044f, 0xd007 },  /* Thrustmaster wireless 3-in-1 */
    { 0x046d, 0xcad1 },  /* Logitech Chillstream */
    { 0x054c, 0x0268 },  /* Sony DualShock 3 / SIXAXIS -- canonical */
    { 0x056e, 0x200f },  /* Elecom (unlabeled) */
    { 0x056e, 0x2013 },  /* Elecom JC-U4113SBK */
    { 0x05b8, 0x1004 },  /* PS3-protocol (unlabeled) */
    { 0x05b8, 0x1006 },  /* Elecom JC-U3412SBK */
    { 0x06a3, 0xf622 },  /* Saitek Cyborg V3 */
    { 0x0738, 0x3180 },  /* Mad Catz Alpha PS3 mode */
    { 0x0738, 0x3250 },  /* Mad Catz FightPad Pro PS3 */
    { 0x0738, 0x3481 },  /* Mad Catz FightStick TE 2+ PS3 */
    { 0x0738, 0x8180 },  /* Mad Catz Alpha PS4 mode (no touchpad on device) */
    { 0x0738, 0x8838 },  /* Mad Catz FightStick Pro */
    { 0x0810, 0x0001 },  /* PS2-via-DS3-protocol adapter */
    { 0x0810, 0x0003 },  /* PS2-via-DS3-protocol adapter */
    { 0x0925, 0x0005 },  /* Sony DS3 clone (uses Lakeview VID) */
    { 0x0925, 0x8866 },  /* WiseGroup PS2 (DS3-protocol) */
    { 0x0925, 0x8888 },  /* WiseGroup MP-8866 Dual Joypad */
    { 0x0e6f, 0x0109 },  /* PDP Versus Fighting Pad */
    { 0x0e6f, 0x011e },  /* Rock Candy (PS4 mode, PS3-protocol on wire) */
    { 0x0e6f, 0x0128 },  /* Rock Candy PS3 */
    { 0x0e6f, 0x0214 },  /* PDP Afterglow PS3 */
    { 0x0e6f, 0x1314 },  /* PDP Afterglow Wireless PS3 */
    { 0x0e6f, 0x6302 },  /* PDP (unlabeled) */
    { 0x0e8f, 0x0008 },  /* GreenAsia PS3 */
    { 0x0e8f, 0x3075 },  /* SpeedLink Strike FX */
    { 0x0e8f, 0x310d },  /* GreenAsia (unlabeled) */
    { 0x0f0d, 0x0009 },  /* HORI BDA GP1 */
    { 0x0f0d, 0x004d },  /* HORI Horipad 3 */
    { 0x0f0d, 0x005f },  /* HORI Fighting Commander 4 PS3 */
    { 0x0f0d, 0x006a },  /* HORI Real Arcade Pro 4 */
    { 0x0f0d, 0x006e },  /* HORI Horipad 4 PS3 */
    { 0x0f0d, 0x0085 },  /* HORI Fighting Commander PS3 */
    { 0x0f0d, 0x0088 },  /* HORI Fighting Stick mini 4 */
    { 0x0f30, 0x1100 },  /* Qanba Q1 fight stick */
    { 0x11ff, 0x3331 },  /* SRXJ-PH2400 */
    { 0x1345, 0x1000 },  /* ACME GA-D5 PS2 */
    { 0x1345, 0x3008 },  /* Sino-Lite / Nyko Core Controller for PS3 (Linux) */
    { 0x1345, 0x6005 },  /* Sino-Lite PS2 */
    { 0x146b, 0x5500 },  /* BigBen / Nacon PS3 */
    { 0x1a34, 0x0836 },  /* Afterglow PS3 */
    { 0x20bc, 0x5500 },  /* ShanWan PS3 */
    { 0x20d6, 0x576d },  /* PowerA PS3 */
    { 0x20d6, 0xca6d },  /* BDA Pro Ex */
    { 0x2563, 0x0523 },  /* Digiflip GP006 */
    { 0x2563, 0x0575 },  /* SWITCH CO. Retro-bit Controller */
    { 0x25f0, 0x83c3 },  /* gioteck VX2 */
    { 0x25f0, 0xc121 },  /* gioteck (unlabeled) */
    { 0x2c22, 0x2003 },  /* Qanba Drone PS3 mode */
    { 0x2c22, 0x2302 },  /* Qanba Obsidian PS3 mode */
    { 0x2c22, 0x2502 },  /* Qanba Dragon PS3 mode */
    { 0x8380, 0x0003 },  /* BTP 2163 */
    { 0x8888, 0x0308 },  /* Sony DS3 clone (counterfeit VID) */
};

int ds3_is_compatible_vidpid(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < sizeof(DS3_EXTRA_PAIRS) / sizeof(DS3_EXTRA_PAIRS[0]); i++) {
        if (DS3_EXTRA_PAIRS[i].vid == vid && DS3_EXTRA_PAIRS[i].pid == pid) return 1;
    }
    return 0;
}

static int ds3_get_feature(int fd, uint8_t interface_number,
                           uint8_t report_id, uint8_t *buf, uint16_t len) {
    struct usb_ctl_request req;
    memset(&req, 0, sizeof(req));
    req.ucr_request.bmRequestType = HID_BMRT_DEV_TO_HOST_IFACE;
    req.ucr_request.bRequest      = HID_GET_REPORT;
    USETW(req.ucr_request.wValue,  (HID_FEATURE_REPORT_TYPE << 8) | report_id);
    USETW(req.ucr_request.wIndex,  interface_number);
    USETW(req.ucr_request.wLength, len);
    req.ucr_data = buf;
    return ioctl(fd, USB_DO_REQUEST, &req);
}

int ds3_set_operational_usb(int fd, uint8_t interface_number) {
    uint8_t buf[DS3_REPORT_F2_SIZE];

    memset(buf, 0, sizeof(buf));
    int r1 = ds3_get_feature(fd, interface_number, 0xF2, buf,
                             DS3_REPORT_F2_SIZE);
    DS3_LOG("ds3 GET_REPORT(0xF2,17) ret=%d errno=%d\n", r1, r1 ? errno : 0);
    if (r1 != 0) return -errno;

    memset(buf, 0, sizeof(buf));
    int r2 = ds3_get_feature(fd, interface_number, 0xF5, buf,
                             DS3_REPORT_F5_SIZE);
    DS3_LOG("ds3 GET_REPORT(0xF5,8) ret=%d errno=%d\n", r2, r2 ? errno : 0);
    if (r2 != 0) return -errno;

    return 0;
}

void ds3_parse_input(const uint8_t *b, ScePadData *o) {
    o->leftStick.x      = b[6];
    o->leftStick.y      = b[7];
    o->rightStick.x     = b[8];
    o->rightStick.y     = b[9];
    o->analogButtons.l2 = b[18];
    o->analogButtons.r2 = b[19];

    uint32_t btn = 0;

    /* b[2]: Select / L3 / R3 / Start, dpad as individual bits (NOT a hat) */
    if (b[2] & 0x01u) btn |= SCE_PAD_BUTTON_SHARE;   /* Select  -> Create/Share */
    if (b[2] & 0x02u) btn |= SCE_PAD_BUTTON_L3;
    if (b[2] & 0x04u) btn |= SCE_PAD_BUTTON_R3;
    if (b[2] & 0x08u) btn |= SCE_PAD_BUTTON_OPTIONS; /* Start   -> Options     */
    if (b[2] & 0x10u) btn |= SCE_PAD_BUTTON_UP;
    if (b[2] & 0x20u) btn |= SCE_PAD_BUTTON_RIGHT;
    if (b[2] & 0x40u) btn |= SCE_PAD_BUTTON_DOWN;
    if (b[2] & 0x80u) btn |= SCE_PAD_BUTTON_LEFT;

    /* b[3]: triggers and face buttons */
    if (b[3] & 0x01u) btn |= SCE_PAD_BUTTON_L2;
    if (b[3] & 0x02u) btn |= SCE_PAD_BUTTON_R2;
    if (b[3] & 0x04u) btn |= SCE_PAD_BUTTON_L1;
    if (b[3] & 0x08u) btn |= SCE_PAD_BUTTON_R1;
    if (b[3] & 0x10u) btn |= SCE_PAD_BUTTON_TRIANGLE;
    if (b[3] & 0x20u) btn |= SCE_PAD_BUTTON_CIRCLE;
    if (b[3] & 0x40u) btn |= SCE_PAD_BUTTON_CROSS;
    if (b[3] & 0x80u) btn |= SCE_PAD_BUTTON_SQUARE;

    /* b[4]: PS button (only bit 0 is meaningful for input) */
    if (b[4] & 0x01u) btn |= SCE_PAD_BUTTON_PS;

    o->buttons   = btn;
    o->connected = 1;
    o->quat.w    = 1.0f;
}

int ds3_handle_packet(int fd, struct usb_fs_endpoint *eps,
                      const uint8_t *buf, uint32_t len,
                      ScePadData *out_pad) {
    (void)fd; (void)eps;

    /* DS3 emits report ID 0x01 at nominally 49 bytes. We only read up to
     * byte 19 (analog triggers), so anything >= 20 with [0]==0x01 parses. */
    if (len >= 20 && buf[0] == 0x01) {
        ds3_parse_input(buf, out_pad);
        return 1;
    }
    return 0;
}

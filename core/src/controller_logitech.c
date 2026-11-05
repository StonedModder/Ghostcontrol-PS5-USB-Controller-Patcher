/* controller_logitech.c — Logitech generic HID gamepads for Ghost-Control
 *
 * Wire format documented in controller_logitech.h. No handshake, single-pass
 * IN endpoint open, stateless 8-byte report parser.
 */

#include "controller_logitech.h"
#include <dev/usb/usb_endian.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

/* Persistent file logger exported by gc_main.c. */
extern void ghostpad_status_log(const char *fmt, ...);
#define LOGI_LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)

/* HID class request constants */
#define HID_BMRT_HOST_TO_DEV_IFACE  0x21u
#define HID_SET_IDLE                0x0Au
#define HID_SET_PROTOCOL            0x0Bu
#define HID_PROTOCOL_REPORT         0x0001u
/* SET_IDLE wValue = (duration_in_4ms_units << 8) | report_id.
 * duration=0 means "only report on state change" — misses button presses
 * when our poll doesn't coincide with the change.
 * duration=0x02 = 2*4ms = 8ms = 125Hz forced periodic, matching standard USB
 * HID polling. Buttons held during a missed read get re-asserted within 8ms. */
#define HID_IDLE_PERIODIC_8MS       0x0200u

int logitech_is_compatible_vidpid(uint16_t vid, uint16_t pid) {
    return vid == VID_LOGITECH &&
           (pid == PID_WINGMAN || pid == PID_DUAL_ACTION ||
            pid == PID_RUMBLEPAD2 || pid == PID_CORDLESS_RP2 ||
            pid == PID_F710_DINPUT);
}

int logitech_wake_up(int fd, uint8_t interface_number) {
    struct usb_ctl_request req;

    /* SET_IDLE(duration=0 indefinite, report_id=0) on the selected interface.
     * Tells the device to send a report every time something changes
     * (no idle/duplicate suppression). */
    memset(&req, 0, sizeof(req));
    req.ucr_request.bmRequestType = HID_BMRT_HOST_TO_DEV_IFACE;
    req.ucr_request.bRequest      = HID_SET_IDLE;
    USETW(req.ucr_request.wValue,  HID_IDLE_PERIODIC_8MS);
    USETW(req.ucr_request.wIndex,  interface_number);
    USETW(req.ucr_request.wLength, 0x0000);
    int r1 = ioctl(fd, USB_DO_REQUEST, &req);
    LOGI_LOG("logi SET_IDLE ret=%d errno=%d\n", r1, r1 ? errno : 0);

    /* SET_PROTOCOL(Report Protocol = 1) on the selected interface.
     * Forces the device out of boot protocol into full HID report mode. */
    memset(&req, 0, sizeof(req));
    req.ucr_request.bmRequestType = HID_BMRT_HOST_TO_DEV_IFACE;
    req.ucr_request.bRequest      = HID_SET_PROTOCOL;
    USETW(req.ucr_request.wValue,  HID_PROTOCOL_REPORT);
    USETW(req.ucr_request.wIndex,  interface_number);
    USETW(req.ucr_request.wLength, 0x0000);
    int r2 = ioctl(fd, USB_DO_REQUEST, &req);
    LOGI_LOG("logi SET_PROTOCOL ret=%d errno=%d\n", r2, r2 ? errno : 0);

    /* A number of otherwise-compatible pads stall one of these optional HID
     * requests. Treat the wake-up as failed only when neither request reached
     * the selected interface successfully. */
    return (r1 == 0 || r2 == 0) ? 0 : -1;
}


/* Integer square root via Newton's iteration. Input range here is bounded
 * by 127² + 128² = 32513, converges in <8 iters. */
static int isqrt32(uint32_t n) {
    if (n < 2) return (int)n;
    uint32_t x = n, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + n / x) >> 1; }
    return (int)x;
}

/* Fernandez square-to-circle mapping. The RumblePad's stick gate is square,
 * the DualSense's is circular — and PS5 games are tuned for the latter. A
 * raw RumblePad corner (255, 0) sits well outside the unit circle (magnitude
 * ~180) and gets axis-dominance-clipped, snapping diagonals to cardinal.
 *
 * Unlike a simple radial clamp (which only touches the corners), Fernandez is
 * a smooth bijection mapping the entire square into the entire circle:
 *   x_circle = x · sqrt(1 - y² / 2)
 *   y_circle = y · sqrt(1 - x² / 2)
 *
 * Centered coords scaled to [-128, 127] → 1.0 = 128. The formula becomes:
 *   nx_out = nx * sqrt(2·128² - ny²) / (128·sqrt(2))
 * and 128·sqrt(2) ≈ 181. All-integer. */
static void logitech_circularize(uint8_t *x, uint8_t *y) {
    int nx = (int)*x - 128;
    int ny = (int)*y - 128;

    const int K2 = 2 * 128 * 128;  /* = 32768, the radicand offset */
    const int NORM = 181;          /* round(128 · sqrt(2)) */

    int sqrt_y_term = isqrt32((uint32_t)(K2 - ny * ny));  /* in [128, 181] */
    int sqrt_x_term = isqrt32((uint32_t)(K2 - nx * nx));

    int nx_out = nx * sqrt_y_term / NORM;
    int ny_out = ny * sqrt_x_term / NORM;

    int rx = 128 + nx_out, ry = 128 + ny_out;
    if (rx < 0) rx = 0; else if (rx > 255) rx = 255;
    if (ry < 0) ry = 0; else if (ry > 255) ry = 255;
    *x = (uint8_t)rx;
    *y = (uint8_t)ry;
}

/* Hat lookup: index 0..8 → dpad bits. Same encoding as DS4. */
static const uint8_t HAT_DPAD[9] = {
    /* 0 N  */ SCE_PAD_BUTTON_UP,
    /* 1 NE */ SCE_PAD_BUTTON_UP   | SCE_PAD_BUTTON_RIGHT,
    /* 2 E  */ SCE_PAD_BUTTON_RIGHT,
    /* 3 SE */ SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_RIGHT,
    /* 4 S  */ SCE_PAD_BUTTON_DOWN,
    /* 5 SW */ SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT,
    /* 6 W  */ SCE_PAD_BUTTON_LEFT,
    /* 7 NW */ SCE_PAD_BUTTON_UP   | SCE_PAD_BUTTON_LEFT,
    /* 8 -- */ 0u,
};

void logitech_parse_input(const uint8_t *b, ScePadData *o) {
    uint8_t lx = b[0], ly = b[1];
    uint8_t rx = b[2], ry = b[3];
    logitech_circularize(&lx, &ly);
    logitech_circularize(&rx, &ry);
    o->leftStick.x  = lx;
    o->leftStick.y  = ly;
    o->rightStick.x = rx;
    o->rightStick.y = ry;

    uint32_t btn = 0;

    /* b[4]: hat (low nibble) + face buttons (high nibble).
     * Logitech RumblePad 2 face button physical layout is a diamond.
     * Button labels and physical positions on the tested unit:
     *   1 = leftmost  → Square
     *   2 = bottom    → Cross
     *   3 = rightmost → Circle
     *   4 = top       → Triangle
     * Hardware-confirmed via live capture on PS5. */
    uint8_t hat = b[4] & 0x0Fu;
    if (hat <= 8) btn |= HAT_DPAD[hat];
    if (b[4] & 0x10u) btn |= SCE_PAD_BUTTON_SQUARE;   /* button 1 — left   */
    if (b[4] & 0x20u) btn |= SCE_PAD_BUTTON_CROSS;    /* button 2 — bottom */
    if (b[4] & 0x40u) btn |= SCE_PAD_BUTTON_CIRCLE;   /* button 3 — right  */
    if (b[4] & 0x80u) btn |= SCE_PAD_BUTTON_TRIANGLE; /* button 4 — top    */

    /* b[5]: shoulders + system + stick clicks */
    if (b[5] & 0x01u) btn |= SCE_PAD_BUTTON_L1;
    if (b[5] & 0x02u) btn |= SCE_PAD_BUTTON_R1;
    if (b[5] & 0x04u) {
        btn |= SCE_PAD_BUTTON_L2;
        o->analogButtons.l2 = 0xFFu;  /* digital trigger → fully-pressed analog */
    }
    if (b[5] & 0x08u) {
        btn |= SCE_PAD_BUTTON_R2;
        o->analogButtons.r2 = 0xFFu;
    }
    if (b[5] & 0x10u) btn |= SCE_PAD_BUTTON_SHARE;    /* Back/Select → Share/Create */
    if (b[5] & 0x20u) btn |= SCE_PAD_BUTTON_OPTIONS;  /* Start → Options */
    if (b[5] & 0x40u) btn |= SCE_PAD_BUTTON_L3;
    if (b[5] & 0x80u) btn |= SCE_PAD_BUTTON_R3;

    o->buttons   = btn;
    o->connected = 1;
    o->quat.w    = 1.0f;
}

int logitech_handle_packet(int fd, struct usb_fs_endpoint *eps,
                           const uint8_t *buf, uint32_t len,
                           ScePadData *out_pad) {
    (void)fd; (void)eps;

    /* Rumblepad 2 reports are 8 bytes, no report ID. Accept anything ≥ 6
     * since we only need bytes [0..5] for input. */
    if (len >= 6) {
        logitech_parse_input(buf, out_pad);
        return 1;
    }
    return 0;
}

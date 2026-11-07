/* controller_n64.c — Generic USB N64-style controller (0e6d:111d)
 * See controller_n64.h for the report layout and mapping.
 */

#include "controller_n64.h"
#include <string.h>

/* Byte 0 */
#define N64_C_LEFT  0x01u
#define N64_B       0x02u
#define N64_A       0x04u
#define N64_C_DOWN  0x08u
#define N64_L       0x10u
#define N64_R       0x20u
#define N64_Z       0x40u
/* Byte 1 */
#define N64_C_RIGHT 0x01u
#define N64_C_UP    0x02u
#define N64_START   0x10u

#define N64_DEADZONE 12

static uint8_t axis(uint8_t v) {
    int d = (int)v - 128;
    if (d < 0) d = -d;
    return (d <= N64_DEADZONE) ? 128u : v;
}

static uint32_t hat_to_dpad(uint8_t hat) {
    static const uint32_t U = SCE_PAD_BUTTON_UP,   R = SCE_PAD_BUTTON_RIGHT;
    static const uint32_t D = SCE_PAD_BUTTON_DOWN, L = SCE_PAD_BUTTON_LEFT;
    static const uint32_t tbl[8] = { U, U|R, R, D|R, D, D|L, L, U|L };
    return (hat < 8) ? tbl[hat] : 0;
}

/* Common PS5 mapping, shared by both report formats. Inputs are the logical
 * N64 buttons plus the stick already converted to PS5 0..255 (0 = up). */
typedef struct {
    int a, b, z, l, r, start, cu, cd, cl, cr;
    uint32_t dpad;
    uint8_t lx, ly;
} n64_state_t;

/* Start is shared between Options and the Start+Z / Start+B combos. Sending
 * Options while Start is held would open the PS5 options menu before the
 * combo arrives (so Start+B "back" just closed that menu). Instead, Start
 * sends nothing while held; a combo sends its button, and a lone tap sends
 * Options for a few reports after release.
 * State is per process: fine for one N64 pad at a time. */
#define N64_OPTIONS_PULSE 6     /* reports (~50-90ms) */
static int g_start_held = 0, g_start_combo = 0, g_options_pulse = 0;

static void n64_map(const n64_state_t *s, ScePadData *o) {
    uint32_t btn = s->dpad;
    int b = s->b, z = s->z;

    if (s->start) {
        if (!g_start_held) { g_start_held = 1; g_start_combo = 0; }
        if (z)      { btn |= SCE_PAD_BUTTON_PS;     z = 0; g_start_combo = 1; }  /* Start+Z → PS */
        else if (b) { btn |= SCE_PAD_BUTTON_CIRCLE; b = 0; g_start_combo = 1; }  /* Start+B → Circle */
    } else if (g_start_held) {
        if (!g_start_combo) g_options_pulse = N64_OPTIONS_PULSE;  /* lone tap released */
        g_start_held = 0;
    }
    if (g_options_pulse > 0) { btn |= SCE_PAD_BUTTON_OPTIONS; g_options_pulse--; }

    if (s->a) btn |= SCE_PAD_BUTTON_CROSS;
    if (b)    btn |= SCE_PAD_BUTTON_SQUARE;
    if (s->l) btn |= SCE_PAD_BUTTON_L1;
    if (s->r) btn |= SCE_PAD_BUTTON_R1;
    if (z)    btn |= SCE_PAD_BUTTON_R2;

    /* C-buttons → right stick, full deflection (opposites cancel) */
    int rx = 128, ry = 128;
    if (s->cl) rx -= 128;
    if (s->cr) rx += 127;
    if (s->cu) ry -= 128;
    if (s->cd) ry += 127;

    o->buttons          = btn;
    o->leftStick.x      = s->lx;
    o->leftStick.y      = s->ly;
    o->rightStick.x     = (uint8_t)rx;
    o->rightStick.y     = (uint8_t)ry;
    o->analogButtons.l2 = 0;
    o->analogButtons.r2 = (btn & SCE_PAD_BUTTON_R2) ? 255 : 0;
    o->connected        = 1;
    o->quat.w           = 1.0f;
}

void n64_parse_report(const uint8_t *b, ScePadData *o) {
    n64_state_t s;
    s.a  = (b[0] & N64_A) != 0;       s.b  = (b[0] & N64_B) != 0;
    s.z  = (b[0] & N64_Z) != 0;       s.l  = (b[0] & N64_L) != 0;
    s.r  = (b[0] & N64_R) != 0;       s.start = (b[1] & N64_START) != 0;
    s.cu = (b[1] & N64_C_UP) != 0;    s.cd = (b[0] & N64_C_DOWN) != 0;
    s.cl = (b[0] & N64_C_LEFT) != 0;  s.cr = (b[1] & N64_C_RIGHT) != 0;
    s.dpad = hat_to_dpad(b[2] & 0x0F);
    s.lx = axis(b[3]);
    s.ly = axis(b[4]);
    n64_map(&s, o);
}

/* 12-bit Pro stick (0..4095, centre 0x800) → 0..255; invert for Y (up = high) */
static uint8_t pro_axis(uint16_t v, int invert) {
    if (v > 4095) v = 4095;
    uint8_t x = (uint8_t)((v * 255u) / 4095u);
    if (invert) x = (uint8_t)(255 - x);
    return axis(x);
}

void n64pro_parse_0x30(const uint8_t *b, ScePadData *o) {
    uint8_t br = b[3], bs = b[4], bl = b[5];
    uint16_t lx = (uint16_t)(b[6] | ((b[7] & 0x0F) << 8));
    uint16_t ly = (uint16_t)((b[7] >> 4) | ((uint16_t)b[8] << 4));
    n64_state_t s;
    s.a  = (br & 0x08) != 0;   s.b  = (br & 0x04) != 0;
    s.r  = (br & 0x40) != 0;   s.cd = (br & 0x02) != 0;   s.cl = (br & 0x01) != 0;
    s.start = (bs & 0x10) != 0; s.cu = (bs & 0x02) != 0;  s.cr = (bs & 0x01) != 0;
    s.z  = (bl & 0x80) != 0;   s.l  = (bl & 0x40) != 0;
    s.dpad = ((bl & 0x02) ? SCE_PAD_BUTTON_UP    : 0) | ((bl & 0x01) ? SCE_PAD_BUTTON_DOWN : 0) |
             ((bl & 0x04) ? SCE_PAD_BUTTON_RIGHT : 0) | ((bl & 0x08) ? SCE_PAD_BUTTON_LEFT : 0);
    s.lx = pro_axis(lx, 0);
    s.ly = pro_axis(ly, 1);
    n64_map(&s, o);
}

int n64_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad) {
    if (len < N64_REPORT_LEN) return 0;
    /* Idle has the hat at 0xF, so an all-zero prefix is not a real report */
    if (!(buf[0] | buf[1] | buf[2] | buf[3] | buf[4])) return 0;
    n64_parse_report(buf, out_pad);
    return 1;
}

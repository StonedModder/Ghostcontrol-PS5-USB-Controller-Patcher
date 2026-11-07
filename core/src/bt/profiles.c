/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "profiles.h"
#include "crc32.h"
#include "util.h"

#include <string.h>

/* Trigger travel past which L2/R2 also count as pressed buttons. */
#define TRIGGER_PRESS 0x20

/* How many vid:pid pairs a profile's id table holds. */
#define NIDS(t) ((int)(sizeof t / sizeof t[0]))

/* ---- shared helpers ------------------------------------------------------ */

/* A hat switch: 0 north, clockwise to 7 north-west; anything else is centred. */
static uint32_t hat_buttons(unsigned hat)
{
    static const uint32_t dirs[8] = {
        PAD_UP, PAD_UP | PAD_RIGHT, PAD_RIGHT, PAD_DOWN | PAD_RIGHT,
        PAD_DOWN, PAD_DOWN | PAD_LEFT, PAD_LEFT, PAD_UP | PAD_LEFT,
    };
    return hat < 8 ? dirs[hat] : 0;
}

static uint32_t trigger_buttons(uint8_t l2, uint8_t r2)
{
    return (l2 > TRIGGER_PRESS ? PAD_L2 : 0) | (r2 > TRIGGER_PRESS ? PAD_R2 : 0);
}

/* The three button bytes DualShock 4 and DualSense share:
 *   b0: hat (low nibble), square, cross, circle, triangle
 *   b1: L1, R1, L2, R2, share/create, options, L3, R3
 *   b2: PS, touchpad click, (DualSense) mute */
static uint32_t sony_buttons(const uint8_t *b)
{
    static const uint32_t b0[4] = { PAD_SQUARE, PAD_CROSS, PAD_CIRCLE, PAD_TRIANGLE };
    static const uint32_t b1[8] = { PAD_L1, PAD_R1, PAD_L2, PAD_R2,
                                    PAD_CREATE, PAD_OPTIONS, PAD_L3, PAD_R3 };
    uint32_t out = hat_buttons(b[0] & 0x0F);
    int i;

    for (i = 0; i < 4; i++) if (b[0] & (0x10 << i)) out |= b0[i];
    for (i = 0; i < 8; i++) if (b[1] & (1 << i)) out |= b1[i];
    if (b[2] & 0x01) out |= PAD_PS;
    if (b[2] & 0x02) out |= PAD_TOUCHPAD;
    return out;
}

/* A touch point: contact byte (bit 7 set = no finger), then 12-bit x and y
 * packed in three bytes. */
static void sony_touch(const uint8_t *p, pad_state *st, int i)
{
    st->touch[i].down = !(p[0] & 0x80);
    st->touch[i].x = (uint16_t)(p[1] | (p[2] & 0x0F) << 8);
    st->touch[i].y = (uint16_t)(p[2] >> 4 | p[3] << 4);
}

static void read_motion(const uint8_t *gyro, const uint8_t *accel, pad_state *st)
{
    int i;

    for (i = 0; i < 3; i++) {
        st->gyro[i] = les16(gyro + 2 * i);
        st->accel[i] = les16(accel + 2 * i);
    }
    st->has_motion = 1;
}

static void sony_basic(const uint8_t *sticks, const uint8_t *buttons,
                       uint8_t l2, uint8_t r2, pad_state *st)
{
    st->lx = sticks[0];
    st->ly = sticks[1];
    st->rx = sticks[2];
    st->ry = sticks[3];
    st->l2 = l2;
    st->r2 = r2;
    st->buttons = sony_buttons(buttons) | trigger_buttons(l2, r2);
}

/* A 12-bit stick axis: the low byte, then the low nibble of the next one. */
static unsigned axis12(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] & 0x0F) << 8;
}

/* ---- DualShock 4 ----------------------------------------------------------
 *
 * Over Bluetooth it starts with the short report 0x01 and moves to the full
 * report 0x11 once it receives an output report 0x11.
 *
 *   0x01: [1..4] sticks  [5..7] buttons  [8] L2  [9] R2
 *   0x11: [1..2] flags   [3..6] sticks   [7..9] buttons  [10] L2  [11] R2
 *         [15..20] gyro  [21..26] accel  [32] battery (low nibble level,
 *         bit 4 cable)   [35] touch report count, [36] first touch report:
 *         timestamp, then two 4-byte points
 */

static const uint16_t ds4_ids[][2] = {
    { 0x054C, 0x05C4 },     /* DualShock 4 v1 */
    { 0x054C, 0x09CC },     /* DualShock 4 v2 */
    { 0x0F0D, 0x00F6 },     /* Hori Onyx */
    { 0x1532, 0x1009 },     /* Razer Raiju Ultimate */
    { 0x1532, 0x100A },     /* Razer Raiju Tournament */
    { 0x2E95, 0x7725 },     /* SCUF Vantage 2 */
    { 0x0F0D, 0x0123 },     /* Hori Wireless Controller Light */
};

static int ds4_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    (void)ctx;
    if (r[0] == 0x01 && n >= 10) {
        sony_basic(r + 1, r + 5, r[8], r[9], st);
        return PARSE_OK;
    }
    if (r[0] != 0x11 || n < 36) return PARSE_NONE;

    sony_basic(r + 3, r + 7, r[10], r[11], st);
    read_motion(r + 15, r + 21, st);

    {
        unsigned level = r[32] & 0x0F, cable = (r[32] >> 4) & 1;
        unsigned full = cable ? 11 : 8;     /* cable reports up to 11 */
        if (level > full) level = full;
        st->battery_pct = (uint8_t)(level * 100 / full);
        st->charging = (uint8_t)(cable && level < full);
        st->has_battery = 1;
    }
    if (n >= 45 && r[35] > 0) {
        sony_touch(r + 37, st, 0);
        sony_touch(r + 41, st, 1);
        st->has_touch = 1;
    }
    return PARSE_FULL;
}

/* Output report 0x11: 73 bytes after the id, then the CRC.
 *   [1] 0xC0 | report interval  [2] 0x20  [3] 0xF3 (rumble, light bar)
 *   [4] 0x04  [6] weak motor  [7] strong motor  [8..10] light bar RGB */
static int ds4_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    const int len = 1 + 73 + 4;

    (void)ctx;
    if (max < len) return 0;
    memset(b, 0, (size_t)len);
    b[0] = 0x11;
    b[1] = 0xC0 | 0x08;                 /* 0x08: 125 Hz input reports */
    b[2] = 0x20;
    b[3] = 0xF3;
    b[4] = 0x04;
    b[6] = o->weak;
    b[7] = o->strong;
    b[8] = o->r;
    b[9] = o->g;
    b[10] = o->b;
    crc32_seal_output(b, len);
    return len;
}

/* Waking a Sony pad up is just sending it an output report. */
static int ds4_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    return step == 0 ? ds4_output(ctx, o, b, max) : 0;
}

/* ---- DualSense --------------------------------------------------------------
 *
 * Same idea: short report 0x01 until an output report 0x31 arrives.
 *
 *   0x01: [1..4] sticks  [5..8] buttons  [9] L2  [10] R2
 *   0x31: [2..5] sticks  [6] L2  [7] R2  [9..12] buttons
 *         [17..22] gyro  [23..28] accel  [34..41] two touch points
 *         [54] battery: low nibble level 0..10, high nibble 1 charging,
 *         2 full
 */

static const uint16_t ds5_ids[][2] = {
    { 0x054C, 0x0CE6 },     /* DualSense */
    { 0x054C, 0x0DF2 },     /* DualSense Edge */
};

static int ds5_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    (void)ctx;
    if (r[0] == 0x01 && n >= 11) {
        sony_basic(r + 1, r + 5, r[9], r[10], st);
        return PARSE_OK;
    }
    if (r[0] != 0x31 || n < 55) return PARSE_NONE;

    sony_basic(r + 2, r + 9, r[6], r[7], st);
    read_motion(r + 17, r + 23, st);
    sony_touch(r + 34, st, 0);
    sony_touch(r + 38, st, 1);
    st->has_touch = 1;

    {
        unsigned level = r[54] & 0x0F, status = r[54] >> 4;
        unsigned pct = level * 10 + 5;
        st->battery_pct = (uint8_t)(pct > 100 ? 100 : pct);
        st->charging = (uint8_t)(status == 1);
        st->has_battery = 1;
    }
    return PARSE_FULL;
}

/* Output report 0x31: 73 bytes after the id, then the CRC.
 *   [1] 0x02 (tag)  [2] valid flags 0: rumble  [3] valid flags 1: lights
 *   [4] weak motor  [5] strong motor  [40] valid flags 2
 *   [43] light bar setup  [44] LED brightness  [45] player LEDs
 *   [46..48] light bar RGB */
static int ds5_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    static const uint8_t player_leds[5] = { 0x00, 0x04, 0x0A, 0x15, 0x1B };
    const int len = 1 + 73 + 4;

    (void)ctx;
    if (max < len) return 0;
    memset(b, 0, (size_t)len);
    b[0] = 0x31;
    b[1] = 0x02;
    b[2] = 0x03;
    b[3] = 0x54;
    b[4] = o->weak;
    b[5] = o->strong;
    b[40] = 0x03;
    b[43] = 0x02;
    b[44] = 0x02;
    b[45] = 0x20 | player_leds[o->player < 5 ? o->player : 4];  /* 0x20: no fade-in */
    b[46] = o->r;
    b[47] = o->g;
    b[48] = o->b;
    crc32_seal_output(b, len);
    return len;
}

static int ds5_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    return step == 0 ? ds5_output(ctx, o, b, max) : 0;
}

/* ---- Xbox One / Series ------------------------------------------------------
 *
 * The same reports over Bluetooth Classic (older firmware) and Bluetooth LE
 * (HID over GATT, current firmware), as documented publicly:
 *
 *   0x01: [1..8] sticks, 16-bit each  [9..12] triggers, 10-bit each
 *         [13] hat, 1 north .. 8 north-west, 0 centred
 *         16 bytes (first Xbox One S firmware):
 *           [14] A B X Y LB RB View Menu  [15] L3 R3
 *         17 bytes and more:
 *           [14] A B - X Y - LB RB  [15] - - View Menu Xbox L3 R3
 *           [16] bit 0: Share on Series controllers, View on older ones
 *   0x02: [1] bit 0 Xbox button (first firmware)
 *   0x04: [1] battery: bits 0-1 level, bits 2-3 power mode, bit 4 charging
 *
 * View is the "select" of Xbox games: it becomes the touchpad click, which
 * plays that part in PlayStation games. Share becomes Create.
 */

static const uint16_t xbox_ids[][2] = {
    { 0x045E, 0x02E0 },     /* Xbox One S (Bluetooth Classic) */
    { 0x045E, 0x02FD },     /* Xbox One S (Bluetooth Classic) */
    { 0x045E, 0x0B00 },     /* Elite Series 2 (Bluetooth Classic) */
    { 0x045E, 0x0B05 },     /* Elite Series 2 (Bluetooth Classic) */
    { 0x045E, 0x0B0A },     /* Adaptive Controller (Bluetooth Classic) */
    { 0x045E, 0x0B13 },     /* Xbox Series X|S (Bluetooth LE) */
    { 0x045E, 0x0B20 },     /* Xbox One S, current firmware (Bluetooth LE) */
    { 0x045E, 0x0B21 },     /* Adaptive Controller (Bluetooth LE) */
    { 0x045E, 0x0B22 },     /* Elite Series 2 (Bluetooth LE) */
};

typedef struct {
    int has_share;
} xbox_ctx;

static void xbox_setup(void *ctx, uint16_t vid, uint16_t pid)
{
    xbox_ctx *c = ctx;

    c->has_share = vid == 0x045E && (pid == 0x0B12 || pid == 0x0B13);
}

static int xbox_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    const xbox_ctx *c = ctx;
    uint32_t b = 0;
    unsigned hat;

    if (r[0] == 0x02 && n >= 2) {
        st->buttons = (st->buttons & ~(uint32_t)PAD_PS) | (r[1] & 1 ? PAD_PS : 0);
        return PARSE_OK;
    }
    if (r[0] == 0x04 && n >= 2) {
        st->battery_pct = (uint8_t)((r[1] & 3) * 100 / 3);
        st->charging = (uint8_t)((r[1] >> 4) & 1);
        st->has_battery = 1;
        return PARSE_OK;
    }
    if (r[0] != 0x01 || n < 16) return PARSE_NONE;

    st->lx = (uint8_t)(le16(r + 1) >> 8);
    st->ly = (uint8_t)(le16(r + 3) >> 8);
    st->rx = (uint8_t)(le16(r + 5) >> 8);
    st->ry = (uint8_t)(le16(r + 7) >> 8);
    st->l2 = (uint8_t)((le16(r + 9) & 0x3FF) >> 2);
    st->r2 = (uint8_t)((le16(r + 11) & 0x3FF) >> 2);

    hat = r[13];
    b |= hat >= 1 && hat <= 8 ? hat_buttons(hat - 1) : 0;

    if (n > 16) {
        if (r[14] & 0x01) b |= PAD_CROSS;
        if (r[14] & 0x02) b |= PAD_CIRCLE;
        if (r[14] & 0x08) b |= PAD_SQUARE;
        if (r[14] & 0x10) b |= PAD_TRIANGLE;
        if (r[14] & 0x40) b |= PAD_L1;
        if (r[14] & 0x80) b |= PAD_R1;
        if (r[15] & 0x04) b |= PAD_TOUCHPAD;    /* View */
        if (r[15] & 0x08) b |= PAD_OPTIONS;     /* Menu */
        if (r[15] & 0x10) b |= PAD_PS;          /* Xbox */
        if (r[15] & 0x20) b |= PAD_L3;
        if (r[15] & 0x40) b |= PAD_R3;
        if (r[16] & 0x01) b |= c && c->has_share ? PAD_CREATE : PAD_TOUCHPAD;
    } else {
        if (r[14] & 0x01) b |= PAD_CROSS;
        if (r[14] & 0x02) b |= PAD_CIRCLE;
        if (r[14] & 0x04) b |= PAD_SQUARE;
        if (r[14] & 0x08) b |= PAD_TRIANGLE;
        if (r[14] & 0x10) b |= PAD_L1;
        if (r[14] & 0x20) b |= PAD_R1;
        if (r[14] & 0x40) b |= PAD_TOUCHPAD;    /* View */
        if (r[14] & 0x80) b |= PAD_OPTIONS;     /* Menu */
        if (r[15] & 0x01) b |= PAD_L3;
        if (r[15] & 0x02) b |= PAD_R3;
        b |= st->buttons & PAD_PS;              /* comes in report 0x02 */
    }
    st->buttons = b | trigger_buttons(st->l2, st->r2);
    return PARSE_OK;
}

/* Output report 0x03, as sent over Bluetooth: all four motors
 * enabled, trigger motors, main motors (0..100), on for 2.55 s, no delay,
 * repeated 235 times: in effect until the next report. */
static int xbox_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    (void)ctx;
    if (max < 9) return 0;
    b[0] = 0x03;
    b[1] = 0x0F;
    b[2] = 0;
    b[3] = 0;
    b[4] = (uint8_t)(o->strong * 100 / 255);
    b[5] = (uint8_t)(o->weak * 100 / 255);
    b[6] = 0xFF;
    b[7] = 0x00;
    b[8] = 0xEB;
    return 9;
}

/* ---- Nintendo: the shared Switch report -------------------------------------
 *
 * Starts in a simple HID mode (report 0x3F) and moves to the standard full
 * report 0x30 when asked with subcommands in output report 0x01:
 *
 *   0x01: [1] packet counter (low nibble)  [2..9] rumble data
 *         [10] subcommand id  [11..] subcommand arguments
 *
 *   0x30: [1] timer  [2] battery (high nibble) and connection
 *         [3] right buttons: Y X B A SR SL R ZR
 *         [4] shared: - + RS LS Home Capture
 *         [5] left buttons: down up right left SR SL L ZL
 *         [6..8] left stick, [9..11] right stick: two 12-bit values each
 *         [13..48] three IMU samples: accel xyz, gyro xyz, 16-bit each
 *
 *   0x3F: [1] B A Y X L R ZL ZR  [2] - + LS RS Home Capture  [3] hat
 *         [4..11] sticks, 16-bit each
 *
 * Buttons are placed by position, as on the PlayStation: B (bottom) is
 * cross, A (right) circle, Y (left) square, X (top) triangle.
 */

static const uint16_t switch_ids[][2] = {
    { 0x057E, 0x2009 },     /* Switch Pro Controller, and pads emulating it */
    { 0x057E, 0x2017 },     /* SNES Online */
    { 0x057E, 0x2019 },     /* N64 Online */
    { 0x057E, 0x201A },     /* Mega Drive Online */
    { 0x057E, 0x201E },     
};

typedef struct {
    uint8_t counter;
} switch_ctx;

/* ---- bit tables -------------------------------------------------------------
 *
 * Every Nintendo report is a few bytes of independent button bits; the tables
 * below are the layouts in the comments, nothing more. One table is one
 * contiguous run of report bytes, addressed from the first byte handed to the
 * map, so the same reader covers a single button byte, the Pro's group of
 * three and a Joy-Con's two.
 */
typedef struct {
    uint8_t byte;                   /* offset from the first byte of the run */
    uint8_t bit;
    uint32_t button;
} bit_button;

#define BITS(t) ((int)(sizeof t / sizeof t[0]))

/* A set bit is a press. */
static uint32_t bits_set(const uint8_t *p, const bit_button *map, int n)
{
    uint32_t b = 0;
    int i;

    for (i = 0; i < n; i++) if (p[map[i].byte] & map[i].bit) b |= map[i].button;
    return b;
}

/* A clear bit is a press: the Wii U Pro's extension buttons are low-active. */
static uint32_t bits_clear(const uint8_t *p, const bit_button *map, int n)
{
    uint32_t b = 0;
    int i;

    for (i = 0; i < n; i++) if (!(p[map[i].byte] & map[i].bit)) b |= map[i].button;
    return b;
}

static const bit_button switch_simple_bits[] = {         /* 0x3F, from [1] */
    { 0, 0x01, PAD_CROSS },         /* B */
    { 0, 0x02, PAD_CIRCLE },        /* A */
    { 0, 0x04, PAD_SQUARE },        /* Y */
    { 0, 0x08, PAD_TRIANGLE },      /* X */
    { 0, 0x10, PAD_L1 },
    { 0, 0x20, PAD_R1 },
    { 0, 0x40, PAD_L2 },
    { 0, 0x80, PAD_R2 },
    { 1, 0x01, PAD_CREATE },        /* - */
    { 1, 0x02, PAD_OPTIONS },       /* + */
    { 1, 0x04, PAD_L3 },
    { 1, 0x08, PAD_R3 },
    { 1, 0x10, PAD_PS },            /* Home */
    { 1, 0x20, PAD_TOUCHPAD },      /* Capture */
};

static const bit_button switch_full_bits[] = {           /* 0x30, from [3] */
    { 0, 0x01, PAD_SQUARE },        /* Y */
    { 0, 0x02, PAD_TRIANGLE },      /* X */
    { 0, 0x04, PAD_CROSS },         /* B */
    { 0, 0x08, PAD_CIRCLE },        /* A */
    { 0, 0x40, PAD_R1 },
    { 0, 0x80, PAD_R2 },
    { 1, 0x01, PAD_CREATE },        /* - */
    { 1, 0x02, PAD_OPTIONS },       /* + */
    { 1, 0x04, PAD_R3 },
    { 1, 0x08, PAD_L3 },
    { 1, 0x10, PAD_PS },            /* Home */
    { 1, 0x20, PAD_TOUCHPAD },      /* Capture */
    { 2, 0x01, PAD_DOWN },
    { 2, 0x02, PAD_UP },
    { 2, 0x04, PAD_RIGHT },
    { 2, 0x08, PAD_LEFT },
    { 2, 0x40, PAD_L1 },
    { 2, 0x80, PAD_L2 },
};

/* Stick travel from centre the raw values usually reach; factory
 * calibration (SPI flash) is not read yet. */
#define SWITCH_STICK_RANGE 1600

static uint8_t switch_axis(int v, int invert)
{
    int s = (v - 2048) * 127 / SWITCH_STICK_RANGE;

    if (invert) s = -s;
    s += 128;
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

static void switch_stick(const uint8_t *p, uint8_t *x, uint8_t *y)
{
    *x = switch_axis((int)axis12(p), 0);
    *y = switch_axis((int)(p[1] >> 4 | p[2] << 4), 1);     /* up is positive */
}

/* The full report's battery nibble, and the digital ZL/ZR as analogue 0/255. */
static void switch_battery(const uint8_t *r, pad_state *st)
{
    unsigned level = r[2] >> 5;                         /* 0 empty .. 4 full */

    st->battery_pct = (uint8_t)(level * 25);
    st->charging = (uint8_t)((r[2] >> 4) & 1);
    st->has_battery = 1;
}

static int switch_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    uint32_t b;

    (void)ctx;
    if (r[0] == 0x3F && n >= 12) {
        b = bits_set(r + 1, switch_simple_bits, BITS(switch_simple_bits)) |
            hat_buttons(r[3]);
        st->lx = (uint8_t)(le16(r + 4) >> 8);
        st->ly = (uint8_t)(le16(r + 6) >> 8);
        st->rx = (uint8_t)(le16(r + 8) >> 8);
        st->ry = (uint8_t)(le16(r + 10) >> 8);
        st->l2 = b & PAD_L2 ? 255 : 0;
        st->r2 = b & PAD_R2 ? 255 : 0;
        st->buttons = b;
        return PARSE_OK;
    }
    if (r[0] != 0x30 || n < 12) return PARSE_NONE;

    b = bits_set(r + 3, switch_full_bits, BITS(switch_full_bits));
    switch_stick(r + 6, &st->lx, &st->ly);
    switch_stick(r + 9, &st->rx, &st->ry);
    st->l2 = b & PAD_L2 ? 255 : 0;                  /* ZL/ZR are digital */
    st->r2 = b & PAD_R2 ? 255 : 0;
    st->buttons = b;
    switch_battery(r, st);

    if (n >= 25) {
        /* First IMU sample: accel then gyro, 16-bit each. */
        int i;
        for (i = 0; i < 3; i++) {
            st->accel[i] = les16(r + 13 + 2 * i);
            st->gyro[i] = les16(r + 19 + 2 * i);
        }
        st->has_motion = 1;
    }
    return PARSE_FULL;
}

/* An output report 0x01 carrying one subcommand, rumble neutral. */
static int switch_subcmd(switch_ctx *c, uint8_t id, const uint8_t *arg, int alen,
                         uint8_t *b, int max)
{
    static const uint8_t neutral[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };

    if (max < 11 + alen) return 0;
    b[0] = 0x01;
    b[1] = c->counter++ & 0x0F;
    memcpy(b + 2, neutral, 8);
    b[10] = id;
    memcpy(b + 11, arg, (size_t)alen);
    return 11 + alen;
}

static int switch_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    static const uint8_t full_mode[1] = { 0x30 };
    static const uint8_t on[1] = { 0x01 };
    uint8_t leds[1];
    switch_ctx *c = ctx;

    switch (step) {
    case 0: return switch_subcmd(c, 0x03, full_mode, 1, b, max);   /* report mode */
    case 1: return switch_subcmd(c, 0x40, on, 1, b, max);          /* IMU on */
    case 2: return switch_subcmd(c, 0x48, on, 1, b, max);          /* vibration on */
    case 3:
        leds[0] = (uint8_t)((1u << ((o->player ? o->player : 1) - 1)) & 0x0F);
        return switch_subcmd(c, 0x30, leds, 1, b, max);            /* player LEDs */
    default:
        return 0;
    }
}


static const uint16_t joycon_ids[][2] = {
    { 0x057E, 0x2006 },     /* Joy-Con (L), and the NES Online (L) */
    { 0x057E, 0x2007 },     /* Joy-Con (R), and the NES Online (R) */
};

/* Left: the shared byte [4] then the left byte [5]. */
static const bit_button joycon_left_bits[] = {
    { 0, 0x01, PAD_CREATE },        /* - */
    { 0, 0x20, PAD_OPTIONS },       /* Capture: the only other one */
    { 0, 0x08, PAD_L3 },            /* stick press */
    { 1, 0x08, PAD_CROSS },         /* left arrow: bottom */
    { 1, 0x01, PAD_CIRCLE },        /* down: right */
    { 1, 0x02, PAD_SQUARE },        /* up: left */
    { 1, 0x04, PAD_TRIANGLE },      /* right: top */
    { 1, 0x20, PAD_L1 },            /* SL */
    { 1, 0x10, PAD_R1 },            /* SR */
    { 1, 0x40, PAD_L2 },            /* L */
    { 1, 0x80, PAD_R2 },            /* ZL */
};

/* Right: the right byte [3] then the shared byte [4]. */
static const bit_button joycon_right_bits[] = {
    { 0, 0x08, PAD_CROSS },         /* A: bottom */
    { 0, 0x02, PAD_CIRCLE },        /* X: right */
    { 0, 0x04, PAD_SQUARE },        /* B: left */
    { 0, 0x01, PAD_TRIANGLE },      /* Y: top */
    { 0, 0x20, PAD_L1 },            /* SL */
    { 0, 0x10, PAD_R1 },            /* SR */
    { 0, 0x40, PAD_L2 },            /* R */
    { 0, 0x80, PAD_R2 },            /* ZR */
    { 1, 0x02, PAD_OPTIONS },       /* + */
    { 1, 0x10, PAD_PS },            /* Home */
    { 1, 0x04, PAD_L3 },            /* stick press */
};

typedef struct {
    switch_ctx sw;                  /* first: switch_init uses it */
    int right;
} joycon_ctx;

static void joycon_setup(void *ctx, uint16_t vid, uint16_t pid)
{
    joycon_ctx *c = ctx;

    (void)vid;
    c->right = pid == 0x2007;
}

/* The quarter turn. The stick is read into its own side first (x to the
 * right, y up), then the axes swap for the sideways hold:
 *
 *   left  (anticlockwise): right on the pad is up  -> lx = y, ly = 255 - x
 *   right (clockwise):     up on the pad is right -> lx = 255 - y, ly = x
 *
 * The button tables above follow the same turn, so the button under the
 * player's thumb is always cross (bottom), circle (right), square (left) and
 * triangle (top) on the virtual pad.
 */
static void joycon_sideways(int right, uint8_t x, uint8_t y_up,
                            uint8_t *lx, uint8_t *ly)
{
    if (!right) {
        *lx = y_up;
        *ly = (uint8_t)(255 - x);
    } else {
        *lx = (uint8_t)(255 - y_up);
        *ly = x;
    }
}

static int joycon_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    const joycon_ctx *c = ctx;
    uint8_t x = 128, y_up = 128;
    uint32_t b;

    if (r[0] != 0x30 || n < 12) return PARSE_NONE;

    if (!c->right) {
        switch_stick(r + 6, &x, &y_up);
        b = bits_set(r + 4, joycon_left_bits, BITS(joycon_left_bits));
    } else {
        switch_stick(r + 9, &x, &y_up);
        b = bits_set(r + 3, joycon_right_bits, BITS(joycon_right_bits));
    }

    joycon_sideways(c->right, x, y_up, &st->lx, &st->ly);
    st->rx = st->ry = 128;                  /* one stick: the right one rests */
    st->l2 = b & PAD_L2 ? 255 : 0;
    st->r2 = b & PAD_R2 ? 255 : 0;
    st->buttons = b;
    switch_battery(r, st);
    return PARSE_FULL;
}

static int joycon_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    return switch_init(&((joycon_ctx *)ctx)->sw, step, o, b, max);
}


static const uint16_t wii_ids[][2] = {
    { 0x057E, 0x0306 },     /* Wii Remote */
    { 0x057E, 0x0330 },     /* Wii U Pro Controller (and Wii Remote Plus) */
};

typedef struct {
    int moded;                      /* a full report came: a status report now drops the mode */
    int mode_lost;                  /* a status report came: set the mode again on the next output */
} wii_ctx;

/* The Pro's extension bytes [8..10]; low-active, so the table's bits are the
 * "released" ones. */
static const bit_button wii_pro_bits[] = {
    { 0, 0x80, PAD_RIGHT },
    { 0, 0x40, PAD_DOWN },
    { 1, 0x02, PAD_LEFT },
    { 1, 0x01, PAD_UP },
    { 1, 0x40, PAD_CROSS },         /* B: bottom */
    { 1, 0x10, PAD_CIRCLE },        /* A: right */
    { 1, 0x20, PAD_SQUARE },        /* Y: left */
    { 1, 0x08, PAD_TRIANGLE },      /* X: top */
    { 0, 0x20, PAD_L1 },
    { 0, 0x02, PAD_R1 },
    { 1, 0x80, PAD_L2 },            /* ZL */
    { 1, 0x04, PAD_R2 },            /* ZR */
    { 0, 0x10, PAD_CREATE },        /* - */
    { 0, 0x04, PAD_OPTIONS },       /* + */
    { 0, 0x08, PAD_PS },            /* Home */
    { 2, 0x02, PAD_L3 },
    { 2, 0x01, PAD_R3 },
};

/* The core buttons [1..2] of any Wii Remote, held sideways as an NES pad;
 * active-high, and the d-pad turns with the remote. */
static const bit_button wii_core_bits[] = {
    { 0, 0x08, PAD_LEFT },          /* up, held sideways */
    { 0, 0x04, PAD_RIGHT },         /* down */
    { 0, 0x01, PAD_DOWN },          /* left */
    { 0, 0x02, PAD_UP },            /* right */
    { 1, 0x01, PAD_CROSS },         /* 2 */
    { 1, 0x02, PAD_SQUARE },        /* 1 */
    { 1, 0x08, PAD_CIRCLE },        /* A */
    { 1, 0x04, PAD_R1 },            /* B, underneath */
    { 1, 0x10, PAD_CREATE },        /* - */
    { 0, 0x10, PAD_OPTIONS },       /* + */
    { 1, 0x80, PAD_PS },            /* Home */
};

/* 12-bit axis, centre 0x800, +-1280 of travel; y is inverted for the pad's
 * "up is positive" reading. */
static uint8_t wii_axis(const uint8_t *d, int invert)
{
    int v = (int)axis12(d) - 0x800;
    int s = v * 127 / 1280;

    if (invert) s = -s;
    s += 128;
    return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
}

/* A Wii U Pro's extension bytes: the bits that are always 1 are, and the
 * sticks are near their range (a Wii Remote alone sends no such thing). */
static int wii_is_pro(const uint8_t *e)
{
    int i;

    if (!(e[8] & 0x01) || !(e[10] & 0x80)) return 0;
    for (i = 0; i < 8; i += 2) {
        int v = (int)axis12(e + i);
        if (e[i + 1] & 0xF0 || v < 0x200 || v > 0xE00) return 0;
    }
    return 1;
}

/* The core buttons only, with the sticks resting: what a Wii Remote sends. */
static void wii_core_sideways(const uint8_t *c, pad_state *st)
{
    st->lx = st->ly = st->rx = st->ry = 128;
    st->l2 = st->r2 = 0;
    st->buttons = bits_set(c, wii_core_bits, BITS(wii_core_bits));
}

static int wii_parse(void *ctx, const uint8_t *r, int n, pad_state *st)
{
    wii_ctx *c = ctx;
    const uint8_t *e;
    uint32_t b;

    /* Status. Once the full report has been asked for, this means the pad
     * dropped that mode and will send nothing more until it is set again;
     * remember it and answer on the next output report. */
    if (r[0] == 0x20) {
        if (c->moded) c->mode_lost = 1;
        return PARSE_NONE;
    }
    if (r[0] == 0x30 && n >= 3) {
        wii_core_sideways(r + 1, st);
        return PARSE_OK;
    }
    if (r[0] != 0x34 || n < 22) return PARSE_NONE;
    c->moded = 1;
    e = r + 3;
    if (!wii_is_pro(e)) {
        wii_core_sideways(r + 1, st);
        return PARSE_FULL;
    }

    st->lx = wii_axis(e + 0, 0);
    st->rx = wii_axis(e + 2, 0);
    st->ly = wii_axis(e + 4, 1);                /* up is positive */
    st->ry = wii_axis(e + 6, 1);
    b = bits_clear(e + 8, wii_pro_bits, BITS(wii_pro_bits));
    st->l2 = b & PAD_L2 ? 255 : 0;
    st->r2 = b & PAD_R2 ? 255 : 0;
    st->buttons = b;
    {
        unsigned level = (e[10] >> 4) & 0x07;   /* 0 .. 4 */
        st->battery_pct = (uint8_t)(level >= 4 ? 100 : level * 25);
        st->charging = (uint8_t)!(e[10] & 0x04);
        st->has_battery = 1;
    }
    return PARSE_FULL;
}

static int wii_write(uint8_t *b, int max, uint32_t addr, uint8_t value)
{
    if (max < 22) return 0;
    memset(b, 0, 22);
    b[0] = 0x16;
    b[1] = 0x04;                                /* registers, not EEPROM */
    b[2] = (uint8_t)(addr >> 16);
    b[3] = (uint8_t)(addr >> 8);
    b[4] = (uint8_t)addr;
    b[5] = 1;
    b[6] = value;
    return 22;
}

static int wii_init(void *ctx, int step, const pad_output *o, uint8_t *b, int max)
{
    int player = o->player >= 1 && o->player <= 4 ? o->player : 1;

    (void)ctx;
    if (max < 3) return 0;
    switch (step) {
    case 0:
        b[0] = 0x11;
        b[1] = (uint8_t)(0x10 << (player - 1));    /* player LED */
        return 2;
    case 1: return wii_write(b, max, 0xA400F0, 0x55);  /* wake the extension */
    case 2: return wii_write(b, max, 0xA400FB, 0x00);
    case 3:
        b[0] = 0x12;
        b[1] = 0x04;                                /* continuous */
        b[2] = 0x34;                                /* core buttons and 19 extension bytes */
        return 3;
    default:
        return 0;
    }
}

/* Rumble and player LEDs (report 0x11: bit 0 rumble, bits 4-7 the LEDs), or
 * the report mode again when a status report took it away. */
static int wii_output(void *ctx, const pad_output *o, uint8_t *b, int max)
{
    wii_ctx *c = ctx;
    const int player = o->player >= 1 && o->player <= 4 ? o->player : 1;

    if (c->mode_lost) {
        if (max < 3) return 0;
        c->mode_lost = 0;
        c->moded = 1;
        b[0] = 0x12;
        b[1] = 0x04;
        b[2] = 0x34;
        return 3;
    }
    if (max < 2) return 0;
    b[0] = 0x11;
    b[1] = (uint8_t)(((o->strong || o->weak) ? 0x01 : 0x00) | (0x10 << (player - 1)));
    return 2;
}

/* ---- the table ---------------------------------------------------------
 *
 * Designated initialisers, so a new field in pad_profile cannot silently
 * shift every row. One row per family; the id table next to its parser.
 */

static const pad_profile k_profiles[] = {
    { .name = "DualShock 4", .ids = ds4_ids, .nids = NIDS(ds4_ids),
      .parse = ds4_parse, .build_output = ds4_output, .init_report = ds4_init,
      .needs_full = 1 },

    { .name = "DualSense", .ids = ds5_ids, .nids = NIDS(ds5_ids),
      .parse = ds5_parse, .build_output = ds5_output, .init_report = ds5_init,
      .needs_full = 1 },

    { .name = "Xbox", .ids = xbox_ids, .nids = NIDS(xbox_ids),
      .parse = xbox_parse, .build_output = xbox_output,
      .needs_full = 0, .setup = xbox_setup },

    { .name = "Switch Pro", .ids = switch_ids, .nids = NIDS(switch_ids),
      .parse = switch_parse, .init_report = switch_init, .needs_full = 1 },

    { .name = "Joy-Con", .ids = joycon_ids, .nids = NIDS(joycon_ids),
      .parse = joycon_parse, .init_report = joycon_init, .needs_full = 1,
      .setup = joycon_setup },

    { .name = "Wii", .ids = wii_ids, .nids = NIDS(wii_ids),
      .parse = wii_parse, .build_output = wii_output, .init_report = wii_init,
      .needs_full = 1 },
};

#define N_PROFILES ((int)(sizeof k_profiles / sizeof k_profiles[0]))

/* Does this profile claim vid:pid? */
static int profile_has_id(const pad_profile *p, uint16_t vid, uint16_t pid)
{
    int i;

    for (i = 0; i < p->nids; i++)
        if (p->ids[i][0] == vid && p->ids[i][1] == pid) return 1;
    return 0;
}

/* NULL for a pad with no profile: generic.c reads the pad's own HID
 * descriptor for it, which is what host.c and le.c fall back to. */
const pad_profile *profile_find(uint16_t vid, uint16_t pid)
{
    int p;

    for (p = 0; p < N_PROFILES; p++)
        if (profile_has_id(&k_profiles[p], vid, pid)) return &k_profiles[p];
    return NULL;
}

int profile_class_is_gamepad(uint32_t cod)
{
    unsigned major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x0F;

    return major == 5 && (minor == 1 || minor == 2);
}

/* controller_gcpad.c — Generic USB GameCube-style pad (DragonRise 0079:0006)
 * See controller_gcpad.h for the report layout.
 */

#include "controller_gcpad.h"
#include <string.h>

/* ── Mapping table ────────────────────────────────────────────────────────
 * Index = HID button number - 1 (buttons 1..12). Value = PS5 button bit(s).
 *
 * GameCube → PS5 is positional: A (big centre) → Cross, B → Square,
 * X → Circle, Y → Triangle, Z → R1, L → L2, R → R2, Start → Options.
 *
 * Confirmed with tools/gcpad_capture on a 0079:0006 GameCube-style pad
 * (2026-11-06). Buttons 8, 9, 11, 12 do not exist on that pad; they are
 * mapped anyway for variants that have extra buttons.
 */
static const uint32_t GCPAD_BTN_MAP[12] = {
    /*  1 */ SCE_PAD_BUTTON_TRIANGLE,  /* Y      (confirmed) */
    /*  2 */ SCE_PAD_BUTTON_CIRCLE,    /* X      (confirmed) */
    /*  3 */ SCE_PAD_BUTTON_CROSS,     /* A      (confirmed) */
    /*  4 */ SCE_PAD_BUTTON_SQUARE,    /* B      (confirmed) */
    /*  5 */ SCE_PAD_BUTTON_L2,        /* L      (confirmed) */
    /*  6 */ SCE_PAD_BUTTON_R2,        /* R      (confirmed) */
    /*  7 */ SCE_PAD_BUTTON_R1,        /* Z      (confirmed) */
    /*  8 */ SCE_PAD_BUTTON_L1,        /* not present */
    /*  9 */ SCE_PAD_BUTTON_SHARE,     /* not present */
    /* 10 */ SCE_PAD_BUTTON_OPTIONS,   /* Start  (confirmed) */
    /* 11 */ SCE_PAD_BUTTON_L3,        /* not present */
    /* 12 */ SCE_PAD_BUTTON_R3,        /* not present */
};

/* C-stick axis bytes (Rx, Rz on most DragonRise firmwares). */
#define GCPAD_CSTICK_X_BYTE 3
#define GCPAD_CSTICK_Y_BYTE 4

/* PS button: Start + Z held together (GC pads have no Home button). */
#define GCPAD_PS_COMBO (SCE_PAD_BUTTON_OPTIONS | SCE_PAD_BUTTON_R1)

/* Cheap pads idle a few counts off centre — snap small offsets to 128. */
#define GCPAD_DEADZONE 12

static uint8_t axis(uint8_t v) {
    int d = (int)v - 128;
    if (d < 0) d = -d;
    return (d <= GCPAD_DEADZONE) ? 128u : v;
}

/* The C-stick only travels ~±64 from centre (captured 0x40..0xBF), so
 * scale it 2x to give the PS5 right stick full range. */
#define GCPAD_CSTICK_GAIN 2
static uint8_t cstick(uint8_t v) {
    int d = ((int)axis(v) - 128) * GCPAD_CSTICK_GAIN;
    if (d < -128) d = -128;
    if (d >  127) d =  127;
    return (uint8_t)(128 + d);
}

/* Hat switch: 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW, anything else = centred */
static uint32_t hat_to_dpad(uint8_t hat) {
    static const uint32_t U = SCE_PAD_BUTTON_UP,   R = SCE_PAD_BUTTON_RIGHT;
    static const uint32_t D = SCE_PAD_BUTTON_DOWN, L = SCE_PAD_BUTTON_LEFT;
    static const uint32_t tbl[8] = { U, U|R, R, D|R, D, D|L, L, U|L };
    return (hat < 8) ? tbl[hat] : 0;
}

void gcpad_parse_report(const uint8_t *b, ScePadData *o) {
    uint32_t btn = hat_to_dpad(b[5] & 0x0F);

    /* 12 buttons: bits 4-7 of b[5] are buttons 1-4, b[6] is buttons 5-12 */
    uint16_t raw = (uint16_t)((b[5] >> 4) | ((uint16_t)b[6] << 4));
    for (int i = 0; i < 12; i++)
        if (raw & (1u << i)) btn |= GCPAD_BTN_MAP[i];

    if ((btn & GCPAD_PS_COMBO) == GCPAD_PS_COMBO)
        btn = (btn & ~GCPAD_PS_COMBO) | SCE_PAD_BUTTON_PS;

    /* Digital L/R → full-scale analog so games that read l2/r2 values work */
    o->analogButtons.l2 = (btn & SCE_PAD_BUTTON_L2) ? 255 : 0;
    o->analogButtons.r2 = (btn & SCE_PAD_BUTTON_R2) ? 255 : 0;

    o->buttons      = btn;
    o->leftStick.x  = axis(b[0]);
    o->leftStick.y  = axis(b[1]);
    o->rightStick.x = cstick(b[GCPAD_CSTICK_X_BYTE]);
    o->rightStick.y = cstick(b[GCPAD_CSTICK_Y_BYTE]);
    o->connected    = 1;
    o->quat.w       = 1.0f;
}

int gcpad_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad) {
    if (len < GCPAD_REPORT_LEN) return 0;
    /* All-zero = not a real report (idle is 80 80 00 80 80 0f ..).
     * Parsing it would inject D-pad UP and both sticks pinned up-left. */
    { uint8_t any = 0; for (uint32_t i = 0; i < 8 && i < len; i++) any |= buf[i];
      if (!any) return 0; }
    gcpad_parse_report(buf, out_pad);
    return 1;
}

/* controller_ds4.c — DualShock 4 / DS4-compatible (XIM4 etc.) for Ghost-Control
 *
 * DS4 USB wire format is the public Sony report (ID 0x01, 64 bytes).
 * No init or handshake required: open the IN endpoint, reports stream at ~250 Hz.
 */

#include "controller_ds4.h"
#include <string.h>
#include <stddef.h>

/* Third-party PS4-protocol PID table, snapshotted from libsdl-org/SDL
 * src/joystick/controller_list.h (k_eControllerType_PS4Controller entries).
 *
 * Entries marked k_eControllerType_XInputPS4Controller in SDL are excluded —
 * those devices present as Xbox 360 protocol over USB, not PS4, and are
 * handled by controller_xbox.c via the GIP interface descriptor.
 *
 * Sony (0x054C) and HORI (0x0F0D) are NOT listed here — they match
 * unconditionally in ds4_is_compatible_vidpid() since those vendors only
 * ship PS-protocol USB controllers. */
typedef struct { uint16_t vid; uint16_t pid; } ds4_vidpid_t;

static const ds4_vidpid_t DS4_EXTRA_PAIRS[] = {
    { 0x0079, 0x181B },  /* Venom Arcade Stick */
    { 0x044F, 0xD00E },  /* Thrustmaster Eswap Pro */
    { 0x0738, 0x8250 },  /* Mad Catz FightPad Pro PS4 */
    { 0x0738, 0x8384 },  /* Mad Catz FightStick TE S+ PS4 */
    { 0x0738, 0x8480 },  /* Mad Catz FightStick TE 2 PS4 */
    { 0x0738, 0x8481 },  /* Mad Catz FightStick TE 2+ PS4 */
    { 0x0C12, 0x0E10 },  /* Armor Armor 3 Pad PS4 */
    { 0x0C12, 0x0E13 },  /* ZEROPLUS P4 Wired Gamepad */
    { 0x0C12, 0x0E15 },  /* Game:Pad 4 */
    { 0x0C12, 0x0E20 },  /* Brook Mars Controller */
    { 0x0C12, 0x0EF6 },  /* Hitbox Arcade Stick */
    { 0x0C12, 0x1CF6 },  /* EMIO PS4 Elite Controller */
    { 0x0C12, 0x1E10 },  /* P4 Wired Gamepad generic clone */
    { 0x0C12, 0x2E18 },  /* ZEROPLUS P4 Wired Gamepad alt */
    { 0x0E6F, 0x0203 },  /* Victrix Pro FS */
    { 0x0E6F, 0x0207 },  /* Victrix Pro FS V2 */
    { 0x0E6F, 0x020A },  /* Victrix Pro FS PS4/PS5 */
    { 0x11C0, 0x4001 },  /* "PS4 Fun Controller" */
    { 0x146B, 0x0D01 },  /* Nacon Revolution Pro Controller */
    { 0x146B, 0x0D02 },  /* Nacon Revolution Pro Controller v2 */
    { 0x146B, 0x0D06 },  /* NACON Asymmetric Wireless Dongle */
    { 0x146B, 0x0D08 },  /* NACON Revolution Unlimited Wireless Dongle */
    { 0x146B, 0x0D09 },  /* NACON Daija Fight Stick */
    { 0x146B, 0x0D10 },  /* NACON Revolution Infinite / Unlimited */
    { 0x146B, 0x0D13 },  /* NACON Revolution Pro Controller 3 */
    { 0x146B, 0x1103 },  /* NACON Asymmetric Controller */
    { 0x1532, 0x0401 },  /* Razer Panthera */
    { 0x1532, 0x1000 },  /* Razer Raiju */
    { 0x1532, 0x1004 },  /* Razer Raiju 2 Ultimate USB */
    { 0x1532, 0x1007 },  /* Razer Raiju 2 Tournament USB */
    { 0x1532, 0x1008 },  /* Razer Panthera Evo */
    { 0x1532, 0x1009 },  /* Razer Raiju 2 Ultimate BT */
    { 0x1532, 0x100A },  /* Razer Raiju 2 Tournament BT */
    { 0x1532, 0x1100 },  /* Razer RAION Fightpad */
    { 0x20D6, 0x792A },  /* PowerA Fusion Fight Pad */
    { 0x2C22, 0x2000 },  /* Qanba Drone */
    { 0x2C22, 0x2300 },  /* Qanba Obsidian */
    { 0x2C22, 0x2500 },  /* Qanba Dragon */
    { 0x3285, 0x0D16 },  /* NACON Revolution 5 Pro (PS4 mode, dongle) */
    { 0x3285, 0x0D17 },  /* NACON Revolution 5 Pro (PS4 mode, wired) */
    { 0x7545, 0x0104 },  /* Armor 3 / Level Up Cobra */
    { 0x9886, 0x0025 },  /* Astro C40 */
};

int ds4_is_compatible_vidpid(uint16_t vid, uint16_t pid) {
    if (vid == VID_SONY && (pid == PID_DS4_V1 || pid == PID_DS4_V2)) return 1;
    if (vid == VID_HORI &&
        (pid == PID_HORIPAD_FPSPLUS || pid == PID_HORIPAD4 ||
         pid == PID_HORIPAD_FPSPLUS_X || pid == PID_HORIPAD_MINI4)) return 1;
    for (size_t i = 0; i < sizeof(DS4_EXTRA_PAIRS) / sizeof(DS4_EXTRA_PAIRS[0]); i++) {
        if (DS4_EXTRA_PAIRS[i].vid == vid && DS4_EXTRA_PAIRS[i].pid == pid) return 1;
    }
    return 0;
}

/* Hat lookup: index 0..8 → (up, right, down, left) bits */
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

void ds4_parse_input(const uint8_t *b, ScePadData *o) {
    o->leftStick.x      = b[1];
    o->leftStick.y      = b[2];
    o->rightStick.x     = b[3];
    o->rightStick.y     = b[4];
    o->analogButtons.l2 = b[8];
    o->analogButtons.r2 = b[9];

    uint32_t btn = 0;

    /* b[5]: dpad (low nibble, hat 0..8) + face buttons (high nibble) */
    uint8_t hat = b[5] & 0x0Fu;
    if (hat <= 8) btn |= HAT_DPAD[hat];
    if (b[5] & 0x10u) btn |= SCE_PAD_BUTTON_SQUARE;
    if (b[5] & 0x20u) btn |= SCE_PAD_BUTTON_CROSS;
    if (b[5] & 0x40u) btn |= SCE_PAD_BUTTON_CIRCLE;
    if (b[5] & 0x80u) btn |= SCE_PAD_BUTTON_TRIANGLE;

    /* b[6]: shoulders + select/start + stick clicks */
    if (b[6] & 0x01u) btn |= SCE_PAD_BUTTON_L1;
    if (b[6] & 0x02u) btn |= SCE_PAD_BUTTON_R1;
    if (b[6] & 0x04u) btn |= SCE_PAD_BUTTON_L2;
    if (b[6] & 0x08u) btn |= SCE_PAD_BUTTON_R2;
    if (b[6] & 0x10u) btn |= SCE_PAD_BUTTON_SHARE;    /* Share → Create */
    if (b[6] & 0x20u) btn |= SCE_PAD_BUTTON_OPTIONS;
    if (b[6] & 0x40u) btn |= SCE_PAD_BUTTON_L3;
    if (b[6] & 0x80u) btn |= SCE_PAD_BUTTON_R3;

    /* b[7]: PS + touchpad-click (low 2 bits) */
    if (b[7] & 0x01u) btn |= SCE_PAD_BUTTON_PS;
    if (b[7] & 0x02u) btn |= SCE_PAD_BUTTON_TOUCH_PAD;

    o->buttons   = btn;
    o->connected = 1;
    o->quat.w    = 1.0f;
}

int ds4_handle_packet(int fd, struct usb_fs_endpoint *eps,
                      const uint8_t *buf, uint32_t len,
                      ScePadData *out_pad) {
    (void)fd; (void)eps;

    /* DS4 USB uses report ID 0x01. Real DS4 sends 64 bytes; HORI third-party
     * pads (and XIM4) often truncate to ~27 bytes — bytes [1..9] are identical
     * so anything ≥ 10 bytes with [0]==0x01 is parseable. */
    if (len >= 10 && buf[0] == 0x01) {
        ds4_parse_input(buf, out_pad);
        return 1;
    }
    return 0;
}

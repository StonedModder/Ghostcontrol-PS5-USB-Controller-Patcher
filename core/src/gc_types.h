#pragma once
#include <stdint.h>

/* SCE pad button constants */
#define SCE_PAD_BUTTON_SHARE     0x00000001u  /* DualSense Create / DS4 Share */
#define SCE_PAD_BUTTON_L3        0x00000002u
#define SCE_PAD_BUTTON_R3        0x00000004u
#define SCE_PAD_BUTTON_OPTIONS   0x00000008u
#define SCE_PAD_BUTTON_UP        0x00000010u
#define SCE_PAD_BUTTON_RIGHT     0x00000020u
#define SCE_PAD_BUTTON_DOWN      0x00000040u
#define SCE_PAD_BUTTON_LEFT      0x00000080u
#define SCE_PAD_BUTTON_L2        0x00000100u
#define SCE_PAD_BUTTON_R2        0x00000200u
#define SCE_PAD_BUTTON_L1        0x00000400u
#define SCE_PAD_BUTTON_R1        0x00000800u
#define SCE_PAD_BUTTON_TRIANGLE  0x00001000u
#define SCE_PAD_BUTTON_CIRCLE    0x00002000u
#define SCE_PAD_BUTTON_CROSS     0x00004000u
#define SCE_PAD_BUTTON_SQUARE    0x00008000u
#define SCE_PAD_BUTTON_PS        0x00010000u
#define SCE_PAD_BUTTON_CREATE    SCE_PAD_BUTTON_SHARE
#define SCE_PAD_BUTTON_TOUCH_PAD 0x00100000u

typedef struct { uint16_t x; uint16_t y; uint8_t finger; uint8_t pad[3]; } ScePadTouch;
typedef struct {
    uint8_t fingers; uint8_t pad1[3]; uint32_t pad2; ScePadTouch touch[2];
} ScePadTouchData;
typedef struct {
    uint32_t buttons;
    struct { uint8_t x; uint8_t y; } leftStick;
    struct { uint8_t x; uint8_t y; } rightStick;
    struct { uint8_t l2; uint8_t r2; } analogButtons;
    uint16_t padding;
    struct { float x, y, z, w; } quat;
    struct { float x, y, z; } vel;
    struct { float x, y, z; } accel;
    ScePadTouchData touchData;
    uint8_t connected;
    uint8_t _align[3];
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t count;
    uint8_t unknown[15];
} ScePadData;

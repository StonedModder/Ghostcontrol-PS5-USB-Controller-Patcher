# Adding Other USB HID Controllers

This guide provides the baseline for adding support for controllers other than the 8BitDo in Nintendo Switch Pro mode. Everything below is derived from what was confirmed working on PS5.

---

## Architecture Overview

The payload has two separate paths:

```
USB HID Controller  →  gc_main USB thread  →  ScePadData  →  scePadVirtualDeviceInsertData
                                               (parse)              (VDI inject)
```

To add a new controller you only need to:
1. Add VID:PID detection in `ugen_find_target()`
2. Add an initialization sequence in `usb_hid_thread()`
3. Add a parser function that converts the controller's HID report to `ScePadData`

---

## Step 1: Identify the Controller

Plug the controller into the PS5 USB port and watch klog:

```
ugen2.2: <ManufacturerName, ProductName>(VID=0xXXXX PID=0xXXXX) at usbus2
```

Also check:
- Which `ugen` path it appears on (`/dev/ugen2.2`, `/dev/ugen1.2`, etc.)
- What kernel driver claims it (`usb_hid0`, `usb_hid1`, etc.)

Add the VID:PID to `gc_main.c`:

```c
#define VID_MYCTLR  0xXXXXu
#define PID_MYCTLR  0xXXXXu
```

And add detection to `ugen_find_target()`:

```c
if ((di.udi_vendorNo == VID_MYCTLR && di.udi_productNo == PID_MYCTLR)) {
    if (out_vid) *out_vid = di.udi_vendorNo;
    if (out_pid) *out_pid = di.udi_productNo;
    if (out_path) *out_path = UGEN_PATHS[i];
    ok = 1;
}
```

---

## Step 2: Find the Endpoints

After `USB_IFACE_DRIVER_DETACH` and `USB_FS_INIT`, try opening common HID endpoints:

```c
// Standard HID: IN=0x81, OUT=0x01 (or 0x02 for some — check your device)
USB_FS_OPEN(ep_no=0x81, ep_index=0, max_bufsize=64)   // IN
USB_FS_OPEN(ep_no=0x01, ep_index=1, max_bufsize=64)   // OUT (try 0x02 if this fails)
```

Log `max_packet_length` from the open result — it tells you the expected HID report size.

**Common endpoint configurations:**

| Controller Family | IN | OUT | Report Size |
|------------------|----|-----|------------|
| Nintendo Pro / 8BitDo Nintendo mode | 0x81 | 0x02 | 64 bytes |
| Xbox 360 (wired) | 0x81 | 0x02 | 20 bytes (IN), 8 bytes (OUT) |
| Xbox One / Series (wired) | 0x81 | 0x02 | 18 bytes (IN) |
| Generic HID gamepad | 0x81 | 0x01 | varies |

---

## Step 3: Determine Init Sequence

### Native HID Controllers (no init needed)

Most generic HID gamepads (XInput-incompatible mode, standard HID descriptor) start sending reports immediately after you open the IN endpoint — no OUT commands needed.

```c
// No init: just open IN and start reading
USB_FS_OPEN(ep_no=0x81, ep_index=0);
// → start reading, data comes immediately
```

Check if `USB_FS_COMPLETE` returns data within 500ms with no OUT sent. If yes: native HID, skip all init.

### Xbox 360 (Wired)

Xbox 360 wired controller uses XInput protocol. It requires an init packet on the OUT endpoint before it sends input reports:

```c
// No handshake needed — just send one OUT "enable" packet
uint8_t enable[] = {0x01, 0x03, 0x0E};  // XInput enable command (3 bytes)
send_out(fd, &eps[1], enable, sizeof(enable));
// Controller then sends 0x00 input reports at ~125Hz
```

Xbox 360 has no USB mode switching — no handshake like Nintendo. See report format below.

### Xbox One / Series (Wired)

Xbox One protocol (XInput HID variant) requires:

```c
// Enable input reports
uint8_t enable[] = {0x05, 0x20, 0x00, 0x01, 0x00};
send_out(fd, &eps[1], enable, sizeof(enable));
// Optionally: rumble keepalive every 5s (or controller disconnects from idle)
```

### Generic / Unknown

1. Open IN endpoint (no OUT init)
2. Read for 2 seconds
3. If no data: try sending `[0x00]` or `[0x01]` on OUT endpoint
4. Capture and log whatever comes in — inspect the HID report manually

---

## Step 4: Parse the HID Report

### Identify Report Structure

Log the raw bytes while pressing each button individually:

```c
gp_log("PKT: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
       buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
       buf[8],buf[9],buf[10],buf[11]);
```

Press each button, record which bit changes. Common patterns:

```
Buttons: one byte per group, each button = 1 bit
Sticks:  one byte per axis (0–255 range, center=127–128)
         OR two bytes per axis (little-endian 16-bit, center=~32767)
Triggers:one byte per trigger (0–255)
         OR in button byte (binary on/off)
Dpad:    one nibble (0-8 hat switch) OR four bits in a button byte
```

### Xbox 360 Report Format (20 bytes, report ID = 0x00)

```
Byte  0: 0x00       — report ID
Byte  1: 0x14       — length (20)
Byte  2: buttons lo — A B X Y LB RB Back Start (bits 4-7: 0000)
Byte  3: buttons hi — LS RS Guide (bits 0-2)
Byte  4: left trigger  (0-255)
Byte  5: right trigger (0-255)
Byte 6-7:  left stick X  (int16 LE, center=0, range -32768..32767)
Byte 8-9:  left stick Y  (int16 LE, center=0, inverted: up=positive)
Byte 10-11: right stick X
Byte 12-13: right stick Y
Byte 14-19: (padding)
```

Dpad is in `buf[2]` bits 0-3:
```
bit 0: Up, bit 1: Down, bit 2: Left, bit 3: Right
```

### Xbox One Report Format (18 bytes, report ID = 0x20)

```
Byte  0: 0x20       — report ID
Byte  1: 0x00       — sequence
Byte  2: buttons lo — A B X Y LB RB View Menu (bits 0-7)
Byte  3: buttons hi — LS RS (bits 0-1), dpad (bits 2-5)
Byte  4: left trigger  (0-255, maps to 0-1023 internally)
Byte  5: right trigger (0-255)
Byte 6-7:  left stick X  (int16 LE, center=0)
Byte 8-9:  left stick Y  (int16 LE)
Byte 10-11: right stick X
Byte 12-13: right stick Y
Byte 14-17: (varies by firmware)
```

---

## Step 5: Map to ScePadData

Add a parse function in `gc_main.c` following this pattern:

```c
static void parse_xbox360(const uint8_t *b, ScePadData *o) {
    uint8_t bl = b[2], bh = b[3];
    uint8_t lt = b[4], rt = b[5];
    int16_t lx = (int16_t)(b[6] | (b[7] << 8));
    int16_t ly = (int16_t)(b[8] | (b[9] << 8));
    int16_t rx = (int16_t)(b[10] | (b[11] << 8));
    int16_t ry = (int16_t)(b[12] | (b[13] << 8));

    uint32_t btn = 0;
    /* Face buttons */
    if (bl & 0x10) btn |= SCE_PAD_BUTTON_CROSS;     // A
    if (bl & 0x20) btn |= SCE_PAD_BUTTON_CIRCLE;    // B
    if (bl & 0x40) btn |= SCE_PAD_BUTTON_SQUARE;    // X
    if (bl & 0x80) btn |= SCE_PAD_BUTTON_TRIANGLE;  // Y
    /* Shoulder */
    if (bh & 0x01) btn |= SCE_PAD_BUTTON_L1;        // LB
    if (bh & 0x02) btn |= SCE_PAD_BUTTON_R1;        // RB
    /* Triggers (analog) */
    o->analogButtons.l2 = lt;
    o->analogButtons.r2 = rt;
    if (lt > 128) btn |= SCE_PAD_BUTTON_L2;
    if (rt > 128) btn |= SCE_PAD_BUTTON_R2;
    /* Sticks */
    if (bh & 0x20) btn |= SCE_PAD_BUTTON_L3;
    if (bh & 0x40) btn |= SCE_PAD_BUTTON_R3;
    /* Menu */
    if (bl & 0x04) btn |= SCE_PAD_BUTTON_CREATE;    // Back
    if (bl & 0x08) btn |= SCE_PAD_BUTTON_OPTIONS;   // Start
    /* Dpad */
    if (bl & 0x01) btn |= SCE_PAD_BUTTON_UP;
    if (bl & 0x02) btn |= SCE_PAD_BUTTON_DOWN;
    if (bl & 0x04) btn |= SCE_PAD_BUTTON_LEFT;
    if (bl & 0x08) btn |= SCE_PAD_BUTTON_RIGHT;

    o->buttons = btn;
    /* Map int16 (-32768..32767) → uint8 (0..255), center=127 */
    o->leftStick.x  = (uint8_t)((lx + 32768) >> 8);
    o->leftStick.y  = (uint8_t)(255 - ((ly + 32768) >> 8));  // Y axis inverted
    o->rightStick.x = (uint8_t)((rx + 32768) >> 8);
    o->rightStick.y = (uint8_t)(255 - ((ry + 32768) >> 8));
    o->connected = 1;
    o->quat.w = 1.0f;
}
```

Then in the main read loop, add a branch:

```c
} else if (pid == PID_XBOX360 && rid == 0x00 && len >= 14) {
    parse_xbox360(buf, &pad);
    if (g_vdi_ready) inject_pad(&pad);
```

---

## Step 6: Add Init to USB Thread

In `usb_hid_thread()`, after the two-pass setup, add a branch for your controller:

```c
if (pid == PID_SWITCH && out_opened) {
    // 8BitDo Nintendo mode — full handshake (existing)
    ...
} else if (pid == PID_XBOX360 && out_opened) {
    uint8_t enable[] = {0x01, 0x03, 0x0E};
    usb_fs_send_out_report(fd, &eps[1], enable, sizeof(enable), "xbox360_enable");
    hs_state = HS_STREAMING;  // Xbox 360 has no handshake
} else {
    // Generic HID: no init, just start reading
    hs_state = HS_STREAMING;
}
```

---

## Button Mapping Reference

All PS5 `SCE_PAD_BUTTON_*` values used in `gc_main.c`:

```c
SCE_PAD_BUTTON_L3        = 0x00000002
SCE_PAD_BUTTON_R3        = 0x00000004
SCE_PAD_BUTTON_OPTIONS   = 0x00000008   // Options / Start / Menu
SCE_PAD_BUTTON_UP        = 0x00000010
SCE_PAD_BUTTON_RIGHT     = 0x00000020
SCE_PAD_BUTTON_DOWN      = 0x00000040
SCE_PAD_BUTTON_LEFT      = 0x00000080
SCE_PAD_BUTTON_L2        = 0x00000100
SCE_PAD_BUTTON_R2        = 0x00000200
SCE_PAD_BUTTON_L1        = 0x00000400
SCE_PAD_BUTTON_R1        = 0x00000800
SCE_PAD_BUTTON_TRIANGLE  = 0x00001000
SCE_PAD_BUTTON_CIRCLE    = 0x00002000
SCE_PAD_BUTTON_CROSS     = 0x00004000
SCE_PAD_BUTTON_SQUARE    = 0x00008000
SCE_PAD_BUTTON_CREATE    = 0x00010000   // Share / Back / View
SCE_PAD_BUTTON_PS        = 0x00010000   // PS button / Guide / Home
SCE_PAD_BUTTON_TOUCH_PAD = 0x00100000
```

`ScePadData` stick fields: `uint8_t x, y` — range 0–255, center = 127–128.
`ScePadData` trigger fields: `uint8_t l2, r2` — range 0–255.

---

## Checklist for a New Controller

- [ ] VID:PID added to constants and `ugen_find_target()`
- [ ] Controller type notification added
- [ ] Init sequence determined (native / Xbox / custom handshake)
- [ ] IN endpoint confirmed working (`max_packet_length` logged)
- [ ] Raw bytes logged per button press — report format mapped
- [ ] Parser function written and tested
- [ ] Read loop handles new `pid` and `rid` correctly
- [ ] Reconnect path tested (unplug and replug while payload running)

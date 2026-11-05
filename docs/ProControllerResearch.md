# Nintendo Switch Pro Controller USB Protocol Research

This document captures the complete research into making the 8BitDo Ultimate 2 (Nintendo Switch Pro Controller mode) work on PS5 via raw USB FS access. 

---

## Device Identification

| Property | Value |
|----------|-------|
| VID:PID (Nintendo mode) | `0x057e:0x2009` |
| VID:PID (Native mode) | `0x2dc8:0x310b` |
| PS5 device node | `/dev/ugen2.2` |
| USB bus | `usbus2`, depth=1 port=2 speed=FS |
| Kernel driver (pre-detach) | `usb_hid0` |
| IN endpoint | `ep=0x81`, wMaxPacketSize=64, bInterval=1ms |
| OUT endpoint | `ep=0x02`, wMaxPacketSize=64 (**NOTE: 0x02 not 0x01 like genuine Nintendo**) |

---

## PS5 USB Access Method

Direct kernel HID access via `usb_hid0` is blocked (`EPERM`). The working path:

```c
USB_IFACE_DRIVER_DETACH(iface=0)   // detach usb_hid0
USB_FS_INIT(ep_index_max=N)        // allocate N endpoint slots
USB_FS_OPEN(ep_no=0x81, ep_index=0)  // IN endpoint
USB_FS_OPEN(ep_no=0x02, ep_index=1)  // OUT endpoint
USB_FS_START(ep_index=0)           // start interrupt IN transfer
USB_FS_COMPLETE(ep_index=0)        // block until data
```

### Why Two-Pass Init

The IN endpoint (0x81) cannot be opened on the first try in some states. A "pass 1" is required:

```c
// Pass 1: unlock endpoint
FS_INIT(1 slot) → DETACH → FS_OPEN(ep=0x81) → FS_UNINIT → close()

// Pass 2: actual session
open() → DETACH → FS_INIT(2 slots) → FS_OPEN IN(0x81) + OUT(0x02)
```

Pass 1 does nothing functionally — it just exercises the endpoint to unlock it for pass 2.

---

## Critical Finding: Subcmd 0x03 Must Be Sent LAST

**Wrong order (pipe breaks):**
```
[80 02] → [80 04] → subcmd 0x03 0x30   ← USB pipe break, controller disconnects
```

**Correct order (works):**
```
[80 02] → [80 04]            ← USB handshake only
respond to 0x81 handshake    ← controller responds with 0x81 0x01 / 0x81 0x02
[80 04] again                ← handshake ack
subcmd 0x40 0x01             ← enable IMU
subcmd 0x48 0x01             ← enable vibration
subcmd 0x30 0x01             ← set player LED
subcmd 0x03 0x30             ← set input report mode (LAST)
```

Sending `subcmd 0x03` first causes a USB pipe break (`EBUSY` on `USB_FS_COMPLETE`) and the controller enters a reconnect loop that requires 2–3 additional handshake rounds to recover. With the correct order, `subcmd 0x03` is accepted cleanly and streaming starts immediately.

---

## Complete Initialization Sequence

### Phase 1: Two-Pass USB FS Setup

```c
// Pass 1 (unlock)
fd1 = open("/dev/ugen2.2", O_RDWR);
USB_FS_INIT(fd1, ep_index_max=1);
USB_IFACE_DRIVER_DETACH(fd1, iface=0);
USB_FS_OPEN(fd1, ep_index=0, ep_no=0x81, max_bufsize=64);
USB_FS_UNINIT(fd1);
close(fd1);
usleep(30000);

// Pass 2 (working session)
fd = open("/dev/ugen2.2", O_RDWR);
USB_IFACE_DRIVER_DETACH(fd, iface=0);
USB_FS_INIT(fd, ep_index_max=2);
USB_FS_OPEN(fd, ep_index=0, ep_no=0x81, max_bufsize=64);   // IN
USB_FS_OPEN(fd, ep_index=1, ep_no=0x02, max_bufsize=64);   // OUT
```

### Phase 2: Initial USB Handshake (Blind)

```c
send_out([0x80, 0x02]);  // handshake start
usleep(30000);
send_out([0x80, 0x04]);  // disable USB timeout
usleep(50000);
```

### Phase 3: Controller-Initiated Handshake Response

The controller sends `0x81` handshake packets. Respond **within ~200ms or the controller gives up:**

```
Controller sends: 0x81 0x01 → Host responds: [0x80, 0x02]
Controller sends: 0x81 0x02 → Host responds: [0x80, 0x04]
```

### Phase 4: Subcmd Init Sequence (MUST be in this order)

```c
send_subcmd(timer=1, subcmd=0x40, data=0x01);  // enable IMU
usleep(50000);
send_subcmd(timer=2, subcmd=0x48, data=0x01);  // enable vibration
usleep(50000);
send_subcmd(timer=3, subcmd=0x30, data=0x01);  // set player LED
usleep(50000);
send_subcmd(timer=4, subcmd=0x03, data=0x30);  // input mode = full push (LAST)
```

Subcmd packet format (12 bytes):
```c
uint8_t sc[12] = {0};
sc[0] = 0x01;       // output report ID
sc[1] = timer;      // 4-bit counter, must increment each packet
// sc[2..9] = neutral rumble = {0,1,0x40,0x40,0,1,0x40,0x40}
sc[10] = subcmd;
sc[11] = data;
```

### Phase 5: Read Loop

After `subcmd 0x03`, the controller immediately sends streaming data:

```c
while (1) {
    USB_FS_START(ep_index=0);
    USB_FS_COMPLETE(ep_index=0, timeout=200ms);
    // process pkt[0..63]
}
```

---

## Streaming Packet Format

After the init sequence, the controller sends `0x00`-prefixed packets at ~60Hz:

```
Byte  0: 0x00         — always 0x00 (report ID stripped by 8BitDo firmware)
Byte  1: timer        — 8-bit counter, increments ~6-8 units per frame
Byte  2: 0x70         — battery/connection (0x70 = USB, full charge)
Byte  3: right_btns   — Y X B A SR SL R ZR (bits 0-7)
Byte  4: misc_btns    — Minus Plus R3 L3 Home Capture (bits 0-5)
Byte  5: left_btns    — Down Up Right Left SR SL L ZL (bits 0-7)
Bytes 6-8:  left stick  (12-bit packed, center ~0x800)
Bytes 9-11: right stick (12-bit packed, center ~0x7FF)
Byte 12: vibration status (0x0b = idle)
Bytes 13-47: IMU data (3 × 6-axis samples, when IMU enabled)
```

**Note:** Genuine Nintendo Switch Pro Controllers send `0x30` as byte 0 (the report ID). The 8BitDo sends `0x00`. The rest of the layout is identical.

### Stick Decoding (12-bit packed)

```c
uint16_t lx = buf[6] | ((buf[7] & 0x0F) << 8);   // 0–4095, center ~2048
uint16_t ly = (buf[7] >> 4) | ((uint16_t)buf[8] << 4); // 0–4095, center ~2047
uint16_t rx = buf[9] | ((buf[10] & 0x0F) << 8);
uint16_t ry = (buf[10] >> 4) | ((uint16_t)buf[11] << 4);

// Map to 0–255 for ScePadData:
uint8_t x = (uint8_t)((v * 255u) / 4095u);
```

### Button Bit Mapping

```
buf[3] — right face buttons:
  bit 0: Y     → PS5 Square
  bit 1: X     → PS5 Triangle
  bit 2: B     → PS5 Cross
  bit 3: A     → PS5 Circle
  bit 6: R     → PS5 R1
  bit 7: ZR    → PS5 R2

buf[4] — shared buttons:
  bit 0: Minus    → PS5 Create/Share
  bit 1: Plus     → PS5 Options
  bit 2: R3       → PS5 R3
  bit 3: L3       → PS5 L3
  bit 4: Home     → PS5 PS button

buf[5] — left buttons:
  bit 0: Down  → PS5 Down
  bit 1: Up    → PS5 Up
  bit 2: Right → PS5 Right
  bit 3: Left  → PS5 Left
  bit 6: L     → PS5 L1
  bit 7: ZL    → PS5 L2
```

---

## Reconnect Handling

On controller unplug/replug:
- `USB_FS_START` or `USB_FS_COMPLETE` returns `ENXIO` or `ENOTTY`
- Close all endpoints, `USB_FS_UNINIT`, `close(fd)`
- Re-scan `ugen` paths for the device (20s timeout)
- Run the full two-pass init + handshake sequence again

On reconnect without full power cycle:
- First packet may be all-zeros (`00 00 00 00...`) — discard it
- Then `0x81` handshake packets appear — run Phase 3 and 4 again

---

## USB FS COMPLETE Polling

`USB_FS_COMPLETE` is non-blocking. Poll at 50ms intervals for up to `timeout_ms + 300ms`:

```c
int max_polls = (int)((timeout_ms + 300) / 50) + 1;
for (int w = 0; w < max_polls; w++) {
    if (ioctl(fd, USB_FS_COMPLETE, &co) == 0) break;
    if (errno != EBUSY) { /* error */ break; }
    usleep(50000);
}
```

**Do not** use 5-retry × 10ms — the USB transfer timeout is 200ms so you'll stop polling before the transfer can complete, leaving the endpoint stuck in `EBUSY`.

---

## Things That Do Not Work

| Approach | Result |
|----------|--------|
| `USB_GET_REPORT_DESC` | `errno=25 ENOTTY` |
| `read(fd, buf, 64)` without DETACH | `errno=1 EPERM` |
| `libSceHidControl` | Controller not auto-registered; ShellCore not initialized |
| `libSceUsbd` | Not loaded in SceShellCore or SceShellUI |
| Soft-reinit after `[80 04]` | Controller goes silent (wrong approach) |
| Sending `subcmd 0x03` first | USB pipe break → reconnect loop |
| Responding to `0x81 0x02` without subcmds | Controller silent after `[80 04]` |

---

## Research Probe History

Probes run on PS5 (`usb_handshake_probe v1` through `v12`) at port 6986, reports fetched from FTP `/data/ghostpad/`:

| Version | Key Discovery |
|---------|--------------|
| v1 | Confirmed soft-reinit after subcmd 0x03 pipe-break works |
| v2–v3 | Fixed COMPLETE polling (50ms intervals needed, not 10ms) |
| v4–v5 | Identified full USB bus-reset after `[80 04]` in second handshake |
| v6 | Second subcmd 0x03 also causes bus-reset — infinite loop |
| v7–v8 | Discovered controller gives up if host takes >200ms to respond to 0x81 |
| v9 | Tight 200ms loop — controller responds within timing window |
| v10 | Added IMU/vibration subcmds — still no streaming |
| v11 | **BREAKTHROUGH**: subcmd 0x03 sent LAST → immediate 0x30 streaming |
| v12 | Full 64-byte packet dump — confirmed format, verified stick decode |

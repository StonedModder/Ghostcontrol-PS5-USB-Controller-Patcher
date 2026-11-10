# Supported controllers

Compatibility matrix for **Ghostcontrol — Manba V2 NBJr USB patch** (`payload/`). A device is *supported* only when Ghostcontrol can identify it, detach the USB driver, open the correct endpoints, parse input into `ScePadData`, and inject through the virtual DualSense (VDI) path.

For how to add new devices, see [othercontrollersGuide.md](othercontrollersGuide.md). Build and deploy: [README.md](README.md).

---

## Hardware-tested

| Controller | Mode | VID:PID | Parser / path | Notes |
|------------|------|---------|---------------|-------|
| 8BitDo Ultimate 2 | Nintendo Switch Pro | `057e:2009` | Nintendo / Manba Switch | Original confirmed path; IN `0x81`, 64-byte reports |
| 8BitDo Ultimate 2C Wireless (81HD) | XInput | `2dc8:310a` | Manba XUSB (reuse) | Composite device: IN `0x84`, OUT `0x05` (not classic `0x81`/`0x01`). USB-C cable and 2.4G dongle.. |

---

## Documented / untested on PS5

| Controller | Mode | VID:PID | Status |
|------------|------|---------|--------|
| 8BitDo Ultimate 2 | Native (8BitDo USB) | `2dc8:310b` | Listed in README; **not** routed in this Manba-focused build until a dedicated path is added |

---

## Manba V2 NBJr (this patch)

| Mode | Detection | Endpoints (typical) | Status |
|------|-----------|---------------------|--------|
| PC / XInput | `045e:028e` or USB interface subclass `0x5d`, protocol `0x01` | IN `0x81`, OUT `0x02` or `0x01` | Routed via `controller_mamba.c` |
| Switch USB | `057e:2009` | IN `0x81`, 64-byte HID | Same VID:PID as Switch Pro clones; shares Nintendo-style path |

---

## Ignored (safe skip)

| Device | VID:PID | Reason |
|--------|---------|--------|
| Manba receiver idle / update | `1a34:f517` | Not a playable controller |
| Xbox One / Series (GIP) | Interface `0x47` / `0xd0` | Ignored in this Manba patch build after identification |
| Unknown USB devices | — | Descriptor read only; no destructive endpoint probe when VID:PID is unrecognized |

---

## XInput endpoint selection (8BitDo 2C and Manba)

Classic Manba XInput uses IN `0x81` and OUT `0x02` (fallback `0x01`). The 8BitDo Ultimate 2C Wireless uses IN `0x84` and OUT `0x05` because it is a **composite** device (XInput + HID keyboard/mouse). `probe_one_path()` matches `2dc8:310a` **before** the generic XInput interface normalizes to `045e:028e`, so the correct endpoints are opened in `usb_hid_thread()`.

---

## Sources

- `payload/gc_main.c` — scan, probe, USB threads
- `payload/controller_mamba.h` / `controller_mamba.c` — Manba + shared XUSB parser
- `payload/controller_nintendo.c` — Switch Pro protocol
- `README.md` — quick reference table
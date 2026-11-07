# Ghostcontrol — by StonedModder

If you enjoy my work - please consider donating to my BTC address: 

`bc1qa9zfgnccajsw8vg7k287qz5a7apf8pefj5jjx5`


Use third-party USB controllers on PS5. Reads USB HID input from a plugged-in controller and injects it into a virtual DualSense via the PS5's `scePadVirtualDeviceInsertData` path (Ghostpad VDI path).

**Tested controller:** 8BitDo Ultimate 2 in Nintendo Switch Pro Controller mode (VID=0x057e PID=0x2009)

---



https://github.com/user-attachments/assets/6583b1c2-3d3d-4f2e-9e79-689121fea4a3



## Features

- 60Hz input streaming from USB HID controller → virtual DualSense
- PS5 notifications: startup, controller connect/disconnect, detected controller type
- User assignment: virtual DualSense is bound to the foreground user on startup
- Full button mapping: face buttons, triggers, sticks, dpad, L3/R3, PS button
- Auto-reconnect on controller unplug/replug

---

## Requirements

- PS5 with kernel exploit (tested on jailbroken PS5)
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)
- USB controller — see supported list below

---

## Supported Controllers

| Controller | Mode | VID:PID | Status |
|-----------|------|---------|--------|
| 8BitDo Ultimate 2 | Nintendo Switch Pro | 057e:2009 | ✅ Working |
| 8BitDo Ultimate 2 | Native | 2dc8:310b | Untested |
| Xbox One S / Series S controller | GIP (IN 0x82 / OUT 0x02) | 045e:02ea | ✅ Working |
| Xbox One controller, original 2013 (model 1537) | GIP (IN 0x81 / OUT 0x01) | 045e:02dd | ✅ Working ¹ |
| Xbox Series X controller | GIP | 045e:… | ✅ Working ¹ (quick test) |
| PowerA Xbox Series X EnWired (BDA) | GIP (IN 0x81 / OUT 0x01) | 20d6:200b | ✅ Working ¹ |
| Generic GameCube-style USB pad (DragonRise / "Microntek USB Joystick") | HID | 0079:0006 | ✅ Working ¹ |
| Generic N64-style USB pad ("SWITCH CO.,LTD.") | HID on PC, Switch Pro mode on PS5 | 0e6d:111d / 057e:2009 | ✅ Working ¹ |
| Sony DualShock 4 (PS4 controller) v1 / v2 | HID, interface 3 | 054c:05c4 / 09cc | ⚠️ Experimental, off by default ² |

¹ Tested on a PS5 launch ("fat") model, firmware 11.40.

² The PS5 supports the DualShock 4 natively for PS4 games, so by default
Ghostcontrol leaves it alone. Setting `ds4=takeover` in
`/data/ghostpad/config.ini` (created on first run; edit over FTP, then restart
the payload) makes the payload present it as a virtual DualSense for PS5 games.
This currently doesn't work reliably: the PS5 either keeps resetting the
controller or ignores the virtual DualSense's input.

Controllers are identified in this order:
1. Vendor ID from the USB device descriptor: Microsoft `045e` → Xbox,
   Nintendo `057e` → Switch Pro, `0079:0006` → GameCube-style pad.
2. Xbox One/Series (GIP) interface signature, class `ff` / subclass `47` /
   protocol `d0`, so licensed third-party Xbox pads (PowerA, PDP, Hori, Razer…)
   use the Xbox path without listing each vendor.
3. Endpoint shape, as a last resort. Look for the `probe:` lines in
`/data/ghostpad/gc_status.log` to see how a controller was identified.

See `othercontrollersGuide.md` for adding new controllers.

### GameCube-style pad mapping

| GameCube | PS5 |
|----------|-----|
| A / B / X / Y | Cross / Square / Circle / Triangle (by position) |
| Z | R1 |
| L / R | L2 / R2 (digital, sent as full press) |
| Start | Options |
| Start + Z | PS button |
| Main stick / C-stick | Left stick / Right stick (C-stick scaled ×2 to full range) |
| D-pad | D-pad |

The pad has no L1, L3, R3 or Share. Other DragonRise-based pads may number
their buttons differently: run `tools/gcpad_capture.c` on Linux
(`gcc -O2 -o gcpad_capture tools/gcpad_capture.c -lusb-1.0 && sudo ./gcpad_capture`)
and adjust `GCPAD_BTN_MAP` in `payload/controller_gcpad.c`.

### N64-style pad mapping

This pad changes identity: on a PC it is a plain HID gamepad (`0e6d:111d`),
on the PS5 it poses as a Switch Pro Controller (`057e:2009`). The payload
recognises the Pro-mode version by its manufacturer string `SWITCH CO.,LTD.`,
so genuine Pro Controllers and 8BitDo pads keep the standard Pro mapping.

| N64 | PS5 |
|-----|-----|
| A / B | Cross / Square |
| L / R / Z | L1 / R1 / R2 |
| Start (tap) | Options (sent on release) |
| Start + Z | PS button |
| Start + B | Circle (back) |
| C-up / down / left / right | Right stick |
| Stick / D-pad | Left stick / D-pad |

To map a different HID pad, `tools/pad_capture.c` works with any controller:
`gcc -O2 -o tools/pad_capture tools/pad_capture.c -lusb-1.0 && sudo ./tools/pad_capture VID:PID [n64|gamecube|generic]`.

---

## Build

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make clean all
```

Output: `ghost-control-ps5.elf`

## Deploy

```sh
# Deploy to PS5 (replace IP)
nc -w 5 192.168.1.xxx 9021 < ghost-control-ps5.elf
```

Or set `PS5_HOST` in your environment:
```sh
make deploy PS5_HOST=192.168.1.xxx
```

---

## How It Works

1. **VDA**: Creates a virtual DualSense via `scePadVirtualDeviceAddDevice(type=3)`
2. **klog capture**: Monitors klogsrv TCP to detect the `DEVICE_ADDED` event and get the device handle
3. **force_bind**: Binds the virtual device to the foreground user via ShellUI MBus IPC
4. **USB HID thread**: Detaches `usb_hid0` from the controller, opens raw USB FS endpoints, runs the Nintendo Switch Pro Controller USB handshake, then reads 60Hz input reports
5. **VDI inject**: Parses HID reports into `ScePadData` and calls `scePadVirtualDeviceInsertData` at 60Hz

See `ProControllerResearch.md` for the full research documentation on the USB HID protocol.

---

## Files

| File | Description |
|------|-------------|
| `gc_main.c` | Main payload — VDA, VDI, USB HID thread, button parsing |
| `shellui_pad.c` | ShellUI PT_ATTACH helper for force_bind via MBus |
| `shellui_pad.h` | Header for shellui_pad |
| `Makefile` | Build system |
| `ghost-control-ps5.elf` | Pre-compiled payload (deploy directly) |
| `ProControllerResearch.md` | Full USB protocol research for Nintendo Switch Pro Controller |
| `othercontrollersGuide.md` | Guide for adding other USB HID controllers |

---

## License

GPL-3.0-or-later

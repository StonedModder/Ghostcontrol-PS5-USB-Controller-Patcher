# Xbox One S / Series S GIP Wire Button Bits
**VID=0x045E PID=0x02EA — hardware-confirmed on PS5 via live GIP probe**

## GIP INPUT packet (cmd=0x20) layout
```
b[0]=0x20  b[1]=seq  b[2]=opts  b[3]=0x0E (payload len=14)
b[4]  = button byte 1 (digital buttons)
b[5]  = button byte 2 (dpad + bumpers + sticks)
b[6..7]  = LT uint16 LE  0-1023  → scale to 0-255
b[8..9]  = RT uint16 LE  0-1023  → scale to 0-255
b[10..11]= LX int16 LE   center≈0
b[12..13]= LY int16 LE   center≈0
b[14..15]= RX int16 LE   center≈0
b[16..17]= RY int16 LE   center≈0
```

## b[4] — face buttons + system buttons
| Bit  | Mask | Physical button | PS5 target        |
|------|------|-----------------|-------------------|
| bit2 | 0x04 | Menu (≡)        | OPTIONS  (0x0008) |
| bit3 | 0x08 | View (⧉)        | SHARE    (0x20000)|
| bit4 | 0x10 | A               | CROSS    (0x4000) |
| bit5 | 0x20 | B               | CIRCLE   (0x2000) |
| bit6 | 0x40 | X               | SQUARE   (0x8000) |
| bit7 | 0x80 | Y               | TRIANGLE (0x1000) |

## b[5] — dpad + bumpers + stick clicks
| Bit  | Mask | Physical button | PS5 target     |
|------|------|-----------------|----------------|
| bit0 | 0x01 | DPad Up         | UP    (0x0010) |
| bit1 | 0x02 | DPad Down       | DOWN  (0x0040) |
| bit2 | 0x04 | DPad Left       | LEFT  (0x0080) |
| bit3 | 0x08 | DPad Right      | RIGHT (0x0020) |
| bit4 | 0x10 | LB              | L1    (0x0400) |
| bit5 | 0x20 | RB              | R1    (0x0800) |
| bit6 | 0x40 | L3 (left click) | L3    (0x0002) |
| bit7 | 0x80 | R3 (right click)| R3    (0x0004) |

## Analog
| Field       | Range  | PS5 field           |
|-------------|--------|---------------------|
| LT (b[6..7])| 0-1023 | analogButtons.l2 (0-255), digital L2 bit if >16 |
| RT (b[8..9])| 0-1023 | analogButtons.r2 (0-255), digital R2 bit if >16 |
| LX (b[10..11])| int16 center≈0 | leftStick.x  = (lx+32768)>>8, deadzone ±7849 |
| LY (b[12..13])| int16 center≈0 | leftStick.y  = 255-((ly+32768)>>8), deadzone ±7849 |
| RX (b[14..15])| int16 center≈0 | rightStick.x = (rx+32768)>>8, deadzone ±7849 |
| RY (b[16..17])| int16 center≈0 | rightStick.y = 255-((ry+32768)>>8), deadzone ±7849 |

## Guide button (Xbox logo) — HARDWARE CONFIRMED
```
GIP cmd=0x07  len=6
b[4]=0x01 = pressed
b[4]=0x00 = released
b[5]=0x5b (constant — controller status byte, ignore)
```
Maps to SCE_PAD_BUTTON_PS (0x10000).
Current code: `if (cmd == 0x07 && buf[4] & 0x01u)` — correct.

## Notes
- xpad.c (Linux) labels bit2=View, bit3=Menu — WRONG for this hardware.
  Confirmed physically: bit2=Menu(≡), bit3=View(⧉).
- SCE_PAD_BUTTON_CREATE = SCE_PAD_BUTTON_PS = 0x10000 — injecting this
  triggers PS home screen. View maps to SHARE (0x20000) instead.
- GIP wire bits differ completely from XInput wButtons API constants.
  XInput is a Windows abstraction layer; these are raw USB GIP bytes.
- Idle stick drift observed: lx≈-1863, ly≈-149, rx≈2007, ry≈-771.
  Deadzone of ±7849 covers this entirely.

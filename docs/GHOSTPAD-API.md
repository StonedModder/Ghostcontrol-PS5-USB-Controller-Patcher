# GhostPad API — remote pad transport, and the ESP32-S3 companion rig

Status: **transport implemented, not wired into the 2.0 app-service.**
`core/src/ghostPad/gc_ghostpad.{c,h}` is compiled into the resident line
(`gc_main.c`, `CTRL_GHOSTPAD`) only. The service reports
`capabilities.ghostpad_output = false` and `[ghostpad] enabled=false` in
`config.ini` by default.

## Why this document exists

The companion firmware project (ESP32-S3 + Bluepad32, "GhostPadAPI") needs a
fixed contract on the console side before any firmware is written. This is that
contract, plus the design constraint that decides the firmware's architecture.

## Wire format (do not change without versioning it)

| Property | Value |
|---|---|
| Transport | TCP (`SOCK_STREAM`), console listens |
| Default port | `6967` (`[ghostpad] port=6967`) |
| Packet size | 16 bytes |
| Layout | `uint32 buttons`, then `lx, ly, rx, ry, l2, r2` (little-endian) |
| Input packet | decodes as **1** (`GPAD`) |
| Disconnect | decodes as **0** (`DISC`) |
| Invalid | decodes as **-1** |
| Stale release | `stale_release_ms`, default **250 ms** — no packet means release, never stick |

`gc_ghostpad_decode()` is the single decoder; `gc_ghostpad_start()` takes ops
callbacks for `connected` / `input` / `disconnected`.

## What has to happen on the console side first

1. Wire `gc_ghostpad_*` into the app-service path (it currently exists only on
   the resident path), with the same lifecycle discipline as the other sources.
2. Gate the virtual pad on real input, exactly as `gc_service_inject.c` does for
   USB: a GhostPad source with no packets must not be announced, or the console
   raises "Who's using this controller?" locked to an undrivable device and
   softlocks.
3. Flip `ghostpad_output` to `true` only once a packet from a live client has
   driven a real state, and record that evidence here.

## Design constraint for the companion

**The ESP32-S3 has one USB-OTG controller.** It cannot simultaneously be a USB
host (reading wired pads) and a USB device (presenting a pad to the PS5). So the
S3 is a **USB/Bluetooth host for controllers** and reaches the console **over
WiFi as a GhostPad client**. Presenting over USB would need separate silicon.

## Firmware mission (summary)

- Bluepad32 on ESP-IDF for ESP32-S3; custom logic in a `ghostpad_api` component.
- Ingest Bluetooth Classic, BLE, and wired USB pads; report vid:pid, transport,
  report rate, parse confidence.
- Speak the GhostPad contract above at a configurable rate (default 250 Hz).
- A line-delimited JSON control channel (suggest port `6970`) for remote
  automation: `hello`, `sources`, `select`, `state`, `rate`, `replug`,
  `connect`/`disconnect`, `stats`, `log`.
- `replug` simulates a physical replug (VBUS/per-port power cycle for USB, forced
  disconnect + re-pair for Bluetooth) and must never require a physical power
  cycle to recover.
- Watchdog: release the pad and close the socket if the control channel goes
  quiet; explicit panic-release on internal error.

## Reference material

- `Ghostcontrol-PS5-USB-Controller-Patcher` (public beta):
  `ProControllerResearch.md`, `othercontrollersGuide.md`,
  `Supported_controllers.md` — the protocol notes this repo's Nintendo path came
  from.
- Nintendo bring-up on the service path: see project notes, commit `4aa33df`.
- Profile-as-data path for unknown pads: project notes, commit `0be54cf`.

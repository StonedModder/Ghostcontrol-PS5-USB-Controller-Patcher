# Feature parity checklist — 1.x toolbox → 2.0

The point of this file is that nothing gets lost. Every capability the XMB
toolbox had is listed with the source that implements it, the IPC commands it
is driven by, and whether the 2.0 app has a screen for it yet.

**This is a source inventory, not a functionality claim.** A source file,
build, declared UI command, or successful IPC acknowledgement does not prove
runtime behavior. Track each capability separately as implemented, host-tested,
integrated, hardware-tested, or unavailable. The user has reported ineffective
toggles and controller effects; those remain acceptance failures until the real
state changes and controller output are verified. Current evidence belongs in
project notes and the per-row audit in `docs/USABILITY-MATRIX.md`.

`legacy reference holds all 194 element ids
from the 1.x UI, and `legacy reference holds
the command vocabulary. Those two files are the specification to port against.
`docs/UI-DESIGN.md` says which widget each screen is built from.

| Capability | core (present) | IPC commands | UI |
|---|---|---|---|
| USB patching for third-party controllers | `gc_usb.c`, `usb_helpers.c`, `controller_xbox/ds4/ds3/nintendo/logitech/mamba.c` | `DEVICES`, `CONTROLLERS`, `REBIND`, `FORCE_VDA` | wired+host-tested (`DEVICES`/`CONTROLLERS` rows; `REBIND`/`FORCE_VDA` present but policy-refused as privileged reset) |
| Universal XInput/DInput handling | `gc_hid.c` (descriptor parse), `gc_rawmap.c` | `DEVICES`, `SETUP_*` | wired+host-tested |
| Controller setup wizard (guided) | `gc_setup.c` | `SETUP_BEGIN/ARM/INPUT/SKIP/SAVE/CANCEL/STATUS` | wired; `SETUP_SKIP` literal untested (WIRE-01 F2) |
| Real-time config for unknown controllers | `gc_setup.c`, `gc_rawmap.c`, `gc_hid.c` | `SETUP_ARM`, `SETUP_INPUT`, `SETUP_STATUS` | wired+host-tested (worker live input) |
| Custom controller rules | `gc_config.c`, `gc_rawmap.c` | `CONTROLLER_SAVE` | wired+host-tested |
| Profiles: create/edit/rename/save | `gc_profile.c`, `gc_config.c` | `PROFILE_CREATE/EDIT/RENAME/SAVE/GET`, `PROFILES` | wired+host-tested (rows + keybinder overlay) |
| Profile import/export (sharing) | `gc_profile.c`, `gc_file_safety.c` | `PROFILE_IMPORT`, `PROFILE_EXPORT`, `PROFILE_FILES` | wired+host-tested (rows + picker) |
| Rumble control | `gc_dualsense_fx.c`, per-controller writers | `DUALSENSE_RUMBLE`, `DUALSENSE_RUMBLE_STOP` | wired+host-tested (native API result reported; hardware output needs console) |
| LED / lightbar control | `gc_dualsense_fx.c`, `gc_dualsense_fx_scan.c` | `DUALSENSE_FX_STATUS`, `RUNTIME_SET dualsense_fx_*` | wired+host-tested (lightbar needs console) |
| Gyro / motion reads | `gc_dualsense_fx.c`, `gc_dualsense_fx_remote.c` | `DUALSENSE_FX_STATUS` | display-only live read (see note; no console motion claim) |
| Bluetooth: third-party controllers | `gc_bt.c`, `gc_bt_hci.c`, `src/bt/*` (Classic + LE) | `BLUETOOTH_STATUS` | wired+host-tested (worker fetch + diagnostics rows) |
| Bluetooth: pair / forget / retry | `gc_bt.c` | `BLUETOOTH_PAIR`, `BLUETOOTH_FORGET`, `BLUETOOTH_RETRY` | wired; `BLUETOOTH_RETRY`/`BLUETOOTH_STOP` literals untested (WIRE-01 F2) |
| Console serial + openpsid reads | `gc_console_id.c` | `CONSOLE_ID` | wired+host-tested (worker fetch) |
| Virtual DualSense + slot assignment | `gc_main.c`, `gc_vda_klog.c` | `FORCE_VDA`, `REBIND`, `STATUS` | slots/status wired+host-tested; rebind/VDA-reset policy-refused (system assignment kept) |
| GhostPad (network pad transport) | `src/ghostPad/gc_ghostpad.c` | `RUNTIME_GET/SET ghostpad_*` | wired+host-tested (runtime toggles) |
| DS4 passthrough / game bridge | `gc_ds4_bridge.c`, `gc_ds4_game.c`, `gc_ds4_policy.c`, `gc_ds4_source.c` | `RUNTIME_SET wireless_ds4` | wired+host-tested (runtime toggle; input side only) |
| Service control (start/stop/restart) | `gc_main.c`, `gc_state.c` | `START`, `STOP`, `RESTART`, `RELOAD` | wired+host-tested (rows) |
| Settings (autostart, daemon, ports…) | `gc_config.c` | `RUNTIME_GET`, `RUNTIME_SET` | wired+host-tested (runtime rows) |
| Updates: check / download / rollback | `gc_update.c`, `gc_update_policy.c` | `UPDATE_CHECK/DOWNLOAD/ROLLBACK` | wired; `UPDATE_CHECK` literal untested (WIRE-01 F2) |
| Diagnostics + log tail | `gc_main.c`, `gc_state.c`, `gc_log_tail.c` | `LOG_TAIL [lines]`, `STATUS` | wired+host-tested (worker fetch + reader rows) |
| Credits / attribution | n/a (presentation only) | n/a | display-only rows (text bundled; testers row loads `testers.txt`) |


> **Status column reconciled 2026-10-14 by WIRE-01** (`
> the previous blanket `to port` text was stale. States above are source-derived
> (79 rows enumerated across 8 screens; 25 row-fired commands; core suite exit 0,
> UI suite 121 checks 0 failures). host-tested = ordinary host gates only;
> target-verified remains zero (no console run). Unwired-on-main: the Classic
> instance consumers (`CONTROLLER_INSTANCE_SAVE`, `SETUP_BEGIN_INSTANCE`) are
> core-tested but their UI consumers live on pending branch `classic-scope-uint64`.

## Notes on the three that are not a straight port

**Native pad output / live motion.** The legacy remote-process path is not
a valid native-app integration. Use ordinary controller APIs in the owning app
and an explicit companion IPC contract, without ShellUI attachment, process
memory access, or privilege changes. Native pad connectivity, adopted VDA slots,
and saved controller rules are different inventories. A compile/stub check is
not evidence that input or output works on hardware.

**3D controller preview.** 1.x drew it with three.js in a web view
(`legacy reference That does not port to the native OpenGL renderer: either
rebuild it against the widget kit or show a 2D diagram instead.

**Credits.** Carried over as text to re-present, since it was pure
presentation in the Settings XML.

## Diagnostics / LOG_TAIL limits

`LOG_TAIL` takes an optional line count (`GC1 LOG_TAIL 40`); with no argument
it returns 200 lines, clamps to 1000, and rejects a blank, zero or
non-numeric argument. The reply is one hex-encoded line,
`{"ok":true,"encoding":"hex","lines":N,"data":"<hex>"}`, capped at 16384
bytes of log (32768 hex characters) so it always fits `GC_IPC_MAX_RESPONSE`
(65536). The ceilings live in `core/src/gc_log_tail.h`.

## Bluetooth provenance

The Bluetooth host in `core/src/bt/` is the console-side stack for this product:
opening the MediaTek radio as a USB HCI device and running inquiry, link keys,
L2CAP, SDP and HID over it, plus SMP and GATT for LE. Support files
(`crc32.c`, `evstream.c`, `profiles.c`, `smp_crypto.c`, `usb_desc.c`) have been
restructured or extended in place; `profiles.c` also carries Joy-Con, Wii / Wii U
Pro, and Hori profile tables.

The one hard-won detail worth never losing: **on the MediaTek `0e8d:3603` the
ACL OUT is the second bulk OUT** — the descriptor's first bulk OUT there runs a
written ACL packet as an HCI command (opcode `0x2033`) that the system stack
also sees. `bt/hci_usb.c` chooses `0x02` at open time on that chip and keeps
`0x01` as the fallback; every other chip keeps descriptor order and switches to
the second pipe only when the first fails.


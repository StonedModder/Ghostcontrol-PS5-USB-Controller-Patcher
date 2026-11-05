# Controller output identity preferences

## Input identity is not output identity

A controller rule's `protocol` chooses the physical input decoder. Its
`output_type` is the requested virtual identity presented to games:

- `ds5`: DualSense.
- `ds4`: DualShock 4.
- `psvr2_left`: PSVR2 Sense left.
- `psvr2_right`: PSVR2 Sense right.

The enum in `core/src/gc_controller_type.h` is an internal preference contract.
Its numbers are not Sony virtual-device constants and do not define a PSVR2
tracking/report ABI. A missing field in an older rule defaults to `ds5`.

Rules currently have unique VID:PID admission identities. Two physical devices
with the same VID:PID cannot independently select different output identities
through this rule schema. Rule IDs are case-sensitive opaque identifiers; do not
lowercase, regenerate or otherwise normalize them when editing a type.

## Persistence and IPC

`core/src/gc_controller_commands.c` is shared by the companion controller-command
dispatch. `CONTROLLERS` returns complete saved rules and `output_types` metadata.
`CONTROLLER_OUTPUT_SET <id>|<type>` changes the requested identity without dropping
name, protocol, endpoints/interface, VID/PID, enabled or ignored state.

`CONTROLLER_SAVE` accepts the established 9/10-field forms and an extended
11-field form ending in `output_type`. Its multi-field text is hex-encoded as
before. Numeric fields, field counts and control characters are validated;
invalid/duplicate config fields do not publish a partial replacement.

A successful preference save is not successful emulation. Replies explicitly
report:

```json
{
  "ok": true,
  "saved": true,
  "applied": false,
  "output_supported": false,
  "effect": "controller_rule_persisted"
}
```

The native Controllers picker sends the type-only command and confirms the exact
saved ID/type through authoritative `CONTROLLERS` readback. Unsupported choices
remain visible as preferences rather than claiming they were applied.
Controller mutations, their persisted rule readback, and their acknowledgement
`generation` share one config-store writer transaction. Type-only commands merge
only `output_type` into the latest rule. Legacy 9/10-field saves preserve the
latest committed type inside that transaction; explicit 11-field saves still
replace the fields the caller supplied. Setup merges preserve every existing
rule field, including disabled/ignored admission. Only new rules get defaults.
The guard covers writers using this in-process store, not independent processes
editing its files.

## Source inventory and setup

`DEVICES` distinguishes physical USB/BT source input from live virtual-device
creation and output application. Core setup accepts an identified source path
and matching VID:PID, reads that
source's original decoded input, and uses its physical protocol. It does not
learn from an unrelated app pad or remapped virtual output. Setup saves merge
with the latest matching rule inside the config transaction; they preserve all
existing rule fields rather than implicitly re-enabling a disabled/ignored rule.
A verified `SETUP_SAVE` returns `accepted`, `saved`, `verified`, `profile_id`,
`controller_id`, the save's `generation`, and `source_connection_generation`,
with `applied=false`. A committed save whose rule readback fails instead returns
`ok=false`, `saved=true`, `verified=false` and `setup_saved_readback_failed`;
it does not claim that the commit was undone.

Each observed USB/BT connection has a nonzero `connection_generation`, exposed
in `DEVICES`. Tokens are unique within the resident process, including service
close/init cycles; exhaustion refuses allocation rather than wrapping. Begin
binds the selected token and original sample with a checked read. Input, status,
arm, skip and save reject a changed token, including an observed same-path,
same-model reconnect. `SETUP_STATUS` exposes `source_valid` and
`source_connection_generation`. A mismatch clears cached input/raw bytes but
retains the old draft as invalid; it never silently adopts a replacement.
Rebegin preserves learned changes only on the same valid connection; a new
connection starts from the persisted profile instead.

Save serializes the draft across the config writer transaction. Its connection
guard follows `feature mutex -> config writer -> source transition -> snapshot
mutex`; the snapshot mutex is released immediately after validation. The source
transition fence remains held through persistence, readback or rollback and is
released once on every acquired path. Validation/filesystem failures keep the
draft available for retry. The fence protects observed in-process transitions,
not physical continuity or independent file writers; filesystem I/O can delay
source transitions. Service shutdown requires command/worker quiescence.

The production Guided setup chooser now requests a fresh `DEVICES` reply and
lists only identified connected USB/BT sources with decoded input. It does not
choose the first STATUS/VDA slot or offer an unidentified initial-user sample.
`setup_profile_id` carries an existing exact enabled profile ID without changing
its spelling. An empty ID produces a model/protocol candidate
`pad-<vid>-<pid>-<protocol>`; this is not a distinct hardware-instance identity.
Unrelated profile-ID collisions remain refused by the config transaction.

The chooser submits hex-encoded
`path|vid_hex|pid_hex|profile_id|identity|connection_generation`. Core still accepts
the legacy five-field form for existing direct IPC clients. The sixth field is a
nonzero decimal uint64, with zero, signs, fractions and overflow rejected; a
mismatched selected connection returns `setup_source_changed`. The UI worker
revalidates fresh STATUS capabilities and the complete DEVICES selection before
Begin, then verifies the exact token in the reply and subsequent SETUP_STATUS.
Only matching active/source-valid readback confirms a binding, never game output.
The chooser correlates completion with its own nonzero snapshot request ID,
not a newer endpoint generation. Background polls and other fetches cannot
complete or overwrite that receipt. Request IDs remain unique across snapshot
start/stop and refuse exhaustion. The bounded receipt store evicts completed
records only; an unavailable receipt fails closed rather than using another
attempt. These local request IDs are separate from physical connection tokens.
Pending, cancelled, failed or invalid inventory cannot select retained last-good
options; changing source generation or profile ID requires selecting again.

Diagnostics exposes `Selected source raw packet` as a separate read-only
`SETUP_INPUT` reader. It displays the bound identified physical source's last
service-cached bytes, exact connection token and path, with a 32-byte ceiling.
Cross manually requests the cache again; this is a snapshot, not a live monitor.
It does not infer arbitrary HID learning or establish game output. Inactive,
changed, disconnected or malformed sources show no packet bytes. Failed fetches,
missing receipts and queue refusal clear a previous successful display rather
than retaining it as current.

All fetch-backed readers consume their own worker request receipt; automatic
polls and cancelled panel completions cannot fill a newly opened panel. Refresh
resets the viewport immediately; completion clamps scrolling to the current
document so loading, shorter results and refusals remain visible. `Recent log`
and `Setup capture log` retain their separate historical `LOG_TAIL` readers.
These UI paths are verified with production row input/worker code and explicit
loopback fixtures, not console hardware. The API-only companion does not thereby
gain physical setup support. VDA/assignment rows and general endpoint-freshness
feedback remain separate integration gates.

A native `scePad` sample alone does not identify its physical model or VID:PID.
Unidentified initial-user samples are reported as such and cannot create a
fabricated DualSense rule. Unsupported DS4/PSVR2 requests are blocked rather than
substituted with DS5. Resident backend add/update acquire a matching config-store
policy lease and a source-transition fence, then revalidate source epoch,
ownership, connection and requested output identity before dispatch. Completed
policy changes or observed disconnects prevent stale dispatch; concurrent writers
and source transitions complete after the bounded operation releases its fences.
Liveness queries run before the final fence, so changes during a query are checked
before sending a frame. IPC snapshots do not hold the backend fence and may lag a
config commit until the next resident evaluation; they are not atomic proof of
current game output.

Backend add/update must be bounded and must not mutate config, acquire a nested
writer lease, join config/source mutators or invoke lifecycle/tick operations.
Same-thread ready/lost callbacks are supported. Store lifetime and worker shutdown
follow `gc_service_inject.h`. These contracts and host regressions do not verify a
console backend's compliance. Target support is not inferred from a compiler
macro, symbol presence, source inventory or a saved preference.

USB feed reuse requires the observed VID:PID, decoder kind, interface index and
number, alternate setting and input endpoint to match, not just the device path.
An observed replacement retires the old source and backend before applying the
replacement's rule. A feed also retires when its current descriptor no longer
qualifies for the supported decoder. The separate connection token fences setup
against observed replacements. Neither mechanism identifies an unobserved
same-model unplug/replug or supplies a persistent hardware-instance identity.

For descriptor-qualified USB HID sources, resident output applies the selected
profile's physical `hid_axes` / `hid_buttons` bindings before semantic remapping.
Physical values are cached across report IDs, so an axis report does not release
a held button and a button report does not reset the last axis. Profile edits
re-evaluate that cache without requiring a fresh packet. Source reads retain
original decoded controls and last raw-report bytes; mapped output is separate.
Observed disconnect and service restart clear the physical cache. The first
valid packet for a meaningfully changed layout resets both physical values and
original normalized controls. Padding/unused-storage differences are not layout
changes. Unknown/truncated reports do not partially replace the physical cache.

This path requires a parsed HID descriptor. It does not infer physical field
coordinates for Bluetooth, known-DS4 decoding or arbitrary raw reports, and it
does not establish a typed virtual-device backend or game acceptance.

## Reproducible host gate

From WSL, with the existing UI vendor kit installed:

```sh
export GC_VENDOR_DIR="$HOME/gc-vendor"
export GC_UI_KIT="$GC_VENDOR_DIR/ps5-homebrew-ui"
bash tools/test-controller-stack.sh
```

Build outputs use `$HOME/gc-controller-stack-tests` by default. Override with
`GC_CONTROLLER_STACK_TEST_ROOT` to keep an independent private build root.
The gate covers config/commands, routing, physical inventory/setup, companion
socket/persistence, the production native picker/worker contract and the full UI
host suite. Explicit fixture transports/backends are used; none of these results
prove console output.

To add ordinary target-header object compilation, set the installed
`PS5_PAYLOAD_SDK`, `GC_PORTS_ROOT` and `GC_ROUTING_COMPILE_SDK=1`. This checks six
ordinary source modules with `-Werror`; it does not link a virtual-pad backend,
produce a console ELF, contact a loader or deploy an app.

## Console support boundary

Hardware creation/report/assignment/output behavior for the requested identities
is not verified. A working console release requires an authorized API-only
backend contract and actual console tests, including PSVR2 Sense tracking,
reconnect and operation after the UI closes. The credential-changing legacy
backend is excluded from these gates. Existing binary files are not rebuilt by
this host gate and must not be treated as artifacts of these source changes.

# Wireless DS4 bridge proposal

This is a contribution built on top of Ghostcontrol's existing PS5 pad and
ptrace work. The goal is to let a DualShock 4 that is already paired over
Bluetooth provide input to a native PS5 game, without creating a second VDA
device and without requiring a USB cable.

## What is reused

The contribution keeps Ghostcontrol's existing system-process helpers and
PS5 SDK setup. The new path uses the same `ScePad` data structures and the
same checked process-memory access rules, but it has a different data flow:

1. Attach to `SceRemotePlay` and read the paired controller with
   `scePadReadState`.
2. Copy each normalized `ScePadData` frame to a small loopback receiver.
3. Resolve the current native game's `libScePad` exports and verify the full
   firmware-specific manifest before writing anything.
4. Detour the five normal read entry points to a private snapshot of the DS4
   state. The original bytes and the page protections are saved for removal.
5. Restore the game when it exits or when the bridge is stopped.

The automatic build waits for a single native game, installs the bridge after
the game has finished loading, and then waits for the next game. The one-shot
build is useful while debugging a title.

## Safety and firmware behavior

Firmware 11.60 (`0x11600005`) is the only version currently supported. It is
matched by firmware value, export offsets, 256-byte function hashes, wrapper
bytes, internal targets, controller-information prologue, and a unique type-0
client-table entry. A label such as "11.xx" is not a compatibility guarantee:
private `libScePad` code and data layouts can change in any system update.

Every other firmware fails closed before the first game-process write. The
wireless reader itself uses resolved public APIs in `SceRemotePlay`; it does not
use firmware offsets. The payload still writes a bounded game fingerprint so
another exact manifest can be reviewed and added later, and it never treats
structural similarity as support.

The report is saved under `/data/ds4tod5/` so a tester can send it without a
shell session. It includes the firmware value, resolved offsets, bounded
instruction prefixes, and the reason a candidate was accepted or rejected.

## Build targets

```sh
make wireless-ds4
make wireless-ds4-auto
make wireless-ds4-status
make stop-wireless-ds4
```

`wireless-ds4-auto` is the intended end-user payload. The other targets are
diagnostic and cleanup helpers.

## Current limits

- Only firmware 11.60 has an accepted game manifest.
- The bridge covers the five standard read/data entry points observed on the
  tested titles. A title that uses another private pad path needs a separate,
  reviewed hook.
- DualSense-only output features such as adaptive-trigger effects are not
  translated back to the DualShock 4.
- A stopped session restores the patched entry points. Small injected
  allocations remain in their target processes until those processes exit.

## Review notes

This started as a private experiment using Ghostcontrol's published code. I
am submitting the work here so the implementation can be reviewed, renamed,
split up, or rejected cleanly by the original maintainer. The intent is to
give the Ghostcontrol project the useful wireless-DS4 work, not to maintain a
parallel copy of its code.

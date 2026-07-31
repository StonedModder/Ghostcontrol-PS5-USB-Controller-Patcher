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
3. Find the current native game's `libScePad` imports and verify their actual
   wrapper instructions before writing anything.
4. Detour the five normal read entry points to a private snapshot of the DS4
   state. The original bytes and the page protections are saved for removal.
5. Restore the game when it exits or when the bridge is stopped.

The automatic build waits for a single native game, installs the bridge after
the game has finished loading, and then waits for the next game. The one-shot
build is useful while debugging a title.

## Safety and firmware behavior

The firmware-11.60 manifest is the only one currently verified on hardware.
Unknown firmware is not patched just because a symbol resolves. The generic
path requires all five wrappers, their internal jump targets, the executable
`PT_LOAD` range, padding, and the controller-information prologue to agree.
When any check fails, the payload writes a report and leaves the game alone.

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

## Review notes

This started as a private experiment using Ghostcontrol's published code. I
am submitting the work here so the implementation can be reviewed, renamed,
split up, or rejected cleanly by the original maintainer. The intent is to
give the Ghostcontrol project the useful wireless-DS4 work, not to maintain a
parallel copy of its code.

# User log findings and follow-up tests

The submitted captures show independent USB, Bluetooth and guitar issues. This
change addresses bond persistence and inquiry retry scheduling. It does not claim
a hardware result or a complete guitar mapping.

## Bluetooth

Classic and LE captures report `controllers.db.new` write failures with errno 2,
followed by messages claiming keys were stored. The store now creates missing
parent directories and returns success or failure. Both pairing paths distinguish
persisted keys from keys available only in memory for the current process.

The Xbox LE capture contains 412 inquiry status `0x0c` responses. Although the
host already computed exponential backoff, sending the next inquiry reset it
before its status arrived. Backoff now resets on successful controller acceptance,
so repeated refusals wait 500, 1000, 2000, 4000 and then at most 8000 ms.

`python3 -B tools/tests/test_log_regressions.py` compiles the actual production
store and inquiry routines on a host. It exercises missing and existing parent
directories, record serialization, replacement, an invalid parent, repeated
refusals, the retry cap and successful acceptance. It mocks the radio and clock;
it does not test Bluetooth hardware or physical pairing.

## Guitar: Lemos's beta 2.2.5 with PR #29 and PR #32

The ZIP and RAR contain byte-identical copies of two sessions. Both detect
`12ba:0100`, read its 137-byte HID descriptor and receive 27-byte interrupt
reports. The second session contains eight distinct 27-byte payloads, including
changes in the first button byte. This establishes that changing input reaches
the logger; it does not identify each fret, strum or analog control.

Both sessions explicitly report `VDA probe mode disabled: raw USB logging only`.
The folder named VDA/MBus therefore does not establish successful injection.
The logs also contain constant three-byte `00 00 6e` control replies. Do not
interpret those as complete controller state. Current main already validates
the described fields before applying a HID report.

For the next hardware capture:

1. Use the diagnostic build's documented mechanism to enable its guitar VDA
   probe, and verify that the startup log confirms enablement. The captures name
   `/data/ghostcontrol-app-service/gh5-vpad-probe.enable`; that diagnostic mode is
   not implemented by this PR.
2. Capture a neutral baseline, then each fret, strum direction, whammy and tilt
   separately, with a written sequence and matching timestamps.
3. Record decoded state, virtual-device creation and user-binding results, then
   check actual controls in a game. Raw USB traffic alone is insufficient.

## USB and rumble

Other captures show readable DS4/XUSB input and successful virtual-device binding,
followed by stale-device ioctl failures after disconnect. PR #32 already proposes
USB retirement, transfer-size and control-request fixes; this change avoids
duplicating that work. Re-test unplug/replug with those changes.

Two service rumble commands occur in the DS4 capture. Confirming physical rumble
still requires a real game request, matched controller handle, relay return code
and tester observation. No such hardware confirmation is claimed here.

Original logs are not included: they contain tester and console identifiers.

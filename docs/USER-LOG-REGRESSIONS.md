# User log findings and follow-up tests

The submitted captures show independent USB, Bluetooth and guitar issues. This
change addresses bond persistence and inquiry retry scheduling. It does not claim
a hardware result or a complete guitar mapping.

## Bluetooth

Classic and LE captures report `controllers.db.new` write failures with errno 2,
followed by messages claiming keys were stored. The store now creates missing
parent directories and returns success or failure. Both pairing paths distinguish
persisted keys from keys available only in memory for the current process.

Explicit forget now checks that result too: the host returns failure and logs
that the controller was forgotten only in memory when the rewrite fails. The
old disk record remains and may return after restart. The service's asynchronous
request acknowledgement is not proof that the host persisted the operation.

The Xbox LE capture contains 412 inquiry status `0x0c` responses. Although the
host already computed exponential backoff, sending the next inquiry reset it
before its status arrived. Backoff now resets on successful controller acceptance,
so repeated refusals wait 500, 1000, 2000, 4000 and then at most 8000 ms.

`python3 -B tools/tests/test_log_regressions.py` compiles the actual production
store and inquiry routines on a host. It exercises missing and existing parent
directories, record serialization, replacement, an invalid parent, successful
and failed forgets, reloads and retry after a blocked rewrite. It also checks
unknown/closing requests, link identity selection, repeated inquiry refusals,
the retry cap and successful acceptance. It mocks the radio and clock; it does
not test Bluetooth hardware or physical pairing.

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

## Follow-up: test1 and Experimental test 2

The new archive contains an app-service log and a vpad log for each session.
The supplied description says the Experimental variant includes PRs 29, 32 and
33 plus their review fixes; its source commit/artifact and console firmware were
not supplied, so that composition cannot be independently confirmed.

- Both vpad logs contain 24 logged successful completions with 27-byte input
  reports from `12ba:0100`. These are logged samples, not a throughput count or
  proof of complete guitar mapping.
- Test 2 contains 54 sampled EBUSY diagnostic lines, with the sample counter
  reaching 2432. The recorded tuple stays fd 12, interface 0, endpoint 81,
  poll flags `0x41`, one requested frame, zero completed frames, requested/current
  length 64 and endpoint status zero. A failed completion's metadata is not a
  successful endpoint result. No timing, transfer-size or recovery change follows
  from these snapshots alone.
- Both sessions record `AddDevice 0x803b0006`, then zero kernel-log bytes, lines,
  MBus events and candidates during identification. No binding success is
  recorded. The code is preserved as an observed return value, not assigned an
  unverified error meaning. The decimal parser cannot match events never read.
- Both show a disconnected handle closing after ENOTTY and the receiver opening
  again. Successful post-reconnect input/binding is not established.
- No `040b:0897` device identifier appears in these files. Its open-failure
  rate limiting and continued open attempts are not verified by this archive.

PR 29 adds checked nonblocking kernel-log setup and per-identification transport
diagnostics (AddDevice result, bytes, lines, device events, candidates, EOF and
read errno), without changing USB transfers or uncertain-device ownership.

For the next test, provide the Experimental source commit and artifact hash,
console model/firmware/loader, kernel-log provider/configuration, and whether
the Toolbox was open. Capture the new identification summary. Establish log
delivery and virtual-device binding before evaluating labeled fret/strum/whammy
inputs in a game. Capture successful/error completion totals and last successful
input times across unplug/replug; sampled failures alone cannot establish a stall.
Include a separate `040b:0897` session to test its retry/log rate behavior.

The EBUSY root cause and guitar gameplay remain NOT VERIFIED. Raw logs are not
committed. The Experimental instrumented source is still needed before porting
or modifying that instrumentation.

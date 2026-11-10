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

## Additional capture: Cosmic Byte Blitz with inactive controls

The separate vpad file has four sessions (timestamp resets). The physical pad
is reported as a Cosmic Byte Blitz; the captured USB identity is `057e:2009`
and the traffic completes Nintendo handshakes and reaches `0x30` reports. The
tester reports a blocked profile-selection screen and no working buttons.
Which screen, exact build and selected controller mode remain unconfirmed.

Each session records `AddDevice 0x803b0006` followed by binding return zero.
Therefore the AddDevice value alone must not be labeled a creation failure.
Binding is still not proof that buttons work. The final session records
post-disconnect FS_START errno 25; this does not establish successful recovery.
Descriptor reads also fail with errno 5 in the capture. Compare the exact build
against PR 32's cache/retirement work before assigning those failures a new cause.

PR 29 reproduces and fixes premature input publication: protocol handshakes and
rejected known-protocol packets must not use the generic HID neutral-learning
fallback. In the capture a virtual pad is announced after the first handshake,
before its first `0x30` control report. The fix retains generic HID learning and
valid Nintendo control decoding, with host regression coverage. It does not
establish that this defect explains all of the tester's inactive controls.

Retest the updated combined build, identify the blocked screen, and capture
timestamped neutral/button press/release payloads, decoded button values,
InsertData results and the selected user. Check startup, hotplug and reconnect,
plus generic HID custom setup. Keep the controller's documented mode fixed for
each session and record its observed identity. No guessed mapping, forced mode
change or USB-transfer change is justified by the current report-ID-only log.
Controller gameplay, the complete profile-selection failure and EBUSY remain
NOT VERIFIED.

## Additional Bluetooth capture: pairing remains visible at user selection

The two supplied app-service files are byte-identical; count them as one capture.
The vpad file includes a timestamp reset and multiple pairing/retry attempts.
The LE controller reports `045e:0b22`, reaches ready-as-Xbox state and binds a
virtual pad successfully in four recorded attempts. The tester says input works
after turning something off, but whether that means search, the Toolbox or the
controller's physical pairing mode has not been established.

The trace has 412 refused inquiry status responses, repeated bond-store errno 2
failures followed by misleading keys-stored messages, and a failed forget rewrite
followed by a success message. Those are precisely behaviors the existing PR 33
changes address. This capture does not verify those changes on console; establish
the exact source/build before treating it as a regression in the updated PR.
Pairing timeouts and encryption failures are also recorded; no single cause is
assigned to them from these logs.

The related code audit found a further retry gap: a command-send failure has no
Command Status event to schedule backoff, so the polling loop could retry every
tick. Failed sends now schedule the same 500 ms to 8 s capped backoff as refused
commands. A regression reproduces the missing delay, then checks growth, cap and
reset only after controller acceptance. This is a tested error-path fix, not a
claim that send failures caused this tester's reported pairing/user-picker issue.

Retest a build including the current PR 33 storage and retry fixes. Record the
exact time and meaning of the action that restores input. Compare search active
versus expired and Toolbox open versus closed in separate sessions, keeping the
controller mode fixed. Record pair completion, first input, bind and InsertData
results and the selected user. Do not automatically stop discovery on readiness:
the search window can support additional controllers, and a ready link is not
proof that the physical controller has left pairing mode. Input behavior at user
selection and the reported workaround remain NOT VERIFIED by these captures.

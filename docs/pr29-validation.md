# PR 29 validation

## Automated checks

Run `python3 -B tools/tests/test_virtual_device.py` and
`python3 -B tools/tests/test_ds4_bridge.py` on a host with Python 3 and a C compiler.
Run `python3 -B tools/tests/test_vpad_log_source.py` for the production kernel-log
reader. It reproduces unchecked nonblocking setup, then checks setup failures,
bounded reads, EOF/read errors, matching versus absent/unrelated events, and
descriptor-close uncertainty. These checks run in the parser workflow too.
The latter compiles the actual source selector, resident reader, bridge relay,
and game vibration stub against host fakes, with warnings treated as errors.
It covers startup and hotplug selection for connection types 0, 1, 2 and 255,
controller-info failure and incomplete output, changed/unchanged motor values,
native vibration errors, unmatched handles, inactive hooks, failed-delivery retry,
stop commands, game teardown and a writer racing the rumble snapshot read.

The host kernel-header shim declares two SDK calls; unused console paths are
removed by the linker. These tests do not validate console ABI, injected code
relocation, USB driver behavior or physical motor output. No full console build
or hardware test was performed in this follow-up; the payload SDK is unavailable
in this environment.

## Console merge gate

Before merging, validate the following with a USB DS4 and a wireless DS4:

1. Confirm the intended DS4 handle in SceRemotePlay reports controller-information
   byte 10 (`ConnectionType`) as 2. Rejected DS4 handles log their observed value.
   If the intended handle reports another value, investigate the transport
   contract before changing the filter; accepting local handles reintroduces
   duplicate ownership with PR 32.
2. Connect a USB DS4 before startup and again after startup. Confirm it is handled
   only by the USB service and is never selected by the RemotePlay bridge.
3. Connect the intended wireless DS4 before startup and during hotplug. Confirm
   each is selected once, game input works, and user/pad routes stay correct.
4. Exercise large/small motors and zero commands from a game. Confirm physical
   rumble starts and stops. Native API success alone does not establish this.
5. Exit the game, disable the bridge, change routes and suspend/resume while
   vibrating. Confirm motors stop and controller ownership recovers.
6. Unplug the USB DS4 during reads. Confirm ENOTTY closes the dead feed and
   interrupt transfers resume correctly after reconnection.

## Guitar follow-up captures

Both new sessions record successful 27-byte input reports for `12ba:0100`, but
also `AddDevice 0x803b0006` and an identification window with zero kernel-log
bytes/candidates. The Experimental session samples EBUSY with ready flags
`0x41`, no completed frames and the requested length still 64. Those are
snapshots after a failed completion call, not a successful endpoint result.

The reader now refuses a TCP socket whose nonblocking setup failed, and reports
the AddDevice result alongside byte/line/event counts, EOF and read errno. A
zero-byte window is distinguished from events that fail to match. Identification
still waits and preserves uncertain ownership: the AddDevice return alone is
not used to guess a handle, delete a pad, grant credentials or force a retry.
The USB transfer mechanism is unchanged.

For the next console run, record firmware/loader, the exact Experimental source
commit and artifact hash, kernel-log provider/configuration, and whether the
Toolbox was open. Capture the new identification summary and verify delivery of
kernel-log events before blaming the parser. Check binding and labeled guitar
controls separately. The supplied logs do not establish firmware, build identity,
physical mapping or gameplay success. The EBUSY cause remains unresolved.

## Switch-protocol input gate: Cosmic Byte Blitz report

The additional vpad log contains four sessions identifying as `057e:2009`:
Nintendo handshakes, `0x30` input packets and a successful bind in each session.
The tester identifies the physical pad as a Cosmic Byte Blitz and reports no
working buttons at a profile-selection screen. The exact screen and selected
controller mode remain unconfirmed; a USB identity does not establish the
physical model or its currently selected XInput mode.

In these sessions `AddDevice 0x803b0006` precedes successful binding, so that
return value alone is not proof that creation failed. This is distinct from the
guitar's zero-byte identification windows.

A host regression reproduces a concrete input-gating defect: `usb_decode`
promoted Nintendo handshake packets (handler result zero) to neutral, readable
input through the generic HID learner fallback. The capture likewise announces
a virtual pad after the first `0x81` handshake, before the first `0x30` report.
The fallback now applies only to generic HID learning. Nintendo handshakes,
protocol errors and rejected DS4/XInput packets do not publish fake control
samples or erase the cached decoded state. Actual decoded input remains valid.

`python3 -B tools/tests/test_usb_input_gate.py` compiles the production USB decode
routine, Nintendo handler and Nintendo parsers, mocking transport/profile/HID
dependencies. It checks handshake progress without publication, valid button
press/release, truncated packets, OUT failure, rejected/accepted DS4/XInput
parser results and retained generic HID learning. This runs in CI. It is not a
replay of button bytes from the tester: the capture logs report IDs and lengths,
not the control payload needed to establish this pad's mapping.

Retest with the exact updated build and identify whether the blocked screen is
the PS5 user picker or Ghostcontrol's profile page. Record neutral/press/release
raw bytes, decoded buttons, InsertData results and user routing. Test startup
and hotplug, including an already-held button, then release and unplug/replug.
Check a generic HID controller's custom setup still works. Do not force a VID/PID
rename, XInput parser or guessed mapping. This fixes premature publication but
does not establish resolution of all inactive controls, reconnect faults or EBUSY.

## GH5 fallback corrections from sdlemos's new captures

Console testing, diagnostic captures and the follow-up findings were provided by
[sdlemos](https://github.com/sdlemos), reporting PS5 firmware 11.60 and the original
PS3 GH5 receiver `12ba:0100`. The supplied archive has four logs, not source or an
ELF. Per-generation final counters sum to 3,313 and 2,494 successful completions.
Each session contains 256 RAW samples spanning about six seconds, including
105/109 short control replies. VDA probing is explicitly disabled. These captures
establish continued USB delivery, not a cause for EBUSY or working gameplay.

The fallback now uses recent valid interrupt-IN reception independently of
decoded/publication state. A one-second quiet window permits fallback again;
initial fallback retains its six-tick grace and retries are at least 250 ms
apart. Per-connection state tracks interrupt reception, control attempts,
transport successes/failures and timestamps. Feed open/close resets that state,
so successful polling on one connection cannot suppress another in the same slot.
PR 32's class/interface GET_REPORT request and all-zero reply safeguard are kept.
Control replies decode separately instead of overwriting an outstanding interrupt
transfer's buffer or requested length. Interrupt START/poll/COMPLETE is unchanged.

`python3 -B tools/tests/test_usb_fallback.py` executes the production fallback
block with mocked clock/control I/O. It reproduces redundant polling during
RAW-only reception before the fix, and checks quiet-pipe fallback, bounded
failed retries, success/failure counters, replacement connection state, expiry
of recent traffic and preservation of an in-flight interrupt buffer.

The Experimental-only decode counters, bounded RAW sampler and marker-controlled
VDA mode are not in the public tree or supplied archive. Their corrections and
preservation of the local db_forget rollback require the corresponding source.
Do not label probe-disabled skips as decode failures, neutral probes as decoded
controls, or requested ELF/source ZIP variants as reproduced without that tree.
Record exact combined source and ELF hashes, then test RAW/probe-OFF and VDA/probe-ON
in separate sessions. Hardware behavior of these new fallback changes is NOT VERIFIED.


## 2026-11-11 Diagnostic / Precision follow-up

[sdlemos](https://github.com/sdlemos) supplied the console tests, four logs and
GH5 developer report. The new archive SHA-256 is
`1442ffea72c342e6c998ed5897afb067a4b2b011bcff6c73e6a3c52372166464`.
It contains logs only, not the executed ELF or matching source. Diagnostic has
256 RAW records; Precision has 660, all 27-byte interrupt-IN samples, including
436 after reconnect. Precision's latest per-generation completion counts sum
to 2,650, with zero GET_REPORT attempts. Both sessions explicitly disable VDA.
This is consistent with suppression during active interrupt traffic; it does
not exercise the quiet-pipe fallback, prove the exact integrated revision,
resolve EBUSY or establish guitar mapping/binding/gameplay.

Reviewing that untested fallback path exposed a host-reproducible defect:
`control_decoded` permanently stopped polling after the first successful
control decode. A control-only device could publish a press without ever
publishing its subsequent release. The flag is now diagnostic state only;
quiet pipes continue polling at the existing minimum 250 ms interval, and
recent interrupt input still suppresses requests for one second. The production
fallback-block regression fails before the fix at the missing release, then
passes with a second publication, retry bounds and renewed-interrupt suppression.
The decoder is mocked: this does not validate GH5's report mapping or classify
short control replies as real guitar input. Existing zero-reply filtering and
interrupt buffer/START/poll/COMPLETE ownership are unchanged.

The initial virtual-pad message now says "waiting for first readable input"
rather than presenting the initial state as an established input failure.
Rollback: revert this follow-up commit. Risk: control-only devices now receive
continued bounded requests instead of one-shot polling; test that behavior on
a controlled device, then repeat after reconnect. PS5 runtime is NOT VERIFIED.

Next console evidence remains a separate Precision VDA-ON capture from the
build that implements the marker, with exact ELF hash/build revision and
AddDevice → kernel-log/MBus discovery → binding/user → InsertData results.
Record timed individual fret/strum/whammy/tilt actions for replay fixtures;
neutral-probe success alone does not establish a guitar mapping.


## Control-report publication and completion ownership follow-up

The remaining mapping/binding gaps prompted two further production-code fixes:

- A rejected HID control reply could enter the generic raw-learning path and
  publish neutral input; a saved profile could also convert rejection into
  success without decoding that reply. Control-origin reports now require
  decoder acceptance before profile application/publication. Interrupt input
  retains setup learning and raw profiles. Valid known-protocol control data
  remains accepted. This intentionally withholds undecodable control-only
  devices; an explicit decoder is needed rather than inventing usable controls.
- Completion index 1 (the Nintendo OUT transfer) previously cleared IN's busy
  flag and could examine IN's endpoint status before checking ownership. The
  reader now checks transfer index first. Only completion index 0 releases IN
  and validates its status/frames/length. EBUSY still retains IN ownership.

Both defects were reproduced by failing host regressions before their fixes.
`test_usb_input_gate.py` covers control rejection with/without a saved profile,
cached-state preservation, interrupt learning and accepted control data.
`test_usb_completion.py` executes the production completion block with fake
I/O, covering OUT completion, stale IN status, EBUSY, invalid frames/lengths
and valid IN completion; it runs in CI. These tests do not emulate the PS5 USB
kernel or provide a GH5 control mapping. Completion ownership is a preventive
fix, not an established explanation for the GH5 EBUSY observations.

Console evidence remains credited to sdlemos. The report's zero-GET_REPORT
Precision capture does not exercise the control-rejection defect; no claim is
made that either defect caused that captured session's behavior. VDA was off,
so no VDA success/failure change or gameplay fix can be established from it.
No new guitar-byte mapping, VDA marker or private Precision instrumentation is
introduced. Revert this follow-up commit to roll back; review control-only HID
compatibility and Nintendo IN/OUT traffic on hardware before release.


## Source-backed PS3 Guitar Hero default button mapping

The 2026-11-11 Precision capture supplied by sdlemos contains a 137-byte HID
descriptor with 18 semantic fields, 16 raw fields and no report ID. Its first
13 fields are Button 1–13. Generic decoding maps Button 1/2/3 to
Cross/Circle/Square. Published PS3 Guitar Hero research instead identifies them
as yellow/Square, green/Cross, red/Circle. Blue/Triangle and orange/L1 already
match the generic mapping.

References (protocol facts, independently implemented here):
- https://github.com/TheNathannator/PlasticBand/blob/main/Docs/Instruments/5-Fret%20Guitar/Guitar%20Hero/PS3.md
- https://santroller.github.io/guitar-configurator/controller-reverse-engineering/ps3-gh-guitar.html

`gc_hid_set_device` selects the default button mapping only for 12ba:0100 after
USB descriptor parsing. It leaves HID usages/offsets unchanged; semantic input,
button arrays and removal of default mappings by custom HID profiles share the
same button-mask helper. Other devices keep their existing mapping. Reusing a
layout for another VID/PID clears the mapping flag. Service layout comparisons
include the mapping flag so cached controls are reset if it changes. Both the
resident and app-service USB readers consume this enumerated layout.

`tools/tests/fixtures/gh5-hid.json` includes only the captured descriptor and two
input records, with archive hash and original line numbers. Full logs and user
identifiers are not committed. The regression compiles real gc_hid.c and
gc_profile.c, reproduces the wrong first-fret output before the fix, and tests
individual frets, chords/releases, strum, neutral hat values 0x0f/0x1f, a
truncated report, custom override and unrelated-device isolation. Captured
frames are replayed against documented protocol semantics; no physical action
diary was supplied, so their meaning is not independently hardware-verified.

This corrects the default face-button mapping, not virtual instrument identity.
It adds no tilt threshold, motion calibration, instrument emulation or PS5 game
compatibility claim. Existing custom profiles may have compensated for the old
button ordering and need review. VDA binding/InsertData and gameplay still need
PS5 retesting. Rollback: revert this mapping follow-up commit.

FreeBSD 11.0's USB_FS_COMPLETE implementation returns EBUSY when its completion
queue has no record; this supports retaining in-flight ownership rather than
restarting on EBUSY. It does not establish identical PS5 behavior:
https://github.com/freebsd/freebsd-src/blob/releng/11.0/sys/dev/usb/usb_generic.c
The captures' poll_revents=0x41 includes read readiness, so simply removing write
readiness is not demonstrated to fix their EBUSY pattern. No such change is made.


## Virtual-pad creation preflight and accepted-input evidence

The GH5 path still depends on AddDevice → kernel-log identification → MBus bind
→ InsertData. Source review reproduced AddDevice being called even when its
new kernel-log source had already reached EOF or a read error. A bounded drain
could also leave old queued data behind for the new identification window.
Creation now requires a drain to EAGAIN and a complete line boundary; a dead,
undrained or partial stream is closed with a diagnostic before AddDevice.
EINTR is retried within the existing bounded read budget. This prevents a known
unusable identification source from creating an unidentifiable pad; it does
not prove that a quiet/live server will deliver the future DEVICE_ADDED event.
Busy or partial streams may withhold creation and require retry/reconnect.

The production startup/reader regression failed before the fix because an EOF
source still called AddDevice. It now covers EOF, hard read failure, backlog
budget exhaustion, partial lines and a healthy empty stream that creates once.
Unknown/ambiguous-device handling and identifier ownership remain unchanged.

The first successful InsertData call now logs one per-device confirmation with
slot/device/handle/user. Rejection/recovery logging remains; repeated successes
do not flood the log. An API-accepted report is still not proof that a game
consumed it. `test_vpad_injection.py` executes the production payload constructor
and API-call path with a fake console API. It checks the host 120-byte layout,
press/release and axes, pending/invalid-slot gating, refusal recovery and
per-device confirmation reset. This does not verify the PS5 ABI on hardware.

Keep credit to sdlemos for the console captures. Newest VDA-disabled captures
cannot exercise this fix; no executed ELF/source identity is available. No
private marker behavior, firmware patch, instrument identity or tilt calibration
is invented. Retest a known build with VDA enabled, exact ELF hash, recorded
physical controls and the complete binding/insertion logs. No full local app
build or PS5 execution was performed. Rollback: revert this follow-up commit.


## Bounded EBUSY progress investigation

GH5 receiver 12ba:0100 now gets `usb progress:` summaries at most once per
second while its feed is serviced. Other devices do not get this extra output.
The per-feed state resets at open/close and receives a monotonically increasing
connection generation, so reused fd/slot values cannot join unrelated sessions.
Summaries distinguish USB endpoint_address (for example 0x81) from transfer_index
(0), and record successful IN starts/completions, EBUSY returns, other completion
ioctl errors, valid input, interrupt decoder acceptance/rejection, poll readiness,
GET_REPORT attempts and ages since completion/input/outstanding START. A missing
completion/input age is -1. Completion metadata is not read on a failed ioctl.
A completion count includes endpoint-error completions; valid-input counts are
separate. Decoder counters describe interrupt data, not control fallback replies.

`phase=possible-stall` means an outstanding transfer is at least 1 second old
and no successful IN completion has been observed for at least 1 second. This
is an observation threshold, not a validated hardware timeout or kernel error.
An intentionally quiet/event-driven device could meet it. The next sampled
non-suspect state is labelled progress-resumed. Samples can miss short episodes;
last snapshots are lower bounds, not final lifetime totals or maximum stall ages.
No automatic restart, close, USB request change or warning suppression is added.

Tests simulate many EBUSY returns interleaved with 18-ms progress, no-progress
pending state, recovery, bounded sample output, missing ages and connection reset.
The production logger is compiled with printf-format checks. The production
completion test checks EBUSY and OUT completions retain pending IN ownership.

Controlled PS5 retest (console testing/captures remain credited to sdlemos):
1. Record exact source/combined revision, executed ELF SHA-256, PS5 model,
   firmware 11.60/loader version and variant. Ensure one service owns the receiver.
   The log-only archive does not establish that its private Precision build
   contains this public change; integrate it into that build explicitly if used.
2. Start with Toolbox closed and VDA disabled; capture 10 seconds idle, each
   fret/strum press and release with timestamps, then unplug/replug and repeat.
   Preserve complete vpad/app-service logs and every connection generation.
3. EBUSY rising alongside fresh completions/input demonstrates progress rather
   than a permanent USB stall. If possible-stall persists while actively strumming,
   inspect outstanding age and whether completions or only decoding stopped.
4. Repeat the same sequence with VDA enabled in the marker-capable build, changing
   only that variable. Capture AddDevice, kernel-log identification, binding and
   first InsertData acceptance. A healthy USB stream with no binding is a separate
   virtual-pad issue; accepted insertion does not establish game recognition.
5. Compare the same known build/configuration before changing USB behavior. Use
   the recorded failing transition to justify the next fix and hardware regression.

FreeBSD reference (not proof of identical PS5 implementation):
https://github.com/freebsd/freebsd-src/blob/releng/11.0/sys/dev/usb/usb_generic.c
USB_FS_COMPLETE returns EBUSY when no queued completion is available. The
captures' successful input between EBUSY returns does not establish the exact
PS5 cause, competing-reader ownership or working gameplay. Hardware behavior of
this instrumentation remains NOT VERIFIED. Rollback: revert this follow-up commit.

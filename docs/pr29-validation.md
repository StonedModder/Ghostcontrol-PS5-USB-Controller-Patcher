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

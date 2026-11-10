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

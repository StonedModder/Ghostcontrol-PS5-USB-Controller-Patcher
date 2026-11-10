# Controller service

## Current native integration

`ui/Makefile` uses the pinned homebrew UI-kit driver through
`tools/compose-kit-root.sh`; the separate service is built by
`GhosttoothAPI/Makefile` / `tools/build-app-service.sh native`, not OpenProspero.
The baseline default is the full resident. The separately selected additive
diagnostic excludes the virtual-game backend while retaining UI/profiles/link
and app-owned feedback; it must be built from its exact reviewed target, not
inferred from the historical ordinary source list below. See
[PC-READY-DIAGNOSTIC.md](PC-READY-DIAGNOSTIC.md).

Packaged asset: `/app0/assets/ghostcontrol/GhosttoothAPI.elf` plus
`GhosttoothAPI.manifest.json` and every reviewed notice. The compiled app and
service default to port 39072. The app discovers the console LAN address;
loader port 9021 transfers the ELF directly, without an OpenProspero daemon or
spawn path. Local-peer restrictions and qualified STATUS readiness still apply.
The kit emits a title folder/ZIP with generated `sce_module/libc.prx`; historical
OpenProspero PKG instructions and import receipts are not current build proof.
No build operation deploys or proves console behavior. Real notice and source
closure is tracked in [release-notices/SOURCE-AVAILABILITY.md](release-notices/SOURCE-AVAILABILITY.md).

## Historical ordinary-candidate reference

The remainder describes the earlier compile-only ordinary candidate and its
retained tests/contracts, not the current default resident's source/import
closure or reproducible native commands. In particular the no-credential/no
explicit kernel_sys paragraph and eight-file source list below are scoped to
that historical target, not current GhosttoothAPI. `build-app-service.sh native`
now uses the stock SDK wrapper, so the historical reproduction commands/provider
failure notes are not the current driver's result. Existing literal checklist
items and their acceptance are unchanged.

# Normal app companion service candidate

## Status and import-audit blocker

This is a **separate explicit compile-only candidate**, not an extension of `gc_main.c`, and not an approved console deployment. Host integration runs the real `gc_ipc.c` server and real UI `gc_client.c` over loopback, plus production config/profile persistence. It passes with synthetic app observations/API-result ACKs explicitly marked **test-only**. Those fixtures do not demonstrate console output.

The SDK produces a real ELF, but the literal **no kernel-library imports** requirement is **not met**: its ordinary POSIX wrappers add `libkernel_web.sprx`. The observed dynamic providers are exactly:

- `libkernel_web.sprx` (ordinary SDK POSIX/thread/file provider)
- `libSceLibcInternal.sprx`
- `libSceNet.sprx`

There is no explicit `-lkernel_sys`, credential API, pad API, process-memory API, ptrace, remote worker, game/ShellUI injection, loader/firmware probe, or legacy startup in this target. The symbol audit has an explicit normal-symbol allowlist and rejects any unreviewed import. The SDK's transitive `sysctl` call was inspected in `arc4_stir`: it uses `CTL_KERN/KERN_ARND` for ordinary entropy, not a firmware/loader query. It is disclosed, not hidden or interpreted as proof of meeting a zero-kernel-library condition.

**`build-app-service.sh native` and `all` deliberately exit 1 at the final strict provider gate**, after writing the ELF, audit reports, byte count and SHA-256. Do not change that gate to success without resolving the requirement/toolchain conflict. `host` returns success for passing tests. Parent review must decide whether normal POSIX provider libraries are allowed or supply an alternative documented normal runtime. No privileged substitute or unsafe fallback is offered.

## Target boundary

Only these sources belong in an eventual parent-owned explicit target:

```
core/src/gc_app_service.c
core/src/gc_runtime_commands.c
core/src/gc_app_pad.c
core/src/gc_config.c
core/src/gc_profile.c
core/src/gc_ini.c
core/src/gc_ipc.c
core/src/gc_log_tail.c
```

Libraries explicitly linked: json-c, pthread and libm; SDK ordinary runtime providers are audited separately. Do not add this main to the legacy core source list. Tests define `GC_APP_SERVICE_NO_MAIN`. The integration fixture declares the opaque `GcAppService` open/close seam without adding a public header or changing the frozen pad header.

Build and test artifacts are private to `/home/red/gc-usability-app-service`. The build script does not edit Makefiles, title IDs, UI assets or existing modules and never deploys anything. Parent owns target/Makefile/UI integration. Native pad owner changes are a separate child task.

## Lifecycle, files and ownership

The console-default private store is `/data/ghostcontrol-app-service`, **not** the legacy `/data/ghostcontrol` store. A normal manual process invocation can select `--root PRIVATE_DIRECTORY` and optionally `--port 1024..65535`. With no port override it uses persisted `ipc_port`. Root selection is not a migration or takeover mechanism; use a dedicated directory with an existing parent. The host tests always use fresh directories beneath `/home/red/gc-usability-app-service`.

A nonblocking ordinary `flock` on `app-service.lock` is acquired **before** config/token/log mutation. Lock errors or busy ownership refuse startup. No PID guessing, process discovery, signal to another process, eviction, alternate lock, busy-lock bypass or lock-file unlink occurs. The lock stays held until the IPC worker has joined and config/log resources are closed. A lock filesystem not supporting this ordinary API is a startup failure, not grounds to bypass it. Config and IPC helpers retain their own path/file safety checks. The lock and append-only log reject symlinks/nonregular/multiple-link files.

`GcAppPad` has one owner and every operation is serialized by the service mutex, including config snapshot synchronization, status/expiry, scheduling, poll and ACK. Boot identity comes from ordinary `/dev/urandom`; missing entropy or monotonic clock fails startup. No fabricated fallback identity is used.

`autostart` controls the **logical running preference when manually starting this ELF**; it does not install a boot task. IPC remains available when logically stopped. START/STOP/RESTART operate only this service's logical output state, not another process or the console loader. RELOAD reloads the real store and synchronizes readback without implicitly starting a stopped service. Shutdown joins IPC before releasing resources. The standalone main responds to signals received by itself; it never sends a signal.

Logs are ordinary append-only `app-service.log`, command names only (not raw untrusted arguments). LOG_TAIL reads a bounded tail using the existing formatter and returns actual bytes in hex. It never reads klog. Config/runtime/profile helpers perform the real writes and reloads; after each successful mutation the effective pad settings and service snapshot are synchronized under the same mutex. Effect step 0..15 remains exactly persisted/readable but the app's minimum normal frame interval is 16ms, reported separately as `effective_step_ms`.

## Commands and truthful results

STATUS identifies `service_kind="app-native"`. Its nested `capabilities` has authoritative booleans for page gating: `native_pad=true`, `profiles=true`, `setup=false`, `bluetooth=false`, `controller_rules=false`, `update_check=false`, `boot_installer=false`, `background_output=false`, `usb_vda=false`. The same capability object is returned by RUNTIME_GET. Controller-rule operations are not implemented by this candidate, so the parent must disable that page's commands rather than infer support from saved rules. Tests assert these names/types/values, STOP's durable event cancellation and no generation/value mutation after rejected RUNTIME_SET.

| Commands | Behaviour |
|---|---|
| STATUS | Compatible schema/version/pid/started_ms/service/error/config_generation, empty VDA slots, native app observations kept separate, explicit capability object |
| PING | Real loopback liveness |
| RUNTIME_GET / RUNTIME_SET | Existing validated helper and durable store; screen field names retained, capabilities adjusted to this candidate; unsupported preferences labelled persisted-only |
| START / STOP / RESTART / RELOAD | Actual logical service lifecycle/config reload; output requires app poll and ACK |
| APP_PAD_POLL / APP_PAD_ACK | Exactly the frozen `gc_app_pad.h` functions and six/twenty/eleven-field ABI; no handle crosses IPC |
| DUALSENSE_FX_STATUS | Configured values, current app connectivity/caps, boot/revision/event state, actual acknowledged return codes, applied status conditioned on a live app lease |
| DUALSENSE_RUMBLE | `large\|small\|on_ms\|off_ms\|repeats`; finite bounds enforced by pad state; retained pending event, busy rejected |
| DUALSENSE_RUMBLE_STOP | Cancels the retained event and publishes stop; never claims the motors are physically stopped before app evidence |
| PROFILES / PROFILE_GET | Actual profile inventory and hex-encoded serialized INI, including disabled profiles |
| PROFILE_CREATE / EDIT / RENAME / IMPORT / EXPORT / FILES | Real bounded existing config/profile helpers; edit `id\|key\|value`, rename `old\|new`, import/export ID-based private exchange files |
| LOG_TAIL | Actual ordinary log tail, validated count, bounded hex encoding |

Effect IDs follow legacy screen configuration: **0 solid, 1 breathe, 2 rainbow, 3 police, 4 strobe**. The native-pad module update must accept all five and treat enabled as a **lightbar-only preference**. Rumble is independent of that preference, but always gated by this service's running state. STOP cancels rumble and makes the effective lightbar preference false without overwriting saved preferences; next poll conveys stop/reset intent. The app must remain polling to apply it; after app exit/disconnect it independently expires its lease and performs bounded cleanup. Failed physical cleanup is not silently reported as success.

Rumble limits are those of `gc_app_pad_schedule`: motors 0..255, not both zero; on 1..2000ms, off 0..2000ms, repeats 1..16; finite total <=10000ms. Repeats zero/infinite is unsupported. Scheduling can await a connected app only until the state-module acceptance TTL. A successful command means **accepted/pending**, not applied. Repeated requests cannot replace a live event. Config changes invalidate old light ACK revisions; stale sessions/revisions/leases cannot establish applied output.

`DUALSENSE_FX_STATUS.applied` is true only for a currently acknowledged configured lightbar or a live acknowledged rumble start, with current app connectivity/capabilities and a running service. Completion retains event/result evidence but does not claim currently active motors. No animation RGB, write-count, reset-count, gyro/quaternion/gravity values are available on the frozen wire; they are not invented. `current_rgb_available=false`, `motion_valid=false`, explicit signed NOT_CALLED values and ACK fields convey the limitation. Parent screens should show unavailable telemetry rather than interpreting missing counters as measurements.

Saved profile/controller rules are configuration, **not physical inventory**. Native controllers are counted only from current app observation, never from a saved rule or service VDA slot. `slots=[]` means this candidate has no VDA inventory; it says nothing about native hardware.

Unsupported commands return `ok:false`, `applied:false` and explicit error codes. Capability flags explicitly disable USB/VDA/kernel/game bridges, remote motion, wireless DS4/GhostPad output, unavailable Bluetooth-host pairing, boot installer, output after app exit, update execution and console ID. CONSOLE_ID is unsupported because no documented ordinary ID API is supplied; the legacy probing helper is not linked. DEVICES/CONTROLLERS return unsupported inventory errors, not a fabricated hardware list. USB setup/controller operations, force/rebind, update, Bluetooth and boot commands never claim success. Unknown commands and malformed settings/profile edits/pad bodies are rejected.

## App-side companion lifecycle (APP-SVC-02)

`ui/src/service_lifecycle.{hpp,cpp}` is the single owner of starting the companion from the app. The frame never probes, reads the package or pushes; it queues a press (`request_start`) or hands over the app-private preference view (`observe_consent`) and copies a `View`.

- **Strict identity.** Only a STATUS reply that is one valid JSON object with top-level `ok:true`, `schema:1`, `service_kind:"app-native"`, exact reviewed `version:"2.0.0-app-service-candidate"`, positive plain-decimal `pid` in the producer's signed 32-bit range, positive plain-decimal uint64 `started_ms`, nonzero plain-decimal uint64 `boot` and `listen_port` equal to the app's client port is compatible. Unknown versions fail closed until separately qualified. Required process fields are validated even when the caller requests no identity output; none is defaulted to zero. `{}`, empty, malformed, `ok:false`, foreign products, incomplete instances or another port never count. `boot` (per-lifetime random) is the freshness identity, not PID. This is compatibility checking, not cryptographic endpoint authentication. A resident whose controller work is `stopped` still owns its IPC listener and suppresses another launch; no absent listening/protocol field is invented.
- **Probe before send.** Connection refused is the only "absent". A compatible answer suppresses the push (`already_running`). An incompatible answer (`identity_mismatch`), a stalled/overflowing port (`unknown_liveness`) or a route failure (`unreachable`) also suppress it: nothing is pushed over a port that something else may own. The package is validated (`ServiceLauncher::prepare`) and STATUS is probed again immediately before `submit`.
- **Readiness.** After the push the worker polls STATUS itself until a compatible instance answers (`ready`) or the budget (12 s default) ends. Silence is `start_unconfirmed`: the ELF may still be starting, and nothing is retried automatically.
- **One automatic attempt.** The first settled preference view decides once per app run. Only `startup_eligible` (loaded, committed, readback-verified On, not revoked) schedules an attempt; saving On mid-run applies at the next launch. The production Off action notifies lifecycle before enqueueing its filesystem write, including a refused/failed save; no later UI frame is required. Final authorization and entry into `sending` are serialized under the same mutex as withdrawal/stop, without holding that mutex over IO. Withdrawal that precedes that entry cancels; already-submitted work cannot be retracted. Later service loss never triggers a resend.
- **Bounded duplicates.** One operation at a time; a press while one is queued or running is refused and counted, not stacked. A press while an automatic attempt runs is refused; an automatic decision while a press runs is consumed.
- **Close.** `stop()` cancels the in-flight probe (request-local cancellation in `gc_client_send_cancelable`) or readiness wait and joins. It never sends EXIT or any other command; the resident is left running. A push already made is reported as "may still start", never as "not started".
- `gc_client.c`'s last-connect errno is thread-local so the lifecycle worker and the snapshot worker each read their own; only `ECONNREFUSED` is classified as absent.

### Bounded transport and resident Stop/Restart (APP-SVC-03)

- **Absolute budgets.** One monotonic operation deadline (27 s default) spans preprobe, package preparation, re-probe, loader transfer, readiness and supported Stop/Restart. Transport (15 s), readiness (12 s) and stop (10 s) caps can only shorten it. Every exchange receives a nonzero remaining millisecond budget; callbacks are checked for expiry/cancellation before accepting success, refusal, release or new identity. Cooperative callbacks must honor their budgets; this does not bound an arbitrarily blocked provider. Fractional final milliseconds are waited through without extending the deadline or passing client-default zero.
- **Transport.** Connect, every positive short send, EINTR retry, shutdown and reply share one transport deadline and cancellation. Prospero uses its existing nonblocking socket option; hosts use fcntl. Immediate connect()==0 is always connected. No ABI or loader protocol is invented.
- **Transferred-byte truth.** Zero, partial and full transfer stay distinct. Any bytes reported by a provider survive its error enum. Cancellation after transfer and unauthenticated failure-like loader text are outcome_unknown with the actual transferred count, not Not sent. Full transfer does not establish startup; only the worker's fresh qualified STATUS establishes readiness. Nothing is automatically retried.
- **Stop/Restart capability.** The reviewed producer has only unconditional EXIT and no same-identity release contract. Production therefore supplies neither optional capability: requests return stop_unconfirmed/unavailable and send ZERO EXIT or replacement pushes. Another STATUS cannot remove the replacement race.
- **Supported synthetic HOST contracts.** conditional_exit must atomically compare the complete expected reviewed version/product/process identity (pid, started_ms, boot and port). A separate ordinary release witness must establish ownership release for that SAME old identity before replacement. Listener refusal, PID absence, a startup marker, delay or different boot is never release evidence. After release, Restart re-probes before submitting and requires a different qualified ready instance. HOST adapters are synthetic, not evidence of native/provider support.
- Closing the app only cancels/joins local work; it never sends EXIT. Already-submitted bytes cannot be retracted.

Accepted APP-SVC-02 gates retain the strict identity and consent-Off rules above: `ui/tests/test_service_lifecycle.cpp`, `ui/tests/test_service_autostart.cpp` (real Service row through ToolboxUi) and `ui/tests/test_app_service_preferences_ui.cpp` (committed next-launch attempt, Off without another frame at either held pre-submit probe, successful Off commit, failed precommit and committed-unverified save). Normal and whole-compile/link ASan/UBSan gates certify only their exact exercised candidate. Retained historical TSan evidence does not certify this correction. The transport/platform and conditional lifecycle fixtures are ordinary synthetic HOST evidence, not console loader, real GhosttoothAPI, native release or controller-output proof. Independent acceptance remains required.

### Ordinary package and resident rows (APP-SVC-04; acceptance open)

- **Consistency, not build linkage.** `companion-manifest.py generate` records the selected ELF's byte count/digest, selected source checkout revision/dirty state, version and notice names. Source checkout cleanliness does **not** prove an independently selected ELF was built there. No immutable native build/source-linkage receipt is supplied; that limitation remains. Descriptive provenance is not authenticated provenance.
- **Strict release.** Every `--release` verification requires an externally provided reviewed artifact SHA-256, exact supported 40-hex source revision and boolean `source_dirty:false`, plus at least one regular notice beside the ELF. Missing/malformed/wrong pins fail closed independently. Synthetic fixture hashes are test inputs, not reviewed release authorization. Legal completeness of notices remains owner review.
- **All pairs and archives.** Both ELF and manifest names are enumerated. Orphan pairs, misplaced assets/title boundaries, duplicate archive names, traversal/backslash names and ambiguous/symlink file types are rejected. `make -C ui verify-companion-dist COMPANION_DIST=... GC_RELEASE=1 GHOSTTOOTH_SHA256=...` only verifies the existing distribution; it has no app/native build prerequisite, checks every zip and refuses a distribution with no archive. Staging/composition verification remains separate.
- **Runtime boundary.** Required top-level fields, strict numeric types, duplicate top-level keys, bounded nesting, trailing JSON, bounded ASCII version token, descriptive provenance types and unique safe notice basenames are validated. Package JSON also rejects nested duplicate keys; the shared runtime reader rejects duplicate top-level keys but does not expose nested fields. Runtime does not inspect listed notice files or authenticate source provenance/external release hashes. `prepare` accepts only ELF64 little-endian x86-64 with nonempty bounded program headers (stripped permitted), checks manifest size/digest/protocol and copies the validated bytes/version/manifest internally. Every public `submit` rejects bypass or altered bytes before transport; failed/new preparation and cancellation invalidate authorization. Budget/cancel checks bracket the byte comparison without re-reading files or re-growing the remaining budget. Public bounded submission floors the time remaining after version copies/logging and refuses transport when no whole millisecond remains. An internal request-local cancellation guard carries the exact original steady-clock expiry through provider entry and its cooperative polling, without changing the public interface; deadline-only interruption remains distinct from user withdrawal. Cancellation observed during or after transport retires preparation even with zero, partial or full transferred bytes; a fresh successful `prepare` is required for recovery, and recorded bytes are never erased. Arbitrarily blocked callbacks/file reads remain non-preemptible; synthetic structural ELF bytes do not prove target executability.
- **Own resident rows.** Production Start/Stop/Restart rows queue distinct correlated receipts and retain their own phase/outcome across other resident actions. Controller-work START/STOP/RESTART stays distinct. Production has no conditional EXIT/release capability: Stop/Restart remain unavailable/unconfirmed with ZERO EXIT and ZERO replacement push. Only named synthetic HOST providers exercise supported release-before-replacement. Closing the app cancels/joins local work only.

The title-specific helper, native build provenance, target execution and independent acceptance remain unresolved. No helper RPC, hidden privilege fallback, process attachment/signal or PShare completion is claimed. Full APP-SVC-04 remains unchecked; parent owns live tracking/publication.


## Reproduce (WSL; no console operations)

```sh
bash /mnt/h/Ghostcontrol/ghostcontrolToolbox2.0/tools/build-app-service.sh host
bash /mnt/h/Ghostcontrol/ghostcontrolToolbox2.0/tools/build-app-service.sh native
# native: compilation succeeds, artifacts printed, strict zero-kernel provider gate exits 1
```

Host mode builds the integration binary, real standalone host executable, then ASan+UBSan integration binary with warnings-as-errors. Tests use real loopback server/client and real atomic persistence, readback across close/reopen, saved autostart semantics, lightbar pending/ACK/stale revision, rumble pending/poll/start/complete/duplicate ACK, busy refusal, STOP/START/RESTART/RELOAD, independent rumble with lightbar disabled, effective step and effect 4, malformed/unsupported requests, profile create/edit/rename/import/export/files/get, ordinary log tail, ownership contention and lease expiry. App observations and successful API return codes are named test fixtures, not manufactured production response data. No console process is launched.

Observed compile-only artifact at initial final audit:

- `/home/red/gc-usability-app-service/ghostcontrol-app-service.elf`
- **216880 bytes**
- SHA-256 `66b2d8156286378701005322f39bfdd2cffba6289f6d51fb82f5377b80e88b3a`
- Normal-symbol allowlist: **86 imported symbols**; dynamic provider audit disclosed above.
- Reports: `native-imports.txt`, `native-dynamic.txt`, `native-sha256.txt`, `host-integration.log`, `host-sanitized.log`.

These are compile/host results only. Console runtime import resolution, normal root/file permissions, filesystem locking, app-local handle acquisition, native input continuity and physical lightbar/rumble/reset results are **not verified**. No deployment, privileged library, unsafe fallback or alternative-process pad borrowing may be used to fill those gaps. The literal zero-kernel provider gate currently remains a blocker for acceptance.

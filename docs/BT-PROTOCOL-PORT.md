# Bluetooth Protocol & Shared Chip Handling — Technical Notes

**Date:** 2026-10-06

---

## LE Connection State Machine

A fire-and-forget `LE Create Connection` is not enough on the PS5's shared Bluetooth chip. The system's driver may already have a connection attempt in flight (Command Disallowed `0x0C`), the scan may not have been disabled in time, and an address that just refused must not be retried immediately or the controller gives up.

**State machine:**

```
LC_NONE → LC_SCAN_OFF → LC_CREATING → LC_PENDING → (connected)
                                ↓ 0x0C
                         LC_CANCELLING → LC_BACKOFF → retry
```

**Key behaviors:**
- **Scan/Create interlock:** Scanning is disabled before Create Connection (some controllers won't do both). The scan command's reply (or a 300ms timeout) advances to `le_create()`.
- **0x0C handling:** When the system's driver has a pending connection, send `LE Create Connection Cancel`, wait for the cancel to complete, then retry with exponential backoff.
- **Exponential backoff:** 300ms → 800ms → 1500ms across 3 retries, then a 5-second address rest.
- **Address hold buffer:** 4-entry circular buffer prevents re-attempting an address that just refused. Checked in `on_advert()` before connecting.
- **Cancel tracking:** `cancel_done`, `saw02`, `addr02` fields track the cancel lifecycle. Connection Complete with status `0x02` during cancellation is a normal cancel confirmation, not an error.
- **`le_abandon()`:** When a link is freed while still connecting, the Create Connection is cancelled to prevent the controller from staying in paging state.

## Command Status / Command Complete Routing

LE-specific Command Status and Command Complete events are routed to the state machine:
- **Command Status for `0x200D`/`0x200E`** (Create Connection / Cancel) → `le_on_create_status()`
- **Command Complete for `0x200C`/`0x200E`** (Scan Enable / Cancel) → `le_on_cmd_complete()`

This allows the state machine to advance when the controller acknowledges commands, rather than relying solely on timeouts.

## Scan Enable Tracking

Outstanding scan enable commands (`le_scan_out`) and the last requested state (`le_scan_last`) are tracked. This prevents redundant scan commands and allows the state machine to know when scanning has been confirmed disabled.

---

## Shared Chip Handling

The PS5's Bluetooth chip (`/dev/ugen0.2`, MediaTek `0e8d:3603`) is shared with the system's driver. Both processes submit USB transfers simultaneously. The kernel caps transfers per process (~65), so:

- **Keep reads pending** to win packets from the system's driver (race for USB transfers).
- **Never reset the controller** — only change event mask, SSP mode, and page scan (with save/restore).
- **Retry all commands 3×** — replies may go to the system's driver.
- **Credit-based ACL flow control** — the controller's NoCP reports may be stolen; track credits and use inflight timestamps.
- **Probe liveness** with `READ_BUFFER_SIZE` when silent for 20 seconds.

---

## Future Work

All 8 TODOs have been implemented. Remaining items for future consideration:

1. **GATT caching persistence across firmware updates** — Cache survives restarts but not firmware changes that alter handle assignments.
2. **Extended advertising periodic advertising** — Periodic advertising sync (for audio accessories) not implemented.
3. **HID boot protocol output reports** — Boot protocol input works; output reports (rumble/LED) still require report protocol.
4. **Numeric comparison user confirmation UI** — Value is logged but not shown in a UI prompt.
5. **L2CAP credit-based flow control for data channels** — Currently rejects credit-based connections; full support would enable higher throughput.
6. **Controller reset recovery escalation** — USB port reset is tried once; could escalate to full transport re-open.
7. **LE PHY dynamic switching** — 1M PHY is preferred; could switch to 2M for firmware updates.
8. **Multi-controller LE PHY coordination** — Multiple LE controllers share the PHY; no per-link PHY control.

---

## Testing Notes

To build:
```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd core && make
```

To test the LE connection state machine:
1. Pair an Xbox Series controller (LE) — verify it connects within 2-3 seconds
2. Turn the controller off and on — verify reconnection uses stored LTK
3. Pair a second LE controller while the first is connected — verify the state machine handles the 0x0C case gracefully
4. Check the log for `le pad ...: connecting`, `connected`, `cancelled`, `kept refusing` messages

---

*End of technical notes.*

---

> **WIRE-01 status note 2026-10-14:** the LE state machine and command-status
> routing described here match the current `gc_bt.c` / `src/bt` symbols
> (host-tested via the core suite). Items still future work per the project
> backlog (extended advertising `0x0A`, LE connection-parameter initiation,
> GATT caching, HID boot protocol, numeric-comparison pairing, L2CAP
> credit-based flow, USB reset recovery, LE PHY selection) are NOT claimed
> complete. See `

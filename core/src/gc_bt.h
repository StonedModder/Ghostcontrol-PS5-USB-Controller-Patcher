/* SPDX-License-Identifier: GPL-3.0-or-later */
/* In-core Bluetooth controller input source.
 *
 * GhostControl owns the console's Bluetooth USB-HCI function in userland and
 * runs its own host (inquiry, link keys, L2CAP, SDP, HID; and the LE path).
 * The system Bluetooth stack never sees a paired pad. Each ready pad is
 * surfaced to the core through the ops below, where it claims one ordinary
 * GhostControl slot and becomes a virtual DualSense on the proven
 * slot/VDA/assignment path -- exactly like USB and the LAN GhostPad source.
 *
 * This header exposes only GhostControl-facing types so gc_main.c never pulls
 * in the host's internal headers. The engine (gc_bt.c) is transport-agnostic:
 * the HCI opener is injected (gc_bt_open_hci) so the engine is unit-testable
 * against the simulated controller with no USB or SDK dependency.
 */
#ifndef GC_BT_H
#define GC_BT_H

#include <stddef.h>
#include <stdint.h>
#include "gc_controller_instance.h"

#define GC_BT_MAX_PADS   4
#define GC_BT_DB_MAX     16
#define GC_BT_DESC_MAX   1024
#define GC_BT_PROFILE_MAX 48

typedef struct { uint64_t occurrence; uint16_t handle; } GcBtOrigin;

/* A pad that just became ready. addr is the identity address, most
 * significant byte first for display. desc is the HID report descriptor
 * (empty for a known profile that needs none). */
typedef struct {
    uint16_t vid, pid;
    unsigned char addr[6];
    int le;   /* Bluetooth LE pad */
    char profile[GC_BT_PROFILE_MAX];   /* host profile name, for display */
    uint8_t desc[GC_BT_DESC_MAX];
    int desc_len;
    GcInstanceTransport instance_transport;
    GcInstanceProof instance_proof;
} GcBtController;

/* A normalized pad report. buttons use the ScePadData bit layout, so the
 * core needs no remapping to pass them on; sticks/triggers are 0..255,
 * 128 centred. */
typedef struct {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  l2, r2;
    uint8_t  has_motion;
    int16_t  gyro[3], accel[3];
    uint8_t  has_touch;
    struct { uint8_t down; uint16_t x, y; } touch[2];
} GcBtPad;

/* Bridges the host to the core. ready() returns 0 to accept the pad (and it
 * is then fed and finally lost()), or <0 to reject it. report() carries the
 * raw HID input report (report id first) so the core can run it through the
 * same profile/rawmap/guided-setup pipeline USB controllers use. state()
 * carries the host's own normalized parse as a default. */
typedef struct {
    void *ud;
    int  (*ready)(void *ud, int bt_slot, const GcBtOrigin *origin, const GcBtController *c);
    void (*report)(void *ud, int bt_slot, const GcBtOrigin *origin, const uint8_t *rep, int len);
    void (*state)(void *ud, int bt_slot, const GcBtOrigin *origin, const GcBtPad *pad);
    void (*lost)(void *ud, int bt_slot, const GcBtOrigin *origin);
} GcBtOps;

/* Measured radio diagnostics (BT-R01). Everything here was observed by the
 * transport, the host or the engine; nothing is configured or inferred.
 * Ages are milliseconds before the snapshot was handed out; -1 means never.
 * No pairing secret or radio address is ever included. */
typedef struct {
    int  radio_open;           /* a host is open on the transport right now */
    int  measured;             /* counters below come from an opened radio
                                * (the current one, or the last one if closed) */
    int  have_usb;             /* USB device descriptor was read */
    uint16_t usb_vid, usb_pid;
    int  iface;                /* HCI interface in use; -1 unknown */
    uint8_t ep_events, ep_acl_in, ep_acl_out;
    int  hci_functions;
    int  endpoints_from_descriptor;
    unsigned acl_mtu;
    int  acl_buffers;
    int  le;
    uint32_t reads, reads_empty, events, acl;
    uint32_t event_queue_drops, acl_queue_drops, partial_event_drops;
    uint32_t acl_stale, acl_malformed, tx_no_credit, tx_failed;
    uint32_t probes, reset_attempts; /* USB reset attempted (may be unavailable) */
    long last_rx_age_ms;       /* -1: nothing heard on that radio */
    uint32_t open_attempts, open_failures, transport_losses;
    char last_failure[120];    /* empty: none since the engine started */
    long last_failure_age_ms;  /* -1: none */
    long sample_age_ms;        /* how old this snapshot was when copied out */
    /* engine-internal monotonic stamps the ages are computed from */
    long at_sample_ms, at_last_rx_ms, at_last_failure_ms;
} GcBtDiag;

/* A point-in-time view for the Toolbox status page. */
typedef struct {
    int  ok;
    char version[24];
    char state[12];            /* trying | ready | failed | stopped */
    char reason[200];
    int  pairing;              /* seconds left in the pairing window */
    int  paired_count;
    struct {
        unsigned char addr[6];
        uint16_t vid, pid;
        int le, connected;
    } paired[GC_BT_DB_MAX];
    int  controller_count;
    struct {
        int slot;              /* 1-based */
        char profile[GC_BT_PROFILE_MAX];
        unsigned char addr[6];
        uint16_t vid, pid;
        int le;
        int virtual_pad;
    } controllers[GC_BT_MAX_PADS];
    GcBtDiag diag;
} GcBtStatus;

typedef struct GcBt GcBt;

/* Starts the engine thread. db_path stores link keys and pad metadata; its
 * directory also holds the per-device generic HID maps (<dir>/maps). Returns
 * NULL only if the thread could not be created. */
GcBt *gc_bt_start(const GcBtOps *ops, const char *db_path);
/* Same engine, but the radio stays off until gc_bt_set_enabled(bt, 1). */
GcBt *gc_bt_start_disabled(const GcBtOps *ops, const char *db_path);
void gc_bt_stop(GcBt *bt);
/* Checked resident shutdown: admission/ticks already stopped by the caller.
 * Preflight is observe-only; successful close joins worker/callbacks and proves
 * transport closure. Failure retains reachable ownership/config dependencies. */
const char *gc_bt_release_blocker(GcBt *bt);
int gc_bt_stop_checked(GcBt *bt);

/* Turns the radio on or off. Off closes the host and leaves the chip to the
 * system. */
void  gc_bt_set_enabled(GcBt *bt, int enabled);

/* Opens a pairing window for `seconds`. Returns 0 if accepted. */
int   gc_bt_pair(GcBt *bt, int seconds);
/* Re-arms a radio that gave up (state failed). Returns 0 if accepted. */
int   gc_bt_retry(GcBt *bt);
/* Drops a saved pad by "AA:BB:CC:DD:EE:FF". Returns 0 if accepted. */
int   gc_bt_forget(GcBt *bt, const char *addr);

void  gc_bt_snapshot(GcBt *bt, GcBtStatus *out);

/* The core reports whether a pad's virtual DualSense is live, for status. */
void  gc_bt_set_virtual(GcBt *bt, int bt_slot, const GcBtOrigin *origin, int live);

/* Sends rumble/lights/player to a pad that takes output. */
void  gc_bt_set_output(GcBt *bt, int bt_slot, const GcBtOrigin *origin, uint8_t strong, uint8_t weak,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t player);

/* The HCI opener, provided per build so the engine stays transport-agnostic.
 * The console build (gc_bt_hci.c) opens the USB-HCI chip; tests provide a
 * simulated controller. `out` is an hci_t; declared void* here to keep the
 * host headers out of the core. Returns non-zero on success. */
struct hci; /* opaque */
int gc_bt_open_hci(void *out_hci);

#endif

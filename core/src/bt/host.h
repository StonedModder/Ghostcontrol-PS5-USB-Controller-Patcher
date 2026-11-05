/* Bluetooth HID host for game controllers, on a controller shared with the
 * console's own driver.
 *
 * Runs on one thread: host_poll() pumps the transport, dispatches what
 * arrived and advances every link's state machine. Every step that needs a
 * reply is retried or timed out, since a few packets go to the system's
 * driver instead of here.
 *
 *   pairing:    host_pair() searches for a pad in pairing mode, connects,
 *               pairs, reads its vendor/product id over SDP and opens the
 *               HID channels. The link key and ids are stored.
 *   reconnect:  a stored pad that is switched on connects by itself; it is
 *               accepted, authenticated with the stored key and its HID
 *               channels accepted or opened.
 */
#ifndef PH_HOST_H
#define PH_HOST_H

#include "hci.h"
#include "pad.h"

#define HOST_MAX_PADS 4

typedef struct host host_t;
typedef struct { uint64_t occurrence; uint16_t handle; } host_origin;

typedef struct {
    void (*on_connect)(void *ud, int slot, const pad_info *info);
    void (*on_state)(void *ud, int slot, const pad_state *st);
    void (*on_disconnect)(void *ud, int slot);
    void *ud;
    /* Called every time the host waits (at least every 50 ms), including
     * while it waits for the controller to answer at start-up, so that the
     * menu keeps serving. Must be quick and must not call into the host.
     * NULL: nothing to do. */
    void (*idle)(void *ud);
    /* The raw HID input report (report id first, HID header stripped), as it
     * arrived, for a ready pad. NULL: not wanted. Lets a host above run the
     * report through its own profile/mapping/guided-setup pipeline instead of
     * (or beside) the normalized on_state parse. */
    void (*on_report)(void *ud, int slot, const uint8_t *rep, int len);
    void (*on_connect_qualified)(void *ud, int slot, const host_origin *origin, const pad_info *info);
    void (*on_state_qualified)(void *ud, int slot, const host_origin *origin, const pad_state *state);
    void (*on_disconnect_qualified)(void *ud, int slot, const host_origin *origin);
    void (*on_report_qualified)(void *ud, int slot, const host_origin *origin, const uint8_t *report, int length);
} host_events;

/* Prepares the controller and loads the paired pads from db_path. Returns
 * NULL if the controller does not answer. */
host_t *host_open(hci_t hci, const char *db_path, const host_events *ev);
void host_close(host_t *h);
int host_close_checked(host_t *h);
/* Force teardown of a host whose checked release already failed. The engine
 * uses this only after a transport loss, so an unreleasable host object does
 * not leak for the life of the resident; the loss stays fail-closed through
 * the engine's release_error. Not a release witness. */
void host_abandon(host_t *h);
const char *host_release_blocker(host_t *h);
int host_open_checked(hci_t hci, const char *db_path, const host_events *ev, host_t **out);

void host_poll(host_t *h, int timeout_ms);

/* Searches for pads in pairing mode for `seconds`. */
int  host_pair(host_t *h, int seconds);

/* Sends rumble and lights to the pad in `slot`. Returns 0 if it is not
 * connected or takes no output. */
int  host_set_output(host_t *h, int slot, const pad_output *out);

int  host_connected(const host_t *h, int slot);
int  host_paired_count(const host_t *h);

/* The controller went away (rest mode): close and open again. */
int  host_transport_lost(const host_t *h);

/* ---- for the web page ---- */

typedef struct {
    unsigned char addr[6];      /* identity address */
    uint16_t vid, pid;
    int le;                     /* Bluetooth LE */
    int connected;
} host_paired_info;

/* Seconds left in the pairing window, 0 if not pairing. */
int  host_pairing_left(const host_t *h);

/* The i-th paired pad (0 <= i < host_paired_count). Returns 0 past the end. */
int  host_paired_get(const host_t *h, int i, host_paired_info *out);

/* A connected, ready pad's identity and latest state. Returns 0 if none. */
int  host_pad_get(const host_t *h, int slot, pad_info *info, pad_state *st);

int host_pad_get_qualified(host_t *h, int slot, host_origin *origin, pad_info *info, pad_state *state);

/* ---- diagnostics (read on the host's thread) ---- */

typedef struct {
    hci_stats transport;        /* zero with have_device 0 if not measured */
    int transport_measured;     /* the transport implements stats */
    unsigned acl_mtu;           /* from the controller's buffer-size reply */
    int acl_buffers;
    int le;                     /* the controller does LE */
    unsigned events_handled, acl_handled;
    unsigned acl_stale, acl_malformed;
    unsigned tx_no_credit, tx_failed;
    unsigned probes_sent, reset_attempts; /* attempted, not succeeded */
    long last_rx_ms;            /* monotonic ms of the last packet heard; 0: none */
} host_diag;

void host_diag_get(const host_t *h, host_diag *out);

/* Forgets a paired pad: disconnects it if linked and drops its keys.
 * Returns 0 if it was not paired. */
int  host_forget(host_t *h, const unsigned char addr[6]);

#endif

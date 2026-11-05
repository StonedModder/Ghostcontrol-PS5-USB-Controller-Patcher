/* The HCI transport: commands out, events and ACL packets in.
 *
 * The host stack only sees these operations, so the same code runs on the
 * console (hci_usb.c, the PS5's Bluetooth chip) and in the tests (a
 * simulated controller and pad).
 */
#ifndef PH_HCI_H
#define PH_HCI_H

#define HCI_PKT_MAX 1100
enum hci_packet_kind { HCI_PACKET_EVENT = 1, HCI_PACKET_ACL = 2 };

/* What a transport has observed about itself, for diagnostics. Every field
 * is a measurement taken by the transport, never a configured guess; a
 * transport that cannot tell leaves have_device at 0. */
typedef struct {
    int have_device;            /* the USB device descriptor was read */
    unsigned short vid, pid;    /* from that descriptor */
    int iface;                  /* HCI interface in use (-1: unknown) */
    unsigned char ep_events, ep_acl_in, ep_acl_out;
    int hci_functions;          /* HCI functions found in the descriptor */
    int from_descriptor;        /* endpoints came from the descriptor, not defaults */
    unsigned reads_done;        /* transfers finished */
    unsigned reads_empty;       /* ... of which empty or failed */
    unsigned events_seen;       /* whole HCI events reassembled */
    unsigned acl_seen;          /* whole ACL packets received */
    unsigned event_ring_drops;  /* events lost to a full queue */
    unsigned acl_ring_drops;    /* ACL packets lost to a full queue */
    unsigned stream_drops;      /* half events abandoned (piece lost) */
    unsigned acl_out_switches;  /* the OUT pipe was switched after a stalled write */
    unsigned out_latency_ms;    /* EWMA of completed ACL-out write latency, 0 = none yet */
    unsigned stall_limit_ms;    /* the ACL-out stall limit currently in force (learned) */
} hci_stats;

typedef struct hci_ops {
    /* Sends an HCI command. Returns 0 on a transport error. */
    int  (*cmd)(void *ctx, unsigned opcode, const void *params, int plen);
    /* Sends one ACL packet, 4-byte header included. Returns 0 on error. */
    int  (*acl_send)(void *ctx, const unsigned char *pkt, int len);
    /* Waits up to timeout_ms for incoming packets. Returns 1 if any queued,
     * 0 if none, -1 if the transport is gone (the device went away, as it
     * does across rest mode). */
    int  (*pump)(void *ctx, int timeout_ms);
    /* Pops one queued packet; returns its length or 0 if none. Events come
     * as code, length, parameters; ACL packets with their 4-byte header. */
    int  (*next_event)(void *ctx, unsigned char *out, int max);
    int  (*next_acl)(void *ctx, unsigned char *out, int max);
    void (*close)(void *ctx);
    /* Optional: resets the transport (e.g. USB port reset) to recover from
     * a hung controller. Returns 0 on success. */
    int  (*reset)(void *ctx);
    /* Optional: writes what the transport knows about itself to the log
     * (device, traffic counts, samples). Called once when the controller
     * does not answer, to find out why. NULL: nothing to say. */
    void (*diag)(void *ctx);
    /* Optional: copies the transport's counters and identity. Called on the
     * host's thread only. NULL: nothing measured. */
    void (*stats)(void *ctx, hci_stats *out);
    /* Complete observed-order record: >0 full packet, 0 empty, <0 terminal loss. */
    int (*next_packet)(void *ctx, int *kind, unsigned char *out, int max);
    /* Optional checked release: 0 iff every owned resource was released.
     * blocker observes current uncertainty only, never predicts API errors. */
    int (*close_checked)(void *ctx);
    const char *(*release_blocker)(void *ctx);
} hci_ops;

typedef struct {
    const hci_ops *ops;
    void *ctx;
} hci_t;

#endif

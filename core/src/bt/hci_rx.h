/* Ordinary receive integrity boundary: complete-observation order, not wire order.
 *
 * Overload policy (adopted from HearBridge-PS5 src/bt/hci_usb.c and reworked):
 * a full queue displaces the OLDEST entry so fresh traffic is never lost to
 * stale traffic, and every displacement is counted per kind. Protocol-invalid
 * input is still fatal - overload is survivable, malformed framing is not. */
#ifndef GC_HCI_RX_H
#define GC_HCI_RX_H
#include "hci.h"
#include "evstream.h"
#define HCI_RX_CAPACITY 64
typedef struct {
    unsigned char data[HCI_RX_CAPACITY][HCI_PKT_MAX];
    int length[HCI_RX_CAPACITY], kind[HCI_RX_CAPACITY];
    int head, count, failed;
    unsigned overflow;        /* entries displaced by overload, both kinds */
    unsigned dropped_events;  /* ... of which events */
    unsigned dropped_acl;     /* ... of which ACL packets */
    evstream stream;
} hci_rx;
/* Initialize only for a fresh transport. Failure cannot be cleared in place. */
void hci_rx_init(hci_rx *rx);
int hci_rx_fail(hci_rx *rx);
int hci_rx_check(hci_rx *rx, long now);
/* status_ok/record_ok are adapter-validated completion properties.
 * A successful empty completion is valid, including during a partial event. */
int hci_rx_read(hci_rx *rx, int kind, const unsigned char *data, int length,
                long now, int status_ok, int record_ok);
int hci_rx_next(hci_rx *rx, int *kind, unsigned char *out, int max);
#endif

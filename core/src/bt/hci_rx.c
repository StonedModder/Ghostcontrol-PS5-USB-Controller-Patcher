#include "hci_rx.h"
#include <string.h>
void hci_rx_init(hci_rx *rx) { memset(rx, 0, sizeof *rx); }
int hci_rx_fail(hci_rx *rx)
{
    rx->failed = 1;
    rx->head = rx->count = 0;
    memset(rx->data, 0, sizeof rx->data);
    memset(&rx->stream, 0, sizeof rx->stream);
    return -1;
}
int hci_rx_check(hci_rx *rx, long now)
{
    if (rx->failed) return -1;
    if (rx->head < 0 || rx->head >= HCI_RX_CAPACITY || rx->count < 0 || rx->count > HCI_RX_CAPACITY || rx->stream.len < 0 || rx->stream.len > EVSTREAM_MAX) return hci_rx_fail(rx);
    if (rx->stream.len && now - rx->stream.t_last > EVSTREAM_TIMEOUT)
        return hci_rx_fail(rx);
    return rx->count != 0;
}
static int put(hci_rx *rx, int kind, const unsigned char *data, int length)
{
    int slot;
    if (rx->failed) return -1;
    if (rx->count == HCI_RX_CAPACITY) {
        /* Overload: displace the oldest entry and count it. Fresh traffic is
         * what matters on a shared radio; a full queue must not kill the
         * session (HearBridge-PS5 src/bt/hci_usb.c rings do the same). */
        const int lost = rx->kind[rx->head];
        rx->head = (rx->head + 1) % HCI_RX_CAPACITY;
        --rx->count;
        ++rx->overflow;
        if (lost == HCI_PACKET_EVENT) ++rx->dropped_events;
        else ++rx->dropped_acl;
    }
    slot = (rx->head + rx->count) % HCI_RX_CAPACITY;
    memcpy(rx->data[slot], data, (size_t)length);
    rx->kind[slot] = kind; rx->length[slot] = length; ++rx->count;
    return 1;
}
static void event(void *ctx, const unsigned char *data, int length)
{
    hci_rx *rx = ctx;
    /* Do not reset evstream from inside its emit: it still owns the buffer. */
    if (rx->failed) return;
    int minimum = 2;
    switch (data[0]) {
    case 0x01: minimum=3; break; /* inquiry complete */
    case 0x03: minimum=13; break; /* connection complete */
    case 0x04: minimum=12; break; /* connection request */
    case 0x05: minimum=6; break; /* disconnect complete */
    case 0x06: minimum=5; break; /* authentication complete */
    case 0x08: minimum=6; break; /* encryption change */
    case 0x0e: minimum=5; break; /* command complete (command-specific return follows) */
    case 0x0f: minimum=6; break; /* command status */
    case 0x13: minimum=3; break; /* number of completed packets */
    case 0x3e: minimum=3; break; /* LE meta subevent */
    default: break;
    }
    if (length < minimum ||
        (data[0] == 0x13 && length != 3 + 4 * data[2]) ||
        (data[0] == 0x3e && data[2] == 1 && length < 21)) {
        rx->failed=1; return;
    }
    (void)put(rx, HCI_PACKET_EVENT, data, length);
}
int hci_rx_read(hci_rx *rx, int kind, const unsigned char *data, int length,
                long now, int status_ok, int record_ok)
{
    int offset = 0;
    if (hci_rx_check(rx, now) < 0) return -1;
    if (!status_ok || !record_ok || length < 0 || length > HCI_PKT_MAX ||
        (length && !data) || (kind != HCI_PACKET_EVENT && kind != HCI_PACKET_ACL))
        return hci_rx_fail(rx);
    if (!length) return 0;
    if (kind == HCI_PACKET_EVENT) {
        evstream_feed(&rx->stream, data, length, now, event, rx);
        return rx->failed ? hci_rx_fail(rx) : 1;
    }
    while (offset < length) {
        int total;
        if (length - offset < 4) return hci_rx_fail(rx);
        total = 4 + data[offset + 2] + ((int)data[offset + 3] << 8);
        if (total > HCI_PKT_MAX || total > length - offset) return hci_rx_fail(rx);
        if (put(rx, kind, data + offset, total) < 0) return -1;
        offset += total;
    }
    return 1;
}
int hci_rx_next(hci_rx *rx, int *kind, unsigned char *out, int max)
{
    int n, k;
    if (rx->failed) return -1;
    if (rx->head < 0 || rx->head >= HCI_RX_CAPACITY || rx->count < 0 || rx->count > HCI_RX_CAPACITY) return hci_rx_fail(rx);
    if (!rx->count) return 0;
    n = rx->length[rx->head]; k = rx->kind[rx->head];
    if (!kind || !out || n <= 0 || n > HCI_PKT_MAX || n > max ||
        (k != HCI_PACKET_EVENT && k != HCI_PACKET_ACL) ||
        (k == HCI_PACKET_EVENT && (n < 2 || n != 2 + rx->data[rx->head][1])) ||
        (k == HCI_PACKET_ACL && (n < 4 || n != 4 + rx->data[rx->head][2] +
                                             ((int)rx->data[rx->head][3] << 8))))
        return hci_rx_fail(rx);
    memcpy(out, rx->data[rx->head], (size_t)n); *kind = k;
    rx->head = (rx->head + 1) % HCI_RX_CAPACITY; --rx->count;
    return n;
}

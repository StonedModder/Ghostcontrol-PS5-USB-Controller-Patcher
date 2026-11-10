/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "usb_desc.h"

#include <string.h>

#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT  0x05

#define BT_CLASS         0xE0
#define BT_SUBCLASS      0x01
#define BT_PROTOCOL      0x01

#define EP_XFER_ISOC     1
#define EP_XFER_BULK     2
#define EP_XFER_INTR     3

#define EP_DIR_IN        0x80

/* Where the scan currently stands: the slot reserved for the interface being
 * read (or -1 while outside a usable one) and whether that interface is a
 * Bluetooth one at all. */
typedef struct {
    int slot;
    int bt_class;
} scan_state;

static int is_bt_interface(const uint8_t *desc)
{
    /* bInterfaceNumber is desc[2], bAlternateSetting desc[3], then the three
     * class bytes. Only alternate setting 0 carries the endpoints. */
    return desc[5] == BT_CLASS && desc[6] == BT_SUBCLASS &&
           desc[7] == BT_PROTOCOL && desc[3] == 0;
}

static void begin_interface(const uint8_t *desc, int *count, hci_function *out,
                            scan_state *st)
{
    st->bt_class = is_bt_interface(desc);
    st->slot = -1;

    if (st->bt_class && *count < HCI_FUNCTIONS_MAX) {
        st->slot = *count;
        (*count)++;
        memset(&out[st->slot], 0, sizeof out[st->slot]);
        out[st->slot].iface = desc[2];
    }
}

/* Endpoints are attributed in descriptor order: the first of each kind wins,
 * anything else (a second bulk OUT, an isochronous endpoint) is only noted so
 * the log can show it. */
static void note_endpoint(const uint8_t *desc, hci_function *fn)
{
    uint8_t addr = desc[2];
    int xfer = desc[3] & 3;
    int is_in = (addr & EP_DIR_IN) != 0;
    uint16_t mps = (uint16_t)(desc[4] | (desc[5] << 8));

    if (xfer == EP_XFER_INTR && is_in && !fn->ep_events) {
        fn->ep_events = addr;
        fn->mps_events = mps;
    } else if (xfer == EP_XFER_BULK && is_in && !fn->ep_acl_in) {
        fn->ep_acl_in = addr;
        fn->mps_acl_in = mps;
    } else if (xfer == EP_XFER_BULK && !is_in && !fn->ep_acl_out) {
        fn->ep_acl_out = addr;
        fn->mps_acl_out = mps;
    } else if (fn->n_other < (int)sizeof fn->other) {
        fn->other[fn->n_other++] = addr;
    }
}

static int is_complete(const hci_function *fn)
{
    return fn->ep_events != 0 && fn->ep_acl_in != 0 && fn->ep_acl_out != 0;
}

/* Drops the candidates that turned out not to be an HCI function, keeping the
 * surviving ones in descriptor order and closing the gaps. */
static int keep_complete(hci_function *out, int count)
{
    int kept = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (!is_complete(&out[i])) continue;
        if (kept != i) out[kept] = out[i];
        kept++;
    }
    return kept;
}

int usb_find_hci_functions(const uint8_t *cfg, int len, hci_function *out)
{
    scan_state st = { -1, 0 };
    int off = 0;
    int count = 0;

    while (off + 2 <= len) {
        const uint8_t *desc = cfg + off;
        int desc_len = desc[0];

        if (desc_len < 2 || off + desc_len > len) break;   /* truncated */

        if (desc[1] == USB_DT_INTERFACE && desc_len >= 9) {
            begin_interface(desc, &count, out, &st);
        } else if (desc[1] == USB_DT_ENDPOINT && desc_len >= 7 &&
                   st.bt_class && st.slot >= 0) {
            note_endpoint(desc, &out[st.slot]);
        }
        off += desc_len;
    }

    return keep_complete(out, count);
}

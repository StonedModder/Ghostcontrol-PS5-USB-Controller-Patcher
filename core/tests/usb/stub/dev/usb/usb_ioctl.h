/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host stand-in for the FreeBSD USB headers: only the fields and request
 * names that gc_usb.c and controller_xbox.c use. The tests intercept ioctl(),
 * so the request values are arbitrary. */
#pragma once
#include <stdint.h>

typedef uint8_t uWord[2];
#define UGETW(w) ((uint16_t)((w)[0] | ((w)[1] << 8)))
#define USETW(w, v) ((w)[0] = (uint8_t)(v), (w)[1] = (uint8_t)((v) >> 8))

struct usb_device_descriptor { uWord idVendor, idProduct, bcdDevice; };
struct usb_gen_descriptor {
    void *ugd_data;
    uint32_t ugd_maxlen, ugd_actlen;
    uint8_t ugd_config_index, ugd_iface_index;
};
struct usb_ctl_request {
    void *ucr_data;
    int ucr_flags, ucr_actlen;
    struct { uint8_t bmRequestType, bRequest; uWord wValue, wIndex, wLength; } ucr_request;
};
struct usb_fs_endpoint {
    void **ppBuffer;
    uint32_t *pLength;
    uint32_t nFrames, aFrames;
    uint16_t flags, timeout;
    int status;
};
struct usb_fs_start { uint8_t ep_index; };
struct usb_fs_stop { uint8_t ep_index; };
struct usb_fs_complete { uint8_t ep_index; };

enum {
    USB_GET_REPORT_DESC = 0x7501, USB_DO_REQUEST, USB_GET_DEVICE_DESC,
    USB_GET_CONFIG, USB_GET_FULL_DESC, USB_FS_START, USB_FS_STOP, USB_FS_COMPLETE,
};
#define USB_SHORT_XFER_OK 1
#define USB_FS_FLAG_SINGLE_SHORT_OK 1
#define USB_FS_FLAG_MULTI_SHORT_OK 2

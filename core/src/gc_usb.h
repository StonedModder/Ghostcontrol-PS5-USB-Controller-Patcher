/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_hid.h"
#include <errno.h>

typedef enum {
    GC_USB_INTERFACE_NONE = 0,
    GC_USB_INTERFACE_HID,
    /* HID interrupt interface whose report layout is not generic-gamepad
     * parseable. Known protocol parsers (DS3/DS4/Nintendo/Logitech) can still
     * bind it, but DirectInput admission cannot. */
    GC_USB_INTERFACE_HID_RAW,
    GC_USB_INTERFACE_XUSB,
    GC_USB_INTERFACE_GIP
} GcUsbInterfaceKind;

/* These helpers are intentionally header-only so descriptor selection can be
 * covered by host tests without pulling in the FreeBSD USB ioctl headers. */
static inline GcUsbInterfaceKind gc_usb_classify_interface(
    uint8_t class_code, uint8_t subclass, uint8_t protocol,
    uint8_t alternate_setting, int hid_layout_valid)
{
    if (alternate_setting != 0) return GC_USB_INTERFACE_NONE;
    if (class_code == 0xff && subclass == 0x47 && protocol == 0xd0)
        return GC_USB_INTERFACE_GIP;
    if (class_code == 0xff && subclass == 0x5d && protocol == 1)
        return GC_USB_INTERFACE_XUSB;
    /* Boot HID interfaces are keyboards/mice, not generic gamepads. A HID
     * candidate is usable only after its report descriptor parsed. */
    if (class_code == 3 && subclass != 1)
        return hid_layout_valid ? GC_USB_INTERFACE_HID
                                : GC_USB_INTERFACE_HID_RAW;
    return GC_USB_INTERFACE_NONE;
}

static inline unsigned gc_usb_interface_rank(GcUsbInterfaceKind kind)
{
    switch (kind) {
    case GC_USB_INTERFACE_XUSB:
    case GC_USB_INTERFACE_GIP:
        return 2;
    case GC_USB_INTERFACE_HID:
        return 1;
    case GC_USB_INTERFACE_HID_RAW:
        return 0;
    default:
        return 0;
    }
}

static inline int gc_usb_interface_matches(GcUsbInterfaceKind required,
                                            GcUsbInterfaceKind candidate)
{
    if (required == GC_USB_INTERFACE_NONE) return 1;
    if (required == GC_USB_INTERFACE_HID_RAW)
        return candidate == GC_USB_INTERFACE_HID_RAW ||
               candidate == GC_USB_INTERFACE_HID;
    return required == candidate;
}

/* Descriptor-qualified XUSB discovery stays input-only. During explicit
 * setup, the standard enable write is deferred until SETUP_BEGIN and sent once. */
static inline int gc_usb_xusb_output_allowed(int setup_only)
{
    return !setup_only;
}

static inline int gc_usb_interface_should_replace(GcUsbInterfaceKind current,
                                                   GcUsbInterfaceKind candidate)
{
    return gc_usb_interface_rank(candidate) > gc_usb_interface_rank(current);
}

/* Composite controllers sometimes expose a small auxiliary/media Game Pad
 * collection beside the real controller. Prefer the layout with useful axes,
 * a hat and the richer button set. */
static inline unsigned gc_usb_hid_layout_score(const GcHidLayout *layout)
{
    unsigned score = 0;
    if (!layout) return 0;
    for (size_t index = 0; index < layout->count; ++index) {
        const GcHidField *field = &layout->fields[index];
        if (field->page == 9) {
            score += 4;
        } else if (field->page == 1) {
            switch (field->usage) {
            case 0x30: case 0x31: score += 64; break;
            case 0x33: case 0x34: score += 32; break;
            case 0x39: score += 48; break;
            case 0x90: case 0x91: case 0x92: case 0x93:
                score += 12; break;
            case 0x32: case 0x35: score += 8; break;
            default: break;
            }
        } else if (field->page == 2) {
            switch (field->usage) {
            case 0xba: case 0xc8: score += 32; break;
            case 0xbb: case 0xc4: case 0xc5: score += 16; break;
            default: break;
            }
        }
    }
    return score;
}

/* Resident I/O class. Not an ioctl number and not a pad ABI.
 * Fatal errnos are a gone device. USB_ERR_NO_PIPE (12) and
 * USB_ERR_NOT_CONFIGURED (19) are the same fact in endpoint status.
 * Timeout, short transfer, stall, interrupt and EAGAIN keep the device. */
enum {
    GC_USB_ERR_NO_PIPE = 12,
    GC_USB_ERR_IOERROR = 18,
    GC_USB_ERR_NOT_CONFIGURED = 19,
    GC_USB_ERR_TIMEOUT = 20,
    GC_USB_ERR_SHORT_XFER = 21,
    GC_USB_ERR_STALLED = 22,
    GC_USB_ERR_INTERRUPTED = 23
};

typedef enum {
    GC_USB_IO_OK = 0,
    GC_USB_IO_TRANSIENT = 1,
    GC_USB_IO_FATAL = 2
} GcUsbIoClass;

static inline GcUsbIoClass gc_usb_io_classify(int errno_value, int endpoint_status,
                                              int short_invalid)
{
    if (errno_value == ENODEV || errno_value == ENXIO || errno_value == EBADF ||
        errno_value == ENOENT)
        return GC_USB_IO_FATAL;
    if (endpoint_status == GC_USB_ERR_NO_PIPE ||
        endpoint_status == GC_USB_ERR_NOT_CONFIGURED)
        return GC_USB_IO_FATAL;
    if (errno_value == ETIMEDOUT || errno_value == EAGAIN ||
        errno_value == EWOULDBLOCK || errno_value == EINTR)
        return GC_USB_IO_TRANSIENT;
    if (endpoint_status == GC_USB_ERR_TIMEOUT ||
        endpoint_status == GC_USB_ERR_SHORT_XFER ||
        endpoint_status == GC_USB_ERR_STALLED ||
        endpoint_status == GC_USB_ERR_INTERRUPTED ||
        endpoint_status == GC_USB_ERR_IOERROR)
        return GC_USB_IO_TRANSIENT;
    if (short_invalid || errno_value || endpoint_status)
        return GC_USB_IO_TRANSIENT;
    return GC_USB_IO_OK;
}

typedef struct {
    char path[32];
    uint16_t vid, pid;
    uint8_t in[15], out[15];
    unsigned in_count, out_count;
    /* interface_index is the FreeBSD ordinal used by USB_* iface ioctls;
     * interface_number is bInterfaceNumber from the USB descriptor. */
    int interface_index, interface_number, alternate_setting, hid;
    uint8_t subclass, protocol;
    GcUsbInterfaceKind kind;
    GcHidLayout layout;
} GcUsbDevice;

/* Descriptor reads only: no driver detachment, endpoint opens or USB writes. */
int gc_usb_inspect(const char *path, GcUsbDevice *device);
/* As above, but require one protocol interface and optionally a stable USB
 * bInterfaceNumber. This prevents a rule from combining one interface's
 * protocol with another interface's endpoints on composite controllers. */
int gc_usb_inspect_matching(const char *path, GcUsbInterfaceKind required_kind,
                            int interface_number, GcUsbDevice *device);
size_t gc_usb_devices(GcUsbDevice *devices, size_t capacity);
/* Best-effort HID SET_IDLE. duration_4ms=2 requests an 8 ms periodic report;
 * devices that only report changes may ignore or stall this request. */
int gc_usb_hid_set_idle(int fd, uint8_t interface_number,
                        uint8_t duration_4ms);

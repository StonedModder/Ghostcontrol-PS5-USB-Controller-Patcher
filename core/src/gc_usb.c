/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_usb.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_endian.h>

typedef struct {
    int interface_index, interface_number, alternate_setting;
    uint8_t subclass, protocol;
    GcUsbInterfaceKind kind;
    uint8_t in[15], out[15];
    unsigned in_count, out_count;
    GcHidLayout layout;
} GcUsbCandidate;

static int endpoint_add(GcUsbCandidate *candidate, uint8_t endpoint)
{
    if (!(endpoint & 0x0f) || (endpoint & 0x70)) return -1;
    uint8_t *endpoints = endpoint & 0x80 ? candidate->in : candidate->out;
    unsigned *count = endpoint & 0x80 ? &candidate->in_count : &candidate->out_count;
    for (unsigned i = 0; i < *count; ++i)
        if (endpoints[i] == endpoint) return 0;
    if (*count < 15) endpoints[(*count)++] = endpoint;
    return 0;
}

static int read_hid_layout(int fd, int interface_index, uint8_t interface_number,
                           uint16_t report_length, GcHidLayout *layout)
{
    if (interface_index < 0 || interface_index > 255) return -1;
    uint8_t report[4096];
    if (report_length > sizeof(report)) return -1;
    const size_t receive_length = report_length ? report_length : sizeof(report);
    struct usb_gen_descriptor request;
    memset(&request, 0, sizeof(request));
    request.ugd_data = report;
    request.ugd_maxlen = receive_length;
    request.ugd_iface_index = (uint8_t)interface_index;
    int read_ok = ioctl(fd, USB_GET_REPORT_DESC, &request) == 0;
    if (!read_ok) {
        struct usb_ctl_request control;
        memset(&control, 0, sizeof(control));
        control.ucr_data = report;
        /* Report descriptors are normally much shorter than our bounded
         * receive buffer.  FreeBSD treats that as an error unless short
         * device-to-host control transfers are explicitly accepted. */
        control.ucr_flags = USB_SHORT_XFER_OK;
        control.ucr_request.bmRequestType = 0x81;
        control.ucr_request.bRequest = 6;
        USETW(control.ucr_request.wValue, 0x2200);
        USETW(control.ucr_request.wIndex, interface_number);
        USETW(control.ucr_request.wLength, receive_length);
        read_ok = ioctl(fd, USB_DO_REQUEST, &control) == 0;
        request.ugd_actlen = control.ucr_actlen;
    }
    const int actlen = request.ugd_actlen;
    if (!read_ok || actlen <= 0 || (size_t)actlen > receive_length) {
        /* Two very different failures used to share one silent -1: the console
         * never gave us the descriptor, or it did and we could not read it. A
         * device in this state looks like a dead pad with no explanation. */
        static int noted_unreadable;
        if (noted_unreadable++ < 3)
            log_line("usb hid: iface %u report descriptor unreadable (read_ok=%d actlen=%d errno=%d)",
                     (unsigned)interface_number, read_ok, actlen, errno);
        return -1;
    }
    const int parsed = gc_hid_parse_descriptor(report, (size_t)actlen, layout);
    /* This path runs on every scan, so say it a few times and not a few
     * hundred: at full rate it rotates vpad.log away within seconds. */
    static int noted_parsed;
    if (noted_parsed++ < 3)
        log_line("usb hid: iface %u descriptor %d bytes parse=%d fields=%u raw=%u numbered=%d",
                 (unsigned)interface_number, actlen, parsed,
                 (unsigned)layout->count, (unsigned)layout->raw_count, layout->numbered);
    return parsed == 0 ? 0 : -1;
}

static void select_candidate(GcUsbDevice *device, const GcUsbCandidate *candidate)
{
    /* Replace every candidate-owned field together; endpoints and HID layout
     * from an earlier interface must never leak into the selected interface. */
    memset(device->in, 0, sizeof(device->in));
    memset(device->out, 0, sizeof(device->out));
    device->in_count = candidate->in_count;
    device->out_count = candidate->out_count;
    memcpy(device->in, candidate->in, sizeof(candidate->in));
    memcpy(device->out, candidate->out, sizeof(candidate->out));
    device->interface_index = candidate->interface_index;
    device->interface_number = candidate->interface_number;
    device->alternate_setting = candidate->alternate_setting;
    device->subclass = candidate->subclass;
    device->protocol = candidate->protocol;
    device->kind = candidate->kind;
    device->hid = candidate->kind == GC_USB_INTERFACE_HID;
    memset(&device->layout, 0, sizeof(device->layout));
    if (device->hid) device->layout = candidate->layout;
}

int gc_usb_hid_set_idle(int fd, uint8_t interface_number,
                        uint8_t duration_4ms)
{
    struct usb_ctl_request control;
    memset(&control, 0, sizeof(control));
    control.ucr_request.bmRequestType = 0x21; /* host-to-device, class, iface */
    control.ucr_request.bRequest = 0x0a;      /* HID SET_IDLE */
    USETW(control.ucr_request.wValue, (uint16_t)duration_4ms << 8);
    USETW(control.ucr_request.wIndex, interface_number);
    USETW(control.ucr_request.wLength, 0);
    return ioctl(fd, USB_DO_REQUEST, &control);
}

int gc_usb_inspect_matching(const char *path, GcUsbInterfaceKind required_kind,
                            int required_interface_number,
                            GcUsbDevice *device)
{
    if (!path || !device || required_kind < GC_USB_INTERFACE_NONE ||
        required_kind > GC_USB_INTERFACE_GIP ||
        (required_interface_number < -1 || required_interface_number > 255))
        return -1;
    memset(device, 0, sizeof(*device));
    device->interface_index = -1;
    device->interface_number = -1;
    device->alternate_setting = -1;
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;
    struct usb_device_descriptor desc;
    if (ioctl(fd, USB_GET_DEVICE_DESC, &desc) != 0) { close(fd); return -1; }
    device->vid = UGETW(desc.idVendor);
    device->pid = UGETW(desc.idProduct);
    snprintf(device->path, sizeof(device->path), "%s", path);
    int config = 0;
    if (ioctl(fd, USB_GET_CONFIG, &config) != 0 || config < 0 || config > 255) {
        close(fd); return -1;
    }
    uint8_t data[8192];
    struct usb_gen_descriptor request;
    memset(&request, 0, sizeof(request));
    request.ugd_data = data;
    request.ugd_maxlen = sizeof(data);
    request.ugd_config_index = (uint8_t)config;
    if (ioctl(fd, USB_GET_FULL_DESC, &request) != 0 || request.ugd_actlen > sizeof(data)) {
        close(fd); return -1;
    }
    int index = -1;
    int last_interface = -1;
    for (size_t pos = 0; pos + 2 <= request.ugd_actlen;) {
        const uint8_t *d = data + pos;
        if (d[0] < 2 || d[0] > request.ugd_actlen - pos) { close(fd); return -1; }
        if (d[1] == 4 && d[0] >= 9) {
            if (d[2] != last_interface) { ++index; last_interface = d[2]; }
            GcUsbCandidate candidate;
            memset(&candidate, 0, sizeof(candidate));
            candidate.interface_index = index;
            candidate.interface_number = d[2];
            candidate.alternate_setting = d[3];
            candidate.subclass = d[6];
            candidate.protocol = d[7];

            size_t next = pos + d[0];
            int endpoints_valid = 1;
            uint16_t hid_report_length = 0;
            while (next + 2 <= request.ugd_actlen) {
                const uint8_t *child = data + next;
                if (child[0] < 2 || child[0] > request.ugd_actlen - next) {
                    close(fd); return -1;
                }
                if (child[1] == 4) break;
                if (child[1] == 0x21 && child[0] >= 9) {
                    const unsigned descriptors = child[5];
                    for (unsigned item = 0; item < descriptors; ++item) {
                        const size_t offset = 6u + (size_t)item * 3u;
                        if (offset + 3u > child[0]) break;
                        if (child[offset] == 0x22)
                            hid_report_length = (uint16_t)child[offset + 1] |
                                ((uint16_t)child[offset + 2] << 8);
                    }
                }
                if (child[1] == 5 && child[0] >= 7 && (child[3] & 3u) == 3 &&
                    endpoint_add(&candidate, child[2]) != 0)
                    endpoints_valid = 0;
                next += child[0];
            }
            int hid_valid = 0;
            if (d[5] == 3 && d[6] != 1 && d[3] == 0)
                hid_valid = read_hid_layout(fd, index, d[2],
                    hid_report_length, &candidate.layout) == 0;
            candidate.kind = gc_usb_classify_interface(
                d[5], d[6], d[7], d[3], hid_valid);
            int eligible = candidate.kind != GC_USB_INTERFACE_NONE &&
                gc_usb_interface_matches(required_kind, candidate.kind) &&
                (required_interface_number < 0 ||
                 candidate.interface_number == required_interface_number);
            /* Ranking applies inside a compatible required set too. In
             * particular HID_RAW accepts both raw and descriptor-validated
             * HID, and must prefer the latter instead of the first interface. */
            int replace = device->kind == GC_USB_INTERFACE_NONE ||
                gc_usb_interface_should_replace(device->kind,
                                                 candidate.kind);
            if (candidate.kind == GC_USB_INTERFACE_HID &&
                device->kind == GC_USB_INTERFACE_HID &&
                gc_usb_hid_layout_score(&candidate.layout) >
                    gc_usb_hid_layout_score(&device->layout))
                replace = 1;
            if (eligible && endpoints_valid && candidate.in_count && replace)
                select_candidate(device, &candidate);
            pos = next;
            continue;
        }
        pos += d[0];
    }
    close(fd);
    return device->interface_index >= 0 && device->in_count ? 0 : -1;
}

int gc_usb_inspect(const char *path, GcUsbDevice *device)
{
    return gc_usb_inspect_matching(path, GC_USB_INTERFACE_NONE, -1, device);
}

size_t gc_usb_devices(GcUsbDevice *devices, size_t capacity)
{
    DIR *directory = opendir("/dev");
    if (!directory) return 0;
    size_t count = 0;
    struct dirent *entry;
    while (count < capacity && (entry = readdir(directory))) {
        unsigned bus, address;
        char extra;
        if (sscanf(entry->d_name, "ugen%u.%u%c", &bus, &address, &extra) != 2 || address <= 1)
            continue;
        char path[32];
        int length = snprintf(path, sizeof(path), "/dev/ugen%u.%u", bus, address);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;
        if (gc_usb_inspect(path, &devices[count]) == 0) ++count;
    }
    closedir(directory);
    return count;
}

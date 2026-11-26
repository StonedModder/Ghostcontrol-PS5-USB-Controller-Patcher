/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gc_usb.c's HID layout cache, against fake /dev nodes and descriptors.
 * Each case swaps the device behind a node and counts report-descriptor
 * reads: a reused node must be read again, a steady device must not be. */

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dev/usb/usb_ioctl.h>

static struct dirent entries[4];
static int entry_count, entry_pos;
static int descriptor_reads;
static uint8_t x_usage = 0x30;   /* the usage the current device declares */
static uint16_t fake_bcd_device = 0x0100;

/* One HID interface (IN 0x81, 8 bytes) and a one-axis gamepad descriptor. */
static uint8_t report_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x09, 0x30, 0x15, 0x00,
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0,
};
#define USAGE_OFFSET 7
static const uint8_t config_descriptor[] = {
    9, 4, 0, 0, 1, 3, 0, 0, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof report_descriptor, 0,
    7, 5, 0x81, 3, 8, 0, 10,
};

static DIR *fake_opendir(const char *path) { (void)path; entry_pos = 0; return (DIR *)1; }
static struct dirent *fake_readdir(DIR *dir)
{
    (void)dir;
    return entry_pos < entry_count ? &entries[entry_pos++] : NULL;
}
static int fake_closedir(DIR *dir) { (void)dir; return 0; }
static int fake_open(const char *path, int flags, ...) { (void)path; (void)flags; return 7; }
static int fake_close(int fd) { (void)fd; return 0; }
static int fake_ioctl(int fd, unsigned long request, void *argument)
{
    (void)fd;
    if (request == USB_GET_DEVICE_DESC) {
        struct usb_device_descriptor *d = argument;
        USETW(d->idVendor, 0x0079);
        USETW(d->idProduct, 0x0006);
        USETW(d->bcdDevice, fake_bcd_device);
        return 0;
    }
    if (request == USB_GET_CONFIG) { *(int *)argument = 1; return 0; }
    if (request == USB_GET_FULL_DESC) {
        struct usb_gen_descriptor *g = argument;
        memcpy(g->ugd_data, config_descriptor, sizeof config_descriptor);
        g->ugd_actlen = sizeof config_descriptor;
        return 0;
    }
    if (request == USB_GET_REPORT_DESC) {
        struct usb_gen_descriptor *g = argument;
        ++descriptor_reads;
        memcpy(g->ugd_data, report_descriptor, sizeof report_descriptor);
        ((uint8_t *)g->ugd_data)[USAGE_OFFSET] = x_usage;
        g->ugd_actlen = sizeof report_descriptor;
        return 0;
    }
    return -1;
}

#define opendir fake_opendir
#define readdir fake_readdir
#define closedir fake_closedir
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#include "../../src/gc_usb.c"
#undef opendir
#undef readdir
#undef closedir
#undef open
#undef close
#undef ioctl

#include "gc_profile.h"
/* gc_hid.c's decode paths reference this; descriptor parsing never calls it. */
uint32_t gc_button_mask(GcButton button) { (void)button; return 0; }
void log_line(const char *fmt, ...) { (void)fmt; }

static int failures;
#define CHECK(condition) do { \
    const int ok_ = (condition); \
    printf("%s %s\n", ok_ ? "PASS" : "FAIL", #condition); \
    failures += !ok_; \
} while (0)

static void nodes(int count)
{
    entry_count = count;
    for (int i = 0; i < count; ++i)
        snprintf(entries[i].d_name, sizeof entries[i].d_name, "ugen0.%d", i + 2);
}

static unsigned first_usage(const GcUsbDevice *device)
{
    return device->layout.count ? device->layout.fields[0].usage : 0;
}

int main(void)
{
    GcUsbDevice devices[4];
    int reads;

    nodes(1);
    CHECK(gc_usb_devices(devices, 4) == 1);
    CHECK(descriptor_reads == 1);
    CHECK(first_usage(&devices[0]) == 0x30);

    /* A device that stays on its node is never asked again: some pads restart
     * their start-up on every descriptor fetch. */
    for (int i = 0; i < 50; ++i) (void)gc_usb_devices(devices, 4);
    CHECK(descriptor_reads == 1);

    /* Unplugged, then a same-ID device with another descriptor on the node. */
    nodes(0);
    CHECK(gc_usb_devices(devices, 4) == 0);
    nodes(1);
    x_usage = 0x31;
    CHECK(gc_usb_devices(devices, 4) == 1);
    CHECK(descriptor_reads == 2);
    CHECK(first_usage(&devices[0]) == 0x31);

    /* Swapped between two scans: closing the feed forgets the layout. */
    gc_usb_hid_layout_forget("/dev/ugen0.2");
    x_usage = 0x32;
    CHECK(gc_usb_devices(devices, 4) == 1);
    CHECK(descriptor_reads == 3);
    CHECK(first_usage(&devices[0]) == 0x32);

    /* Swapped with nothing in between: a different bcdDevice is a new key. */
    x_usage = 0x33;
    fake_bcd_device = 0x0200;
    CHECK(gc_usb_devices(devices, 4) == 1);
    CHECK(descriptor_reads == 4);
    CHECK(first_usage(&devices[0]) == 0x33);

    /* A node past the caller's capacity is still present and keeps its entry. */
    nodes(2);
    (void)gc_usb_devices(devices, 4);
    reads = descriptor_reads;
    for (int i = 0; i < 5; ++i) (void)gc_usb_devices(devices, 1);
    (void)gc_usb_devices(devices, 4);
    CHECK(descriptor_reads == reads);

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures != 0;
}

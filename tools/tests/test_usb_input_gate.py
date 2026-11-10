#!/usr/bin/env python3
"""Compile production USB decoding/Nintendo handling; mock transport and HID storage."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/gc_service_inject.c').read_text()
decode = source[source.index('static int usb_decode('):source.index('/* Switch Pro and compatibles:')]
nintendo = (root / 'core/src/controller_nintendo.c').read_text()
handler = nintendo[nintendo.index('int nintendo_handle_packet('):nintendo.rindex('#endif')]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "gc_hid.h"
#include "gc_rawmap.h"
#include "bt/pad.h"
#define NINTENDO_PARSER_TEST
#include "controller_nintendo.c"
struct usb_fs_endpoint { int unused; };
enum { GC_USB_INTERFACE_HID=1, GC_USB_INTERFACE_HID_RAW=2, GC_USB_INTERFACE_XUSB=3 };
typedef struct { int kind; uint16_t vid, pid; GcHidLayout layout; } GcUsbDevice;
typedef struct {
    ScePadData decoded; int fd, slot, nintendo, hs_state; uint8_t nintendo_seq;
    struct usb_fs_endpoint ep[2]; uint8_t report[64]; uint32_t lenp; char path[32];
} UsbFeed;
static GcHidState source_hid_values[1];
static GcHidLayout source_hid_layout[1];
static int out_fails;
static void log_line(const char *format, ...) { (void)format; }
#define LOG(...) log_line(__VA_ARGS__)
static void source_snapshot_lock(void) {}
static void source_snapshot_unlock(void) {}
static int usb_send_cmd(int fd, struct usb_fs_endpoint *ep, uint8_t a, uint8_t b) {
    (void)fd; (void)ep; (void)a; (void)b; return out_fails ? -1 : 0;
}
static int nintendo_send_subcmd(int fd, struct usb_fs_endpoint *ep, uint8_t *seq,
                               uint8_t command, const uint8_t *data, uint32_t size) {
    (void)fd; (void)ep; (void)seq; (void)command; (void)data; (void)size;
    return out_fails ? -1 : 0;
}
HANDLER
static int usb_xusb_qualified(const GcUsbDevice *d) { return d->kind == GC_USB_INTERFACE_XUSB; }
static int ds4_is_compatible_vidpid(uint16_t vid, uint16_t pid) {
    (void)pid; return vid == 0x054c;
}
static int ds4_handle_packet(int fd, void *ep, const uint8_t *data, uint32_t size, ScePadData *pad) {
    (void)fd; (void)ep;
    if (size < 8 || data[0] != 1) return 0;
    pad->buttons = SCE_PAD_BUTTON_CROSS; return 1;
}
#define mamba_xinput_handle_packet ds4_handle_packet
static int hid_layout_same(const GcHidLayout *a, const GcHidLayout *b) { return a->count == b->count; }
int gc_hid_input(const GcHidLayout *layout, const uint8_t *data, size_t size, ScePadData *pad) {
    (void)layout; (void)data; (void)size; (void)pad; return 0;
}
int gc_hid_state_update(GcHidState *state, const GcHidLayout *layout, const uint8_t *data, size_t size) {
    (void)state; (void)layout; (void)data; (void)size; return 0;
}
static int slot_profile(int slot, GcProfile *profile) { (void)slot; (void)profile; return 0; }
void gc_profile_apply(const GcProfile *profile, ScePadData *pad) { (void)profile; (void)pad; }
void gc_rawmap_apply(const GcProfile *profile, const uint8_t *data, size_t size, ScePadData *pad) {
    (void)profile; (void)data; (void)size; (void)pad;
}
DECODE
int main(void) {
    UsbFeed feed = {.nintendo=1, .hs_state=HS_WAIT_81_01, .lenp=64};
    GcUsbDevice device = {.kind=GC_USB_INTERFACE_HID, .vid=0x057e, .pid=0x2009};
    pad_state state; pad_state_reset(&state);
    feed.report[0]=0x81; feed.report[1]=1;
    assert(!usb_decode(&feed, &device, &state)); /* Handshake is not usable input. */
    assert(feed.hs_state == HS_WAIT_81_02);
    feed.report[1]=2;
    assert(!usb_decode(&feed, &device, &state));
    assert(feed.hs_state == HS_STREAMING);
    memset(feed.report, 0, sizeof feed.report); feed.report[0]=0x30;
    feed.report[3]=4; feed.report[4]=2; feed.report[5]=2;
    assert(usb_decode(&feed, &device, &state));
    assert(state.buttons == (SCE_PAD_BUTTON_CROSS|SCE_PAD_BUTTON_OPTIONS|SCE_PAD_BUTTON_UP));
    const ScePadData cached = feed.decoded;
    const pad_state published = state;
    feed.report[0]=0x81; feed.report[1]=1;
    assert(!usb_decode(&feed, &device, &state));
    assert(!memcmp(&feed.decoded, &cached, sizeof cached));
    assert(!memcmp(&state, &published, sizeof published));
    feed.report[0]=0x30;
    feed.report[3]=feed.report[4]=feed.report[5]=0;
    assert(usb_decode(&feed, &device, &state) && state.buttons == 0);
    feed.lenp=1; assert(!usb_decode(&feed, &device, &state)); /* Truncated data. */
    feed.lenp=64; feed.hs_state=HS_WAIT_81_01; feed.report[0]=0x81; feed.report[1]=1;
    out_fails=1; assert(!usb_decode(&feed, &device, &state)); out_fails=0;
    feed.nintendo=0; device.vid=0x054c; feed.report[0]=0x81;
    assert(!usb_decode(&feed, &device, &state)); /* Invalid DS4 report. */
    feed.report[0]=1; assert(usb_decode(&feed, &device, &state));
    device.kind=GC_USB_INTERFACE_XUSB; feed.report[0]=0x81;
    assert(!usb_decode(&feed, &device, &state));
    feed.report[0]=1; assert(usb_decode(&feed, &device, &state));
    device.kind=GC_USB_INTERFACE_HID; device.vid=0x1234; device.layout.count=1;
    assert(usb_decode(&feed, &device, &state)); /* Keep generic HID learning. */
    assert(state.buttons == 0 && state.lx == 128 && source_hid_layout[0].count == 1);
    return 0;
}
'''.replace('HANDLER', handler).replace('DECODE', decode)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-input-gate-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'core/src'),
        str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: protocol handshakes/errors cannot publish neutral input; Nintendo controls and HID learning retained')

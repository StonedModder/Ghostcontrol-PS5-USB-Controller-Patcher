#!/usr/bin/env python3
"""Run the production fallback block with fake USB control requests and clock."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/gc_service_inject.c').read_text()
block = source[source.index('        /* A HID device can be silent'):source.index('        if (!feed->busy)')]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "gc_usb_fallback.h"
#define USB_SLOTS 2
#define USB_REPORT 64
#define GC_USB_INTERFACE_HID 1
#define USB_SHORT_XFER_OK 1
#define USB_DO_REQUEST 1
#define USETW(field, value) ((field) = (value))
typedef struct { unsigned report; } Field;
typedef struct { int numbered; unsigned count; Field fields[1]; } Layout;
typedef struct { int kind, interface_number; uint16_t vid, pid; Layout layout; char path[32]; } Device;
typedef struct { int buttons; } pad_state;
typedef struct { int fd, slot, busy; uint32_t lenp; char path[32]; uint8_t report[64]; GcUsbFallback fallback; } Feed;
static Feed usb_feed[USB_SLOTS];
static int last_valid[USB_SLOTS], calls, decode_ok, zero_reply, request_ok = 1;
static uint64_t clock_ms;
struct usb_ctl_request {
    void *ucr_data; int ucr_flags, ucr_actlen;
    struct { unsigned bmRequestType, bRequest, wValue, wIndex, wLength; } ucr_request;
};
uint64_t now_ms(void) { return clock_ms; }
static void source_snapshot_lock(void) {}
static void source_snapshot_unlock(void) {}
static void log_line(const char *fmt, ...) { (void)fmt; }
static void raw_sample(int slot, const void *data, unsigned length) { (void)slot; (void)data; (void)length; }
static int usb_decode_report(Feed *feed, const Device *device, pad_state *state,
                             const uint8_t *data, uint32_t length) {
    (void)feed; (void)device; (void)state;
    assert(length == 3 && data[2] == 0x6f); return decode_ok;
}
static void remember(int slot, pad_state *state, unsigned vid, unsigned pid) {
    (void)state; (void)vid; (void)pid; last_valid[slot] = 1;
}
static int ioctl(int fd, unsigned command, struct usb_ctl_request *request) {
    (void)fd; (void)command; ++calls;
    assert(request->ucr_request.bmRequestType == 0xa1 && request->ucr_request.bRequest == 1);
    if (!request_ok) { errno = EIO; return -1; }
    request->ucr_actlen = 3;
    memcpy(request->ucr_data, zero_reply ? "\0\0\0" : "\0\0o", 3); return 0;
}
static void tick(Feed *feed) {
    const Device devices[1] = {{.kind=GC_USB_INTERFACE_HID}};
    unsigned i = 0; pad_state state = {0};
    do {
BLOCK
    } while (0);
}
int main(void) {
    Feed *feed = &usb_feed[0]; feed->slot = 0;
    /* RAW-only: interrupt packets arrive but no decoded state is published. */
    for (unsigned i=0; i<30; ++i) {
        clock_ms = i*10; gc_usb_fallback_interrupt(&feed->fallback, clock_ms); tick(feed);
    }
    assert(calls == 0 && !last_valid[0]);
    /* A silent connection still receives fallback, with bounded attempts. */
    memset(feed, 0, sizeof *feed); calls = 0; request_ok = 0;
    for (unsigned i=0; i<100; ++i) { clock_ms = i*10; tick(feed); }
    assert(calls > 0 && calls <= 4);
    assert(feed->fallback.get_report_attempts == (unsigned)calls);
    assert(feed->fallback.get_report_failures == (unsigned)calls);
    /* A replacement connection cannot inherit successful polling state. */
    memset(feed, 0, sizeof *feed); calls = 0; request_ok = decode_ok = 1;
    for (unsigned i=0; i<20; ++i) { clock_ms = 2000+i*10; tick(feed); }
    assert(calls == 1 && feed->fallback.control_decoded);
    assert(feed->fallback.get_report_successes == 1);
    memset(feed, 0, sizeof *feed); last_valid[0] = 0; calls = 0;
    for (unsigned i=0; i<6; ++i) { clock_ms = 3000+i*10; tick(feed); }
    assert(calls == 1 && feed->fallback.get_report_attempts == 1);
    /* Control fallback must not overwrite an outstanding interrupt buffer. */
    memset(feed, 0, sizeof *feed); decode_ok = 0; calls = 0;
    feed->busy = 1; feed->lenp = 64; memset(feed->report, 0xa5, sizeof feed->report);
    for (unsigned i=0; i<6; ++i) { clock_ms = 4000+i; tick(feed); }
    assert(calls == 1 && feed->busy && feed->lenp == 64);
    for (unsigned i=0; i<sizeof feed->report; ++i) assert(feed->report[i] == 0xa5);
    memset(feed, 0, sizeof *feed); zero_reply = decode_ok = 1;
    for (unsigned i=0; i<6; ++i) { clock_ms = 5000+i; tick(feed); }
    assert(feed->fallback.get_report_successes == 1 && !feed->fallback.control_decoded);
    zero_reply = 0;
    /* Recent traffic expires; a genuinely silent pipe can be polled again. */
    memset(feed, 0, sizeof *feed); decode_ok = 0; calls = 0;
    gc_usb_fallback_interrupt(&feed->fallback, 0);
    for (unsigned i=0; i<6; ++i) { clock_ms = 999; tick(feed); }
    assert(calls == 0);
    for (unsigned i=0; i<6; ++i) { clock_ms = 1000+i; tick(feed); }
    assert(calls == 1 && feed->fallback.interrupt_in_received == 1);
    return 0;
}
'''.replace('BLOCK', block)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-fallback-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'core/src'),
        str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: interrupt-based fallback gating, rate limit, counters and connection reuse')

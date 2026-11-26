/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Xbox GIP start-up, run for real against a fake USB_FS endpoint pair:
 *   - the INPUT report that ends the handshake is kept and decodes with the
 *     buttons it holds;
 *   - run through gc_usb_opener, a handshake held at its read boundary by a
 *     silent pad does not stall the caller's tick or its lock, and is
 *     cancelled promptly. */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <dev/usb/usb_ioctl.h>

enum { PAD_STREAMING, PAD_ANNOUNCES, PAD_SILENT };
static int pad_mode;
static struct usb_fs_endpoint *endpoints;
static int started[2], stopped[2];
static pthread_mutex_t fake_mu = PTHREAD_MUTEX_INITIALIZER;

/* An INPUT report holding A (Cross) and LB (L1). */
static const uint8_t held_input[18] = { 0x20, 0x01, 0x00, 0x0e, 0x10, 0x10 };
static const uint8_t announce[4] = { 0x02, 0x09, 0x00, 0x00 };

static int complete_one(struct usb_fs_complete *done, int index, int status)
{
    started[index] = stopped[index] = 0;
    done->ep_index = (uint8_t)index;
    endpoints[index].status = status;
    endpoints[index].aFrames = 1;
    return 0;
}

static int fake_ioctl(int fd, unsigned long request, void *argument)
{
    int rc = -1;
    (void)fd;
    pthread_mutex_lock(&fake_mu);
    if (request == USB_FS_START) {
        started[((struct usb_fs_start *)argument)->ep_index] = 1;
        rc = 0;
    } else if (request == USB_FS_STOP) {
        const int index = ((struct usb_fs_stop *)argument)->ep_index;
        if (started[index]) stopped[index] = 1;
        rc = 0;
    } else if (request == USB_FS_COMPLETE) {
        struct usb_fs_complete *done = argument;
        if (started[1] || stopped[1]) {           /* writes always land */
            rc = complete_one(done, 1, stopped[1] ? 5 : 0);
        } else if (stopped[0]) {                  /* USB_ERR_CANCELLED */
            rc = complete_one(done, 0, 5);
        } else if (started[0] && pad_mode != PAD_SILENT) {
            const uint8_t *packet = pad_mode == PAD_STREAMING ? held_input : announce;
            const uint32_t length = pad_mode == PAD_STREAMING ? sizeof held_input : sizeof announce;
            memcpy(endpoints[0].ppBuffer[0], packet, length);
            endpoints[0].pLength[0] = length;
            rc = complete_one(done, 0, 0);
        } else {
            errno = EBUSY;
        }
    }
    pthread_mutex_unlock(&fake_mu);
    return rc;
}

#define ioctl fake_ioctl
#include "../../src/controller_xbox.c"
#undef ioctl
#include "gc_usb_opener.h"

static int failures;
#define CHECK(condition) do { \
    const int ok_ = (condition); \
    printf("%s %s\n", ok_ ? "PASS" : "FAIL", #condition); \
    failures += !ok_; \
} while (0)

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

typedef struct {
    int fd;
    struct usb_fs_endpoint ep[2];
    XboxControllerState xbox;
} FakeFeed;

static int handshake_work(void *argument, GcUsbOpenerKeepRunning keep_running, void *context)
{
    FakeFeed *feed = argument;
    return xbox_gip_handshake(feed->fd, feed->ep, 0x045e, 0x0b12, &feed->xbox,
                              keep_running, context);
}

static void reset(FakeFeed *feed, int mode, int fd)
{
    memset(feed, 0, sizeof *feed);
    memset(started, 0, sizeof started);
    memset(stopped, 0, sizeof stopped);
    feed->fd = fd;
    feed->xbox.out_available = 1;
    endpoints = feed->ep;
    pad_mode = mode;
}

int main(void)
{
    static FakeFeed feed;
    static GcUsbOpener opener;
    uint8_t report[64];
    ScePadData pad;
    int pipe_fds[2];
    int result = 0;

    /* A pipe nobody writes to: the handshake's poll() on it just times out. */
    if (pipe(pipe_fds) != 0) return 2;

    /* 1. Already streaming: the report that ends the handshake is kept. */
    reset(&feed, PAD_STREAMING, pipe_fds[0]);
    CHECK(xbox_gip_handshake(feed.fd, feed.ep, 0x045e, 0x0b12, &feed.xbox, NULL, NULL) == 1);
    const uint32_t length = xbox_take_initial_report(&feed.xbox, report, sizeof report);
    CHECK(length == sizeof held_input);
    memset(&pad, 0, sizeof pad);
    CHECK(xbox_handle_packet(feed.fd, feed.ep, report, length, &pad, &feed.xbox) == 1);
    CHECK((pad.buttons & SCE_PAD_BUTTON_CROSS) != 0);
    CHECK((pad.buttons & SCE_PAD_BUTTON_L1) != 0);
    CHECK(feed.xbox.input_count == 1);
    CHECK(feed.xbox.cached_pad_valid == 1);
    CHECK(xbox_take_initial_report(&feed.xbox, report, sizeof report) == 0); /* only once */

    /* 2. ANNOUNCE path: no input was consumed, so nothing is handed over. */
    reset(&feed, PAD_ANNOUNCES, pipe_fds[0]);
    CHECK(xbox_gip_handshake(feed.fd, feed.ep, 0x045e, 0x0b12, &feed.xbox, NULL, NULL) == 1);
    CHECK(xbox_take_initial_report(&feed.xbox, report, sizeof report) == 0);

    /* 3. Silent pad, handshake on the opener: the tick keeps running. The
     * mutex stands in for source_dispatch_lock, which the tick takes every
     * pass and the worker never takes. */
    {
        pthread_mutex_t dispatch = PTHREAD_MUTEX_INITIALIZER;
        long passes = 0, worst_gap = 0, last, started_ms;
        int pending_while_running = 1;
        reset(&feed, PAD_SILENT, pipe_fds[0]);
        started_ms = now_ms();
        CHECK(gc_usb_opener_start(&opener, handshake_work, &feed) == 0);
        last = now_ms();
        while (now_ms() - started_ms < 500) {
            pthread_mutex_lock(&dispatch);
            if (gc_usb_opener_poll(&opener, &result) != 0) pending_while_running = 0;
            ++passes;
            pthread_mutex_unlock(&dispatch);
            usleep(8000);   /* the service's pass interval */
            const long t = now_ms();
            if (t - last > worst_gap) worst_gap = t - last;
            last = t;
        }
        CHECK(pending_while_running);
        CHECK(passes >= 40);
        CHECK(worst_gap < 50);
        printf("     %ld passes in 500 ms while the handshake ran; worst gap %ld ms\n",
               passes, worst_gap);
        while (gc_usb_opener_poll(&opener, &result) == 0) usleep(8000);
        const long total = now_ms() - started_ms;
        CHECK(result == 0);                       /* bounded timeout, no pad */
        CHECK(total >= 1400 && total < 2500);
        printf("     handshake ran %ld ms on the worker\n", total);
        CHECK(!gc_usb_opener_active(&opener));
    }

    /* 4. Cancelled mid-handshake, as usb_close_feed does on unplug/shutdown. */
    {
        reset(&feed, PAD_SILENT, pipe_fds[0]);
        CHECK(gc_usb_opener_start(&opener, handshake_work, &feed) == 0);
        usleep(150000);
        const long before = now_ms();
        result = gc_usb_opener_cancel(&opener);
        const long waited = now_ms() - before;
        CHECK(result == -ECANCELED);
        CHECK(waited < 300);
        printf("     cancel returned after %ld ms\n", waited);
        CHECK(!gc_usb_opener_active(&opener));
        CHECK(gc_usb_opener_cancel(&opener) == 0);   /* idle: no-op */
        CHECK(gc_usb_opener_poll(&opener, &result) == -1);
    }

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures != 0;
}

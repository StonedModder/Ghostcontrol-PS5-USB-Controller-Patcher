#include "usb_helpers.h"
#include "usb_helpers_state.h"
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
extern void ghostpad_status_log(const char *fmt, ...);
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define USB_OUT_EP_INDEX        1u
#define USB_OUT_TIMEOUT_MS    150u
#define USB_OUT_WALL_GRACE_MS 100u
#define USB_OUT_DRAIN_MS       250u
#define USB_OUT_BACKING_MAGIC  UINT64_C(0x47434f5554425546)

/* usb_error_t is intentionally not exported by usb_ioctl.h. These values are
 * part of FreeBSD's stable USB ABI (dev/usb/usbdi.h). */
#define USB_STATUS_CANCELLED    5
#define USB_STATUS_TIMEOUT     20
#define USB_STATUS_INTERRUPTED 23

struct UsbOutBacking {
    uint64_t magic;
    void *buffers[1];
    uint32_t lengths[1];
    uint8_t packet[];
};

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ||
        clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u +
               (uint64_t)ts.tv_nsec / 1000000u;
    /* Fail closed. Both clocks are mandatory on the target, so this arm is a
     * guard: reporting expiry makes every wait return -ETIMEDOUT at once and
     * the caller falls through to closing the endpoint, instead of spinning on
     * a clock that never advances. time() is deliberately not a fallback: one
     * second of granularity and wall-clock jumps are wrong for a 150 ms USB
     * deadline, and it is an unreviewed import for this artifact. */
    return UINT64_MAX;
}

static int wait_slice(int fd, uint64_t deadline_ms) {
    struct pollfd pfd;
    uint64_t now = monotonic_ms();
    uint64_t remaining;
    int timeout_ms;
    int result;

    if (now >= deadline_ms)
        return 0;
    remaining = deadline_ms - now;
    timeout_ms = (int)(remaining > 20u ? 20u : remaining);
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    result = poll(&pfd, 1, timeout_ms);
    if (result < 0)
        return errno == EINTR ? 1 : -errno;
    if (result > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return -ENODEV;
    return 1;
}

static int status_error(int status) {
    if (status == USB_STATUS_TIMEOUT)
        return -ETIMEDOUT;
    if (status == USB_STATUS_CANCELLED)
        return -ECANCELED;
    if (status == USB_STATUS_INTERRUPTED)
        return -EINTR;
    return -EIO;
}

/* Wait for the endpoint-1 record specifically. USB_FS_COMPLETE overwrites
 * ep_index with whichever queued completion is oldest. Unexpected records are
 * drained rather than mistaken for successful OUT completion. Call sites
 * serialize IN/OUT, so seeing one indicates stale session state and is logged. */
static int wait_out_completion(int fd, struct usb_fs_endpoint *ep,
                               uint32_t expected_length,
                               uint64_t deadline_ms, const char *tag,
                               int *reaped) {
    for (;;) {
        struct usb_fs_complete complete;
        enum UsbOutCompletionCheck check;
        int result;

        if (monotonic_ms() >= deadline_ms)
            return -ETIMEDOUT;

        memset(&complete, 0, sizeof(complete));
        result = ioctl(fd, USB_FS_COMPLETE, &complete);
        if (result == 0) {
            check = usb_out_check_completion(
                complete.ep_index, USB_OUT_EP_INDEX, ep->status, ep->aFrames,
                ep->pLength ? ep->pLength[0] : 0u, expected_length);
            if (check == USB_OUT_COMPLETION_OTHER) {
                LOG("OUT %s observed unexpected completion ep=%u while waiting for ep=1\n",
                    tag, (unsigned)complete.ep_index);
                continue;
            }

            *reaped = 1;
            if (check == USB_OUT_COMPLETION_STATUS_ERROR) {
                LOG("OUT %s completion status=%d\n", tag, ep->status);
                return status_error(ep->status);
            }
            if (check == USB_OUT_COMPLETION_FRAME_ERROR) {
                LOG("OUT %s invalid completed frames=%u\n", tag,
                    (unsigned)ep->aFrames);
                return -EIO;
            }
            if (check == USB_OUT_COMPLETION_LENGTH_ERROR) {
                LOG("OUT %s short/invalid write=%u expected=%u\n", tag,
                    ep->pLength ? (unsigned)ep->pLength[0] : 0u,
                    (unsigned)expected_length);
                return -EIO;
            }
            return 0;
        }

        if (errno != EBUSY && errno != EINTR)
            return -errno;

        result = wait_slice(fd, deadline_ms);
        if (result <= 0)
            return result == 0 ? -ETIMEDOUT : result;
    }
}

static void stop_out(int fd, const char *tag) {
    struct usb_fs_stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.ep_index = USB_OUT_EP_INDEX;
    if (ioctl(fd, USB_FS_STOP, &stop) != 0 &&
        errno != ENXIO && errno != ENOTTY && errno != EBADF)
        LOG("OUT %s STOP fail errno=%d\n", tag, errno);
}

/* STOP is asynchronous: keep the user endpoint/backing alive until its cancel
 * (or raced normal) completion has been copied out. If a broken device never
 * produces that record, closing endpoint 1 synchronously unsets the transfer. */
static int cancel_and_reap_out(int fd, struct usb_fs_endpoint *ep,
                               uint32_t expected_length, const char *tag) {
    struct usb_fs_close close_ep;
    int reaped = 0;
    int result;

    stop_out(fd, tag);
    result = wait_out_completion(fd, ep, expected_length,
                                 monotonic_ms() + USB_OUT_DRAIN_MS,
                                 tag, &reaped);
    if (reaped)
        return 0;

    memset(&close_ep, 0, sizeof(close_ep));
    close_ep.ep_index = USB_OUT_EP_INDEX;
    if (ioctl(fd, USB_FS_CLOSE, &close_ep) == 0)
        return 0;

    /* These errors mean the descriptor/device or endpoint transfer no longer
     * exists, so there is no kernel operation left that can address backing. */
    if (errno == EBADF || errno == ENXIO || errno == ENOTTY || errno == EINVAL)
        return 0;

    LOG("OUT %s could not reap cancellation or close ep=1 errno=%d\n",
        tag, errno);
    return result < 0 ? result : -EIO;
}

static void clear_out_endpoint(struct usb_fs_endpoint *ep) {
    ep->ppBuffer = NULL;
    ep->pLength = NULL;
    ep->nFrames = 0;
    ep->aFrames = 0;
    ep->flags = 0;
    ep->timeout = 0;
    ep->status = 0;
}

int usb_send_out(int fd, struct usb_fs_endpoint *ep,
                 const uint8_t *data, uint32_t len, const char *tag) {
    struct UsbOutBacking *backing;
    struct usb_fs_start start;
    uint64_t deadline_ms;
    size_t allocation_size;
    int reaped = 0;
    int result;
    int start_error;

    if (fd < 0 || !ep || (!data && len != 0))
        return -EINVAL;
    /* A prior catastrophic cancel path deliberately leaves these pointers
     * alive for synchronous session teardown. Never overwrite them: the old
     * transfer could copy out through replacement pointers and corrupt/free
     * the wrong allocation. */
    if (ep->ppBuffer || ep->pLength || ep->nFrames != 0)
        return -EBUSY;
    if (!tag)
        tag = "packet";
    if ((size_t)len > SIZE_MAX - sizeof(*backing))
        return -EOVERFLOW;

    allocation_size = sizeof(*backing) + (size_t)len;
    backing = (struct UsbOutBacking *)malloc(allocation_size);
    if (!backing)
        return -ENOMEM;
    backing->magic = USB_OUT_BACKING_MAGIC;
    if (len != 0)
        memcpy(backing->packet, data, len);
    backing->buffers[0] = backing->packet;
    backing->lengths[0] = len;

    ep->ppBuffer = backing->buffers;
    ep->pLength  = backing->lengths;
    ep->nFrames  = 1;
    ep->timeout  = USB_OUT_TIMEOUT_MS;
    ep->flags    = 0;
    ep->aFrames  = 0;
    ep->status   = 0;

    memset(&start, 0, sizeof(start));
    start.ep_index = USB_OUT_EP_INDEX;
    if (ioctl(fd, USB_FS_START, &start) != 0) {
        start_error = errno;
        LOG("OUT %s START fail errno=%d\n", tag, start_error);
        if (start_error == EBUSY) {
            /* The queued record belongs to an older transfer. Its copy-out
             * metadata cannot safely be associated with this packet. Keep
             * our replacement backing alive, cancel best-effort, and force
             * the caller down the synchronous session-reinit path. */
            stop_out(fd, tag);
            return -start_error;
        }
        stop_out(fd, tag);
        clear_out_endpoint(ep);
        free(backing);
        return -start_error;
    }

    deadline_ms = monotonic_ms() + USB_OUT_TIMEOUT_MS +
                  USB_OUT_WALL_GRACE_MS;
    result = wait_out_completion(fd, ep, len, deadline_ms, tag, &reaped);
    if (result != 0) {
        LOG("OUT %s %s error=%d\n", tag,
            result == -ETIMEDOUT ? "timeout" : "completion", -result);
        /* Even a validation/status error gets a defensive STOP. If the
         * expected completion was already reaped, there is nothing to drain. */
        if (reaped)
            stop_out(fd, tag);
        else if (cancel_and_reap_out(fd, ep, len, tag) != 0) {
            /* Do not free storage that a still-live kernel transfer could
             * address. This rare tiny backing is quarantined until the
             * payload process exits rather than risking a kernel UAF. */
            return result;
        }
    }

    clear_out_endpoint(ep);
    backing->magic = 0;
    free(backing);
    return result;
}

int usb_send_cmd(int fd, struct usb_fs_endpoint *ep, uint8_t a, uint8_t b) {
    uint8_t buf[2] = { a, b };
    char tag[8];
    snprintf(tag, sizeof(tag), "%02x%02x", a, b);
    return usb_send_out(fd, ep, buf, 2, tag);
}

void usb_out_release_after_uninit(struct usb_fs_endpoint *ep) {
    struct UsbOutBacking *backing;
    if (!ep || !ep->ppBuffer) return;
    backing = (struct UsbOutBacking *)((uint8_t *)ep->ppBuffer -
        offsetof(struct UsbOutBacking, buffers));
    if (backing->magic != USB_OUT_BACKING_MAGIC ||
        ep->ppBuffer != backing->buffers ||
        ep->pLength != backing->lengths)
        return;
    clear_out_endpoint(ep);
    backing->magic = 0;
    free(backing);
}

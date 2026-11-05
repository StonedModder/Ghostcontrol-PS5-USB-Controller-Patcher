/* controller_xbox.c — Xbox One S GIP controller for Ghost-Control
 * All button bit positions hardware-confirmed on PS5 via live GIP probe.
 * Reference: xboxSeriesSButtonBits.md
 */

#include "controller_xbox.h"
#include <string.h>

#ifndef XBOX_PARSER_TEST
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __PROSPERO__
extern void ghostpad_status_log(const char *fmt, ...);
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#include <stdio.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif
#endif

/* Counts GIP INPUT packets — gates Guide button injection at startup */
/* ── helpers ──────────────────────────────────────────────────────────── */

static uint8_t trig_scale(uint16_t v) {
    if (v > 1023u) v = 1023u;
    return (uint8_t)((v * 255u) / 1023u);
}

static uint8_t stick_x(int16_t v) {
    return (uint8_t)((v + 32768) >> 8);
}

static uint8_t stick_y(int16_t v) {
    int32_t scaled = (32768 - (int32_t)v) >> 8;
    return (uint8_t)(scaled > 255 ? 255 : scaled);
}

XboxCompletionValidation xbox_validate_completion(
    int is_input, int status, uint32_t actual_frames,
    uint32_t actual_length, uint32_t requested_length) {
    if (status != 0) return XBOX_COMPLETION_STATUS_ERROR;
    if (actual_frames != 1u) return XBOX_COMPLETION_FRAME_ERROR;
    if (is_input) {
        if (actual_length == 0u || actual_length > 64u ||
            actual_length > requested_length)
            return XBOX_COMPLETION_LENGTH_ERROR;
    } else if (actual_length != requested_length) {
        return XBOX_COMPLETION_LENGTH_ERROR;
    }
    return XBOX_COMPLETION_OK;
}

/* ── input parsing ────────────────────────────────────────────────────── */

/* Parse GIP INPUT (cmd=0x20, 18 bytes) into ScePadData.
 * Hardware-confirmed bit positions — see xboxSeriesSButtonBits.md */
void xbox_parse_input(const uint8_t *b, ScePadData *o) {
    uint8_t  b4   = b[4];
    uint8_t  b5   = b[5];
    uint16_t lt16 = (uint16_t)b[6]  | ((uint16_t)b[7]  << 8);
    uint16_t rt16 = (uint16_t)b[8]  | ((uint16_t)b[9]  << 8);
    int16_t  lx   = (int16_t)((uint16_t)b[10] | ((uint16_t)b[11] << 8));
    int16_t  ly   = (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8));
    int16_t  rx   = (int16_t)((uint16_t)b[14] | ((uint16_t)b[15] << 8));
    int16_t  ry   = (int16_t)((uint16_t)b[16] | ((uint16_t)b[17] << 8));

    uint8_t lt = trig_scale(lt16);
    uint8_t rt = trig_scale(rt16);

    o->leftStick.x      = stick_x(lx);
    o->leftStick.y      = stick_y(ly);
    o->rightStick.x     = stick_x(rx);
    o->rightStick.y     = stick_y(ry);
    o->analogButtons.l2 = lt;
    o->analogButtons.r2 = rt;

    uint32_t btn = 0;

    /* b[4]: system + face buttons */
    if (b4 & 0x04u) btn |= SCE_PAD_BUTTON_OPTIONS;  /* Menu (≡)  → Options  */
    if (b4 & 0x08u) btn |= SCE_PAD_BUTTON_SHARE;    /* View (⧉)  → Share    */
    if (b4 & 0x10u) btn |= SCE_PAD_BUTTON_CROSS;    /* A         → Cross    */
    if (b4 & 0x20u) btn |= SCE_PAD_BUTTON_CIRCLE;   /* B         → Circle   */
    if (b4 & 0x40u) btn |= SCE_PAD_BUTTON_SQUARE;   /* X         → Square   */
    if (b4 & 0x80u) btn |= SCE_PAD_BUTTON_TRIANGLE; /* Y         → Triangle */

    /* b[5]: dpad + bumpers + stick clicks */
    if (b5 & 0x01u) btn |= SCE_PAD_BUTTON_UP;
    if (b5 & 0x02u) btn |= SCE_PAD_BUTTON_DOWN;
    if (b5 & 0x04u) btn |= SCE_PAD_BUTTON_LEFT;
    if (b5 & 0x08u) btn |= SCE_PAD_BUTTON_RIGHT;
    if (b5 & 0x10u) btn |= SCE_PAD_BUTTON_L1;       /* LB → L1 */
    if (b5 & 0x20u) btn |= SCE_PAD_BUTTON_R1;       /* RB → R1 */
    if (b5 & 0x40u) btn |= SCE_PAD_BUTTON_L3;       /* LS → L3 */
    if (b5 & 0x80u) btn |= SCE_PAD_BUTTON_R3;       /* RS → R3 */

    /* Triggers: analog + digital threshold */
    if (lt > 16u) btn |= SCE_PAD_BUTTON_L2;
    if (rt > 16u) btn |= SCE_PAD_BUTTON_R2;

    o->buttons   = btn;
    o->connected = 1;
    o->quat.w    = 1.0f;
}

void xbox_parse_and_cache_input(const uint8_t *buf, ScePadData *out_pad,
                                XboxControllerState *state) {
    xbox_parse_input(buf, out_pad);
    if (state) {
        ++state->input_count;
        state->cached_pad = *out_pad;
        state->cached_pad_valid = 1;
        if (state->guide_down) out_pad->buttons |= SCE_PAD_BUTTON_PS;
    }
}

int xbox_parse_guide(const uint8_t *buf, uint32_t len,
                     ScePadData *out_pad, XboxControllerState *state) {
    if (!buf || !out_pad || len < 5) return 0;

    if (state && state->cached_pad_valid) {
        *out_pad = state->cached_pad;
    } else {
        memset(out_pad, 0, sizeof(*out_pad));
        out_pad->connected    = 1;
        out_pad->quat.w       = 1.0f;
        out_pad->leftStick.x  = 128;
        out_pad->leftStick.y  = 128;
        out_pad->rightStick.x = 128;
        out_pad->rightStick.y = 128;
    }
    /* Controllers emit a synthetic Guide press during startup. Do not expose
     * Guide until ordinary INPUT traffic proves enumeration has settled. */
    if (state) {
        state->guide_down = (uint8_t)(state->input_count > 10 &&
                                      (buf[4] & 0x01u));
    }
    out_pad->buttons &= ~SCE_PAD_BUTTON_PS;
    if (state && state->guide_down)
        out_pad->buttons |= SCE_PAD_BUTTON_PS;
    return 1;
}

int xbox_requires_powera_rumble_init(uint16_t vid, uint16_t pid) {
    return vid == 0x24c6u &&
           (pid == 0x541au || pid == 0x542au || pid == 0x543au);
}

/* ── GIP protocol ─────────────────────────────────────────────────────── */

#ifndef XBOX_PARSER_TEST

#define GIP_HANDSHAKE_TOTAL_MS 1600u
#define GIP_HANDSHAKE_ATTEMPTS 3
#define GIP_ATTEMPT_LISTEN_MS  400u
#define GIP_READ_TIMEOUT_MS     100u
#define GIP_WRITE_TIMEOUT_MS    120u
#define GIP_POLL_SLICE_MS        20
#define GIP_CANCEL_DRAIN_MS     200u

/* Maintained Linux xpad initialization packets. The PowerA pair is required
 * for these pads to begin input streaming; common LED/auth packets also cover
 * PDP-class GIP devices. Packet byte 2 is replaced with our live sequence. */
static const uint8_t xboxone_hori_ack_id[] = {
    GIP_CMD_ACK, 0x20, 0x00, 0x09,
    0x00, 0x04, 0x20, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00
};
static const uint8_t xboxone_power_on[] = {
    0x05, 0x20, 0x00, 0x01, 0x00
};
static const uint8_t xboxone_s_init[] = {
    0x05, 0x20, 0x00, 0x0f, 0x06
};
static const uint8_t xboxone_extra_input[] = {
    0x4d, 0x10, 0x01, 0x02, 0x07, 0x00
};
static const uint8_t xboxone_led_on[] = {
    0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14
};
static const uint8_t xboxone_auth_done[] = {
    0x06, 0x20, 0x00, 0x02, 0x01, 0x00
};
static const uint8_t xboxone_rumble_begin[] = {
    GIP_CMD_RUMBLE, 0x00, 0x00, 0x09,
    0x00, 0x0f, 0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00
};
static const uint8_t xboxone_rumble_end[] = {
    GIP_CMD_RUMBLE, 0x00, 0x00, 0x09,
    0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

typedef struct {
    uint16_t vid, pid;
    const uint8_t *data;
    uint8_t length;
} XboxInitPacket;

#define XBOX_INIT(v, p, packet) \
    { (v), (p), (packet), (uint8_t)sizeof(packet) }

static const XboxInitPacket xbox_init_packets[] = {
    XBOX_INIT(0x0e6f, 0x0165, xboxone_hori_ack_id),
    XBOX_INIT(0x0f0d, 0x0067, xboxone_hori_ack_id),
    XBOX_INIT(0, 0, xboxone_power_on),
    XBOX_INIT(0x045e, 0x02ea, xboxone_s_init),
    XBOX_INIT(0x045e, 0x0b00, xboxone_s_init),
    XBOX_INIT(0x045e, 0x0b00, xboxone_extra_input),
    XBOX_INIT(0, 0, xboxone_led_on),
    XBOX_INIT(0, 0, xboxone_auth_done),
    XBOX_INIT(0x24c6, 0x541a, xboxone_rumble_begin),
    XBOX_INIT(0x24c6, 0x542a, xboxone_rumble_begin),
    XBOX_INIT(0x24c6, 0x543a, xboxone_rumble_begin),
    XBOX_INIT(0x24c6, 0x541a, xboxone_rumble_end),
    XBOX_INIT(0x24c6, 0x542a, xboxone_rumble_end),
    XBOX_INIT(0x24c6, 0x543a, xboxone_rumble_end),
};

/* Values from FreeBSD's stable usb_error_t ABI. usb_ioctl.h exposes status as
 * an int without importing the kernel-only enum declaration. */
#define GIP_USB_ERR_CANCELLED    5
#define GIP_USB_ERR_TIMEOUT     20
#define GIP_USB_ERR_INTERRUPTED 23

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    struct timeval tv;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u +
               (uint64_t)ts.tv_nsec / 1000000u;
    if (gettimeofday(&tv, NULL) == 0)
        return (uint64_t)tv.tv_sec * 1000u +
               (uint64_t)tv.tv_usec / 1000u;
    return (uint64_t)time(NULL) * 1000u;
}

static int callback_allows_work(XboxKeepRunningFn keep_running,
                                void *context) {
    return !keep_running || keep_running(context);
}

static uint32_t timeout_before(uint64_t deadline_ms, uint32_t maximum_ms) {
    uint64_t now = monotonic_ms();
    uint64_t remaining;
    if (now >= deadline_ms) return 0;
    remaining = deadline_ms - now;
    if (remaining > maximum_ms) remaining = maximum_ms;
    return (uint32_t)remaining;
}

static void stop_transfer(int fd, uint8_t ep_index) {
    struct usb_fs_stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.ep_index = ep_index;
    (void)ioctl(fd, USB_FS_STOP, &stop);
}

static int endpoint_result(uint8_t ep_index,
                           const struct usb_fs_endpoint *eps,
                           const XboxControllerState *state) {
    int status = eps[ep_index].status;
    if (status == GIP_USB_ERR_TIMEOUT) return 0;
    if (status == GIP_USB_ERR_CANCELLED ||
        status == GIP_USB_ERR_INTERRUPTED)
        return -ECANCELED;
    {
        XboxCompletionValidation validation = xbox_validate_completion(
            ep_index == 0, status, eps[ep_index].aFrames,
            state->transfer_length[ep_index],
            state->transfer_requested[ep_index]);
        if (validation == XBOX_COMPLETION_OK) return 1;
        LOG("Xbox ep[%u] invalid completion validation=%d status=%d "
            "frames=%u length=%u requested=%u\n",
            (unsigned)ep_index, (int)validation, status,
            (unsigned)eps[ep_index].aFrames,
            (unsigned)state->transfer_length[ep_index],
            (unsigned)state->transfer_requested[ep_index]);
    }
    return -EIO;
}

/* USB_FS_COMPLETE returns the next completed endpoint index; it does not use
 * complete.ep_index as a selector. STOP also queues a cancellation completion,
 * which must be consumed before starting another transfer. */
static int drain_after_stop(int fd, struct usb_fs_endpoint *eps,
                            XboxControllerState *state,
                            uint8_t wanted_index) {
    uint64_t deadline_ms = monotonic_ms() + GIP_CANCEL_DRAIN_MS;
    stop_transfer(fd, wanted_index);

    for (;;) {
        struct usb_fs_complete complete;
        int result;

        memset(&complete, 0, sizeof(complete));
        result = ioctl(fd, USB_FS_COMPLETE, &complete);
        if (result == 0) {
            uint8_t completed_index = complete.ep_index;
            if (completed_index >= 2) {
                LOG("Xbox invalid completed endpoint index=%u\n",
                    (unsigned)completed_index);
                return -EIO;
            }
            (void)endpoint_result(completed_index, eps, state);
            if (completed_index == wanted_index) return 0;
            LOG("Xbox drained queued ep[%u] while cancelling ep[%u]\n",
                (unsigned)completed_index, (unsigned)wanted_index);
            continue;
        }
        if (errno != EBUSY && errno != EINTR) return -errno;

        {
            uint32_t remaining = timeout_before(deadline_ms,
                                                GIP_POLL_SLICE_MS);
            struct pollfd pfd;
            int poll_result;
            if (remaining == 0) return -ETIMEDOUT;
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = fd;
            pfd.events = POLLIN | POLLOUT;
            poll_result = poll(&pfd, 1, (int)remaining);
            if (poll_result < 0 && errno != EINTR) return -errno;
            if (poll_result > 0 &&
                (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                return -ENODEV;
        }
    }
}

/* Returns 1 for a normal completion, 0 for a transfer/wall-clock timeout, or
 * a negative errno. The callback is checked at most every 20 ms. */
static int complete_until(int fd, uint8_t ep_index,
                          struct usb_fs_endpoint *eps,
                          XboxControllerState *state, uint64_t deadline_ms,
                          XboxKeepRunningFn keep_running, void *context) {
    for (;;) {
        struct usb_fs_complete complete;
        int result;

        if (!callback_allows_work(keep_running, context)) {
            result = drain_after_stop(fd, eps, state, ep_index);
            return result < 0 ? result : -ECANCELED;
        }

        memset(&complete, 0, sizeof(complete));
        result = ioctl(fd, USB_FS_COMPLETE, &complete);
        if (result == 0) {
            uint8_t completed_index = complete.ep_index;
            if (completed_index >= 2) {
                LOG("Xbox invalid completed endpoint index=%u\n",
                    (unsigned)completed_index);
                result = drain_after_stop(fd, eps, state, ep_index);
                return result < 0 ? result : -EIO;
            }
            result = endpoint_result(completed_index, eps, state);
            if (completed_index == ep_index) return result;
            LOG("Xbox dispatched queued ep[%u] while waiting for ep[%u]\n",
                (unsigned)completed_index, (unsigned)ep_index);
            continue;
        }
        if (errno != EBUSY && errno != EINTR) {
            int error = errno;
            result = drain_after_stop(fd, eps, state, ep_index);
            return result < 0 ? result : -error;
        }

        {
            uint32_t remaining = timeout_before(deadline_ms,
                                                GIP_POLL_SLICE_MS);
            struct pollfd pfd;
            int poll_result;
            if (remaining == 0) {
                result = drain_after_stop(fd, eps, state, ep_index);
                return result < 0 ? result : 0;
            }
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = fd;
            pfd.events = POLLIN | POLLOUT;
            poll_result = poll(&pfd, 1, (int)remaining);
            if (poll_result < 0 && errno != EINTR) {
                int error = errno;
                result = drain_after_stop(fd, eps, state, ep_index);
                return result < 0 ? result : -error;
            }
            if (poll_result > 0 &&
                (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                result = drain_after_stop(fd, eps, state, ep_index);
                return result < 0 ? result : -ENODEV;
            }
        }
    }
}

static int read_one(int fd, struct usb_fs_endpoint *eps,
                    XboxControllerState *state, uint32_t timeout_ms,
                    XboxKeepRunningFn keep_running, void *context) {
    struct usb_fs_start start;
    uint64_t deadline_ms;
    int completion;

    if (timeout_ms == 0) return 0;
    deadline_ms = monotonic_ms() + timeout_ms;
    memset(state->transfer_input, 0, sizeof(state->transfer_input));
    state->transfer_buffer[0] = state->transfer_input;
    state->transfer_length[0] = sizeof(state->transfer_input);
    state->transfer_requested[0] = sizeof(state->transfer_input);
    eps[0].ppBuffer = &state->transfer_buffer[0];
    eps[0].pLength = &state->transfer_length[0];
    eps[0].nFrames = 1;
    eps[0].timeout = (uint16_t)timeout_ms;
    eps[0].aFrames = 0;
    eps[0].status = 0;
    eps[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK |
                   USB_FS_FLAG_MULTI_SHORT_OK;

    memset(&start, 0, sizeof(start));
    start.ep_index = 0;
    if (ioctl(fd, USB_FS_START, &start) != 0) return -errno;
    completion = complete_until(fd, 0, eps, state, deadline_ms,
                                keep_running, context);
    if (completion <= 0) return completion;
    if (eps[0].aFrames == 0 || state->transfer_length[0] == 0) return 0;
    if (state->transfer_length[0] > sizeof(state->transfer_input))
        return -EOVERFLOW;
    return (int)state->transfer_length[0];
}

static int write_one(int fd, struct usb_fs_endpoint *eps,
                     XboxControllerState *state,
                     const uint8_t *data, uint32_t length,
                     uint32_t timeout_ms, XboxKeepRunningFn keep_running,
                     void *context) {
    struct usb_fs_start start;
    uint64_t deadline_ms;

    if (timeout_ms == 0) return 0;
    if (length > sizeof(state->transfer_output)) return -EMSGSIZE;
    deadline_ms = monotonic_ms() + timeout_ms;
    memcpy(state->transfer_output, data, length);
    state->transfer_buffer[1] = state->transfer_output;
    state->transfer_length[1] = length;
    state->transfer_requested[1] = length;
    eps[1].ppBuffer = &state->transfer_buffer[1];
    eps[1].pLength = &state->transfer_length[1];
    eps[1].nFrames = 1;
    eps[1].timeout = (uint16_t)timeout_ms;
    eps[1].aFrames = 0;
    eps[1].status = 0;
    eps[1].flags = 0;

    memset(&start, 0, sizeof(start));
    start.ep_index = 1;
    if (ioctl(fd, USB_FS_START, &start) != 0) return -errno;
    return complete_until(fd, 1, eps, state, deadline_ms,
                          keep_running, context);
}

static int write_gip_packet(int fd, struct usb_fs_endpoint *eps,
                            XboxControllerState *state,
                            const uint8_t *data, uint32_t length,
                            uint32_t timeout_ms,
                            XboxKeepRunningFn keep_running, void *context) {
    uint8_t packet[64];
    if (!data || length < 3 || length > sizeof(packet)) return -EINVAL;
    memcpy(packet, data, length);
    packet[2] = state->output_sequence;
    int result = write_one(fd, eps, state, packet, length, timeout_ms,
                           keep_running, context);
    if (result > 0) ++state->output_sequence;
    return result;
}

static int send_device_init(int fd, struct usb_fs_endpoint *eps,
                            uint16_t vid, uint16_t pid,
                            XboxControllerState *state,
                            uint64_t deadline_ms,
                            XboxKeepRunningFn keep_running, void *context) {
    for (size_t index = 0;
         index < sizeof(xbox_init_packets) / sizeof(xbox_init_packets[0]);
         ++index) {
        const XboxInitPacket *packet = &xbox_init_packets[index];
        if ((packet->vid && packet->vid != vid) ||
            (packet->pid && packet->pid != pid))
            continue;
        uint32_t timeout_ms = timeout_before(deadline_ms,
                                             GIP_WRITE_TIMEOUT_MS);
        if (timeout_ms == 0) return -ETIMEDOUT;
        int result = write_gip_packet(fd, eps, state, packet->data,
                                      packet->length, timeout_ms,
                                      keep_running, context);
        if (result <= 0) return result < 0 ? result : -ETIMEDOUT;
    }
    return 0;
}

static int send_ack_bounded(int fd, struct usb_fs_endpoint *eps,
                            XboxControllerState *state, uint8_t original_seq,
                            uint64_t deadline_ms,
                            XboxKeepRunningFn keep_running, void *context) {
    uint8_t ack[8] = {GIP_CMD_ACK, 0x00, 0x00, 0x04,
                      original_seq, GIP_CMD_ANNOUNCE, 0x00, 0x00};
    uint32_t timeout_ms = timeout_before(deadline_ms, GIP_WRITE_TIMEOUT_MS);
    return write_one(fd, eps, state, ack, sizeof(ack), timeout_ms,
                     keep_running, context);
}

int xbox_gip_handshake(int fd, struct usb_fs_endpoint *eps,
                       uint16_t vid, uint16_t pid,
                       XboxControllerState *state,
                       XboxKeepRunningFn keep_running, void *context) {
    static const uint8_t hello[] = {GIP_CMD_ACK, 0x00, 0x00, 0x00};
    uint64_t deadline_ms;

    if (fd < 0 || !eps || !state) return -EINVAL;
    state->input_count = 0;
    state->cached_pad_valid = 0;
    state->guide_down = 0;
    state->keep_running = keep_running;
    state->keep_running_context = context;
    if (!state->out_available) return -ENXIO;
    if (!callback_allows_work(keep_running, context)) return -ECANCELED;
    deadline_ms = monotonic_ms() + GIP_HANDSHAKE_TOTAL_MS;
    state->output_sequence = 0;

    int init_result = send_device_init(fd, eps, vid, pid, state,
                                       deadline_ms, keep_running, context);
    if (init_result < 0) return init_result;

    /* The xpad-derived sequence above triggers ANNOUNCE and covers device
     * quirks. Three short listen windows cover slow hubs without multiplying
     * nested timeouts. A late ANNOUNCE is still handled while streaming. */
    for (int attempt = 0; attempt < GIP_HANDSHAKE_ATTEMPTS; ++attempt) {
        uint32_t timeout_ms;
        uint64_t listen_deadline;
        int sent;

        if (attempt > 0) {
            timeout_ms = timeout_before(deadline_ms, GIP_WRITE_TIMEOUT_MS);
            if (timeout_ms == 0) break;
            sent = write_gip_packet(fd, eps, state, xboxone_power_on,
                                    sizeof(xboxone_power_on), timeout_ms,
                                    keep_running, context);
            if (sent <= 0) return sent < 0 ? sent : -ETIMEDOUT;
            timeout_ms = timeout_before(deadline_ms, GIP_WRITE_TIMEOUT_MS);
            if (timeout_ms == 0) break;
            sent = write_gip_packet(fd, eps, state, hello, sizeof(hello),
                                    timeout_ms, keep_running, context);
            if (sent <= 0) return sent < 0 ? sent : -ETIMEDOUT;
        }

        listen_deadline = deadline_ms;
        if (attempt + 1 < GIP_HANDSHAKE_ATTEMPTS) {
            uint64_t slice = monotonic_ms() + GIP_ATTEMPT_LISTEN_MS;
            if (slice < listen_deadline) listen_deadline = slice;
        }
        while (timeout_before(listen_deadline, GIP_READ_TIMEOUT_MS) > 0) {
            int count = read_one(
                fd, eps, state,
                timeout_before(listen_deadline, GIP_READ_TIMEOUT_MS),
                keep_running, context);
            if (count < 0) return count;
            if (count >= 4 &&
                state->transfer_input[0] == GIP_CMD_ANNOUNCE) {
                int ack = send_ack_bounded(
                    fd, eps, state, state->transfer_input[1], deadline_ms,
                    keep_running, context);
                if (ack < 0) return ack;
                if (ack > 0) {
                    LOG("Xbox handshake done (announce acknowledged)\n");
                    return 1;
                }
            } else if (count >= 18 &&
                       state->transfer_input[0] == GIP_CMD_INPUT) {
                LOG("Xbox handshake done (already streaming)\n");
                return 1;
            }
        }
    }

    LOG("Xbox handshake ended without announce (bounded timeout)\n");
    return 0;
}

int xbox_handle_packet(int fd, struct usb_fs_endpoint *eps,
                       const uint8_t *buf, uint32_t len,
                       ScePadData *out_pad, XboxControllerState *state) {
    if (!buf || !out_pad || len == 0) return 0;
    uint8_t cmd = buf[0];

    /* Player input */
    if (cmd == GIP_CMD_INPUT && len >= 18) {
        xbox_parse_and_cache_input(buf, out_pad, state);
        return 1;
    }

    /* Guide button (Xbox logo): cmd=0x07, b[4]=0x01 pressed, 0x00 released.
     * Controller auto-sends this at connect — gate behind 10 INPUT packets
     * so the startup auto-send does not inject a PS button press. */
    if (cmd == 0x07) return xbox_parse_guide(buf, len, out_pad, state);

    /* Re-announce during streaming */
    if (cmd == GIP_CMD_ANNOUNCE && len >= 4) {
        uint64_t deadline_ms;
        int result;
        if (!state || !state->out_available) return 0;
        deadline_ms = monotonic_ms() + (2u * GIP_WRITE_TIMEOUT_MS);
        result = send_ack_bounded(
            fd, eps, state, buf[1], deadline_ms,
            state->keep_running, state->keep_running_context);
        if (result <= 0) return -1;
        {
            uint32_t timeout_ms = timeout_before(
                deadline_ms, GIP_WRITE_TIMEOUT_MS);
            if (timeout_ms == 0) return -1;
            result = write_gip_packet(
                fd, eps, state, xboxone_power_on,
                sizeof(xboxone_power_on), timeout_ms,
                state->keep_running, state->keep_running_context);
            if (result <= 0) return -1;
        }
        return 0;
    }

    return 0;
}

#endif

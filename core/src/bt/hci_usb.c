#include "hci_rx.h"
#include "hci_usb.h"
#include "log.h"
#include "usb_desc.h"
#include "util.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NODE        "/dev/ugen0.2"

/* The endpoints come only from the chip's configuration descriptor, and
 * without one the open is refused (BT-R02). The removed fallback layout
 * (events 0x82 / ACL 0x81) was a guessed constant that real hardware
 * contradicts: the console's MediaTek radio (0e8d:3603, live 2026-10-18,
 * fromDescriptor) reports events 0x81,
 * ACL in 0x82, ACL out 0x01. A wrong layout on a shared radio is worse than
 * no radio: refuse with a diagnostic instead. */
#define MPS_EVENTS_DEFAULT  16          /* bounded sanity for a parsed mps only */

/* Reads kept pending so this payload can see traffic on the shared node.
 * Do not detach the system driver and do not reset the device: the console's
 * own pad must keep working on the normal pad path. */
#define N_EVENT_RD  40
#define N_ACL_RD    20
#define ACL_BUF     HCI_PKT_MAX

#define X_ACL_OUT   (N_EVENT_RD + N_ACL_RD)
/* The chip's other bulk OUT. It is the one ACL starts on for a chip whose
 * first bulk OUT runs written ACL as a command (see choose_acl_out), and in
 * every case the one the timeout switch falls back to. */
#define X_ACL_OUT2  (X_ACL_OUT + 1)
#define N_XFER      (X_ACL_OUT2 + 1)

typedef struct {
    int fd;
    int fs_initialized, close_uncertain;
    struct usb_fs_endpoint ep[N_XFER];
    void *bufp[N_XFER][1];
    uint32_t lenp[N_XFER][1];
    unsigned char buf[N_XFER][ACL_BUF];
    unsigned char busy[N_XFER];
    int n_event, n_acl;             /* reads actually opened */
    int dead;                       /* the device went away */
    hci_rx rx;

    /* The HCI function in use and its endpoints. */
    int iface;                      /* its interface number: where commands go */
    uint8_t ep_events, ep_acl_in, ep_acl_out, ep_acl_out2;
    int out_idx;                    /* the OUT transfer in use: X_ACL_OUT or X_ACL_OUT2 */
    long out_start_ms;              /* when the current OUT write was started */
    unsigned out_latency_ms;        /* EWMA of completed ACL-out write latency, 0 = none */
    unsigned stall_limit_ms;        /* learned ACL-out stall limit in ms (0 = default) */
    unsigned acl_out_switches;      /* stalled writes that switched to the second pipe */
    int out2_open;
    int acl_out_second;             /* ACL starts on the descriptor's second bulk OUT */
    int out_done_logged;
    int ev_read_len;                /* one packet of the events endpoint */
    int n_functions;                /* how many the chip has, for the log */
    int from_descriptor;            /* endpoints read from the descriptor */

    /* What was seen, for diag(). */
    struct usb_device_descriptor dd;
    int dd_ok;
    unsigned reads_done, reads_empty, status_hist[48];
    unsigned events_seen, acl_seen;
    unsigned char sample[12][24];
    int sample_len[12], n_sample;
    unsigned char last_cmd[40];
    int last_cmd_len;
    int first_idx, first_len, first_frames;     /* the first read to finish */
    unsigned char first_bytes[16];
} usb_ctx;

static usb_ctx g_ctx = { .fd = -1 };

/* ---- transfers ----------------------------------------------------------- */

static int xfer_open(usb_ctx *c, int index, int ep_addr, int size)
{
    struct usb_fs_open op;

    memset(&op, 0, sizeof op);
    op.ep_index = (uint8_t)index;
    op.ep_no = (uint8_t)ep_addr;
    op.max_bufsize = (uint32_t)size;
    op.max_frames = 1;
    if (ioctl(c->fd, USB_FS_OPEN, &op) != 0) {
        log_line("usb: open endpoint %#x as transfer %d: errno %d", ep_addr, index, errno);
        return 0;
    }
    return 1;
}

static int xfer_start(usb_ctx *c, int index, int length)
{
    struct usb_fs_start st;

    c->lenp[index][0] = (uint32_t)length;
    c->ep[index].nFrames = 1;
    c->ep[index].aFrames = 0;
    c->ep[index].status = 0;
    memset(&st, 0, sizeof st);
    st.ep_index = (uint8_t)index;
    if (ioctl(c->fd, USB_FS_START, &st) != 0) {
        log_line("usb: start transfer %d: errno %d", index, errno);
        if (errno == ENXIO || errno == ENODEV || errno == EIO || errno == EBADF) c->dead = 1;
        if (index < X_ACL_OUT) hci_rx_fail(&c->rx);
        return 0;
    }
    c->busy[index] = 1;
    return 1;
}

/* Adapter validates USB records before handing exact bytes to the ordinary
 * integrity boundary. Empty successful completions alone are not loss. */
static int split_read(usb_ctx *c, int index)
{
    int n = (int)c->lenp[index][0];
    int kind = index < N_EVENT_RD ? HCI_PACKET_EVENT : HCI_PACKET_ACL;
    int valid = c->ep[index].aFrames <= 1 && n >= 0 &&
        n <= (index < N_EVENT_RD ? c->ev_read_len : ACL_BUF) &&
        (c->ep[index].aFrames == 1 || n == 0);
    unsigned before = (unsigned)c->rx.count;
    int result = hci_rx_read(&c->rx, kind, c->buf[index], n, now_ms(),
                              c->ep[index].status == 0, valid);
    if (result >= 0) {
        unsigned added = (unsigned)c->rx.count - before;
        if (kind == HCI_PACKET_EVENT) c->events_seen += added;
        else c->acl_seen += added;
    }
    return result;
}

static int reap(usb_ctx *c)
{
    struct usb_fs_complete done;
    int any = 0;

    for (;;) {
        memset(&done, 0, sizeof done);
        if (hci_rx_check(&c->rx, now_ms()) < 0) return -1;
        if (ioctl(c->fd, USB_FS_COMPLETE, &done) != 0) {
            /* USB_FS_COMPLETE documents EBUSY when no completion is pending. */
            if (errno != EBUSY) return hci_rx_fail(&c->rx);
            break;
        }
        any = 1;
        if (done.ep_index >= N_XFER || !c->busy[done.ep_index]) return hci_rx_fail(&c->rx);
        c->busy[done.ep_index] = 0;
        if (done.ep_index == X_ACL_OUT || done.ep_index == X_ACL_OUT2) {
            if (c->ep[done.ep_index].status) {
                log_line("usb: ACL out status %u", (unsigned)c->ep[done.ep_index].status);
            } else {
                /* Learn the limit from the link's own rhythm: EWMA of completed
                 * write latency, limit = 4x it, clamped to [100, 2000] ms. */
                const long dt = now_ms() - c->out_start_ms;
                if (c->out_start_ms && dt >= 0 && dt < 10000) {
                    c->out_latency_ms = c->out_latency_ms
                        ? (unsigned)((7ull * c->out_latency_ms + (unsigned long)dt) / 8ull)
                        : (unsigned)dt;
                    unsigned learned = c->out_latency_ms * 4u;
                    if (learned < 100u) learned = 100u;
                    if (learned > 2000u) learned = 2000u;
                    c->stall_limit_ms = learned;
                }
                if (!c->out_done_logged) {
                    c->out_done_logged = 1;
                    log_line("usb: first ACL packet written on endpoint %#x",
                             done.ep_index == X_ACL_OUT ? c->ep_acl_out : c->ep_acl_out2);
                }
            }
            continue;
        }
        if (done.ep_index >= (unsigned)c->n_event &&
            (done.ep_index < N_EVENT_RD || done.ep_index >= N_EVENT_RD + c->n_acl))
            return hci_rx_fail(&c->rx);
        if (split_read(c, done.ep_index) < 0) return -1;
        if (c->reads_done == 0) {
            int n = (int)c->lenp[done.ep_index][0];
            c->first_idx = done.ep_index;
            c->first_len = n;
            c->first_frames = c->ep[done.ep_index].aFrames;
            memcpy(c->first_bytes, c->buf[done.ep_index], n < 16 ? (size_t)n : 16);
        }
        c->reads_done++;
        {
            unsigned st = (unsigned)c->ep[done.ep_index].status;
            c->status_hist[st < 47 ? st : 47]++;
        }
        if (!c->lenp[done.ep_index][0]) c->reads_empty++;
    }
    return any;
}

static void keep_reads_pending(usb_ctx *c)
{
    int i;

    for (i = 0; i < c->n_event; i++)
        if (!c->busy[i]) xfer_start(c, i, c->ev_read_len);
    for (i = 0; i < c->n_acl; i++)
        if (!c->busy[N_EVENT_RD + i]) xfer_start(c, N_EVENT_RD + i, ACL_BUF);
}

/* ---- hci_ops ------------------------------------------------------------- */

static int op_cmd(void *ctx, unsigned opcode, const void *params, int plen)
{
    usb_ctx *c = ctx;
    struct usb_ctl_request req;
    unsigned char pkt[3 + 255];

    if (c->dead || hci_rx_check(&c->rx, now_ms()) < 0 || plen < 0 || plen > 255) return 0;
    put16(pkt, opcode);
    pkt[2] = (unsigned char)plen;
    if (plen) memcpy(pkt + 3, params, (size_t)plen);
    c->last_cmd_len = 3 + plen < (int)sizeof c->last_cmd ? 3 + plen : (int)sizeof c->last_cmd;
    memcpy(c->last_cmd, pkt, (size_t)c->last_cmd_len);

    memset(&req, 0, sizeof req);
    req.ucr_data = pkt;
    req.ucr_request.bmRequestType = UT_WRITE_CLASS_DEVICE;
    req.ucr_request.bRequest = 0;
    USETW(req.ucr_request.wValue, 0);
    USETW(req.ucr_request.wIndex, c->iface);
    USETW(req.ucr_request.wLength, 3 + plen);
    if (ioctl(c->fd, USB_DO_REQUEST, &req) != 0) {
        log_line("usb: command %#06x: errno %d", opcode, errno);
        if (errno == ENXIO || errno == ENODEV || errno == EIO || errno == EBADF) c->dead = 1;
        return 0;
    }
    return 1;
}

static int op_acl_send(void *ctx, const unsigned char *pkt, int len)
{
    usb_ctx *c = ctx;
    /* The stall limit is learned from this link's own completion latency
     * (HearBridge-PS5 hci_usb.c STALL_SWITCH_MS, reworked): a fixed 500 was a
     * guess, and the liveness threshold is exactly what must not be guessed. */
    const unsigned limit = c->stall_limit_ms ? c->stall_limit_ms : 500u;
    long deadline = now_ms() + (long)limit;

    if (c->dead || hci_rx_check(&c->rx, now_ms()) < 0 || len < 4 || len > ACL_BUF) return 0;
    while (c->busy[c->out_idx]) {
        if (reap(c) < 0) return 0;
        if (!c->busy[c->out_idx]) break;
        if (now_ms() >= deadline) {
            /* The write never finished: the chip may take ACL on its other
             * bulk OUT endpoint. Try it once, and say so. */
            if (c->out_idx == X_ACL_OUT && c->out2_open) {
                struct usb_fs_stop st;

                memset(&st, 0, sizeof st);
                st.ep_index = (uint8_t)X_ACL_OUT;
                ioctl(c->fd, USB_FS_STOP, &st);
                c->busy[X_ACL_OUT] = 0;
                c->out_idx = X_ACL_OUT2;
                ++c->acl_out_switches;
                log_line("usb: ACL out %#x never finished in %u ms; switching to %#x",
                         c->ep_acl_out, limit, c->ep_acl_out2);
                break;
            }
            log_line("usb: ACL out still busy");
            return 0;
        }
        usleep(500);
    }
    if (c->dead || hci_rx_check(&c->rx, now_ms()) < 0) return 0;
    c->out_start_ms = now_ms();
    memcpy(c->buf[c->out_idx], pkt, (size_t)len);
    return xfer_start(c, c->out_idx, len);
}

static int op_pump(void *ctx, int timeout_ms)
{
    usb_ctx *c = ctx;
    long deadline = now_ms() + timeout_ms;

    for (;;) {
        int any;

        if (c->dead || hci_rx_check(&c->rx, now_ms()) < 0) return -1;
        keep_reads_pending(c);
        any = reap(c);
        if (c->dead || any < 0 || c->rx.failed) return -1;
        if (c->rx.count) return 1;
        if (any) continue;
        if (now_ms() >= deadline) return 0;
        usleep(1000);
    }
}

static int op_next_packet(void *ctx, int *kind, unsigned char *out, int max)
{
    usb_ctx *c = ctx;
    if (c->dead || hci_rx_check(&c->rx, now_ms()) < 0) return -1;
    return hci_rx_next(&c->rx, kind, out, max);
}

static const char *op_release_blocker(void *ctx)
{
    usb_ctx *c = ctx;
    return c->close_uncertain ? "hci_usb_close_uncertain" : NULL;
}
static int op_close_checked(void *ctx)
{
    usb_ctx *c = ctx;
    struct usb_fs_uninit un;
    if (op_release_blocker(c)) return -1;
    if (c->fd < 0) return 0;
    if (c->fs_initialized) {
        memset(&un, 0, sizeof un);
        if (ioctl(c->fd, USB_FS_UNINIT, &un) != 0) return -1;
        c->fs_initialized = 0;
    }
    /* close failure has unspecified descriptor ownership: never retry a
     * possibly reused number. Preserve the ledger and report uncertainty. */
    if (close(c->fd) != 0) { c->close_uncertain = 1; return -1; }
    c->fd = -1;
    return 0;
}
static void op_close(void *ctx) { (void)op_close_checked(ctx); }

/* Why the controller may not be answering: what the device is, whether
 * anything at all came in, in which state the transfers finished, and the
 * first events the system's own traffic put in front of us. */
static void op_diag(void *ctx)
{
    usb_ctx *c = ctx;
    char hex[3 * 24 + 1];
    unsigned i;
    int k, j;

    if (c->dd_ok)
        log_line("diag: device %04x:%04x class %02x/%02x/%02x, USB %04x, %d endpoint-0 bytes",
                 UGETW(c->dd.idVendor), UGETW(c->dd.idProduct), c->dd.bDeviceClass,
                 c->dd.bDeviceSubClass, c->dd.bDeviceProtocol, UGETW(c->dd.bcdUSB),
                 c->dd.bMaxPacketSize);
    else
        log_line("diag: the device descriptor could not be read");
    log_line("diag: %u reads finished (%u empty or failed); %u events and %u ACL packets seen",
             c->reads_done, c->reads_empty, c->events_seen, c->acl_seen);
    if (c->reads_done) {
        for (k = 0; k < c->first_len && k < 16; k++) snprintf(hex + 3 * k, 4, "%02x ", c->first_bytes[k]);
        if (c->first_len <= 0) hex[0] = '\0';
        log_line("diag: the first read to finish was transfer %d (%s), %d bytes in %d frame(s): %s",
                 c->first_idx, c->first_idx < N_EVENT_RD ? "events" : "ACL in",
                 c->first_len, c->first_frames, hex);
        log_line("diag: using interface %d: events %#x, ACL in %#x, ACL out %#x (%#x on a stall), %d HCI function(s) in the chip",
                 c->iface, c->ep_events, c->ep_acl_in, c->ep_acl_out, c->ep_acl_out2,
                 c->n_functions);
    }
    for (i = 0; i < 48; i++)
        if (c->status_hist[i]) log_line("diag: %u reads ended with USB status %u", c->status_hist[i], i);
    for (j = 0; j < c->n_sample; j++) {
        for (k = 0; k < c->sample_len[j]; k++) snprintf(hex + 3 * k, 4, "%02x ", c->sample[j][k]);
        log_line("diag: event %d: %s", j + 1, hex);
    }
    for (k = 0; k < c->last_cmd_len && k < 24; k++) snprintf(hex + 3 * k, 4, "%02x ", c->last_cmd[k]);
    if (c->last_cmd_len) log_line("diag: last command sent: %s", hex);

    /* Read-only questions about the device: what it calls itself, its whole
     * configuration (every interface and endpoint), and which kernel driver
     * has each interface. This is what shows how this firmware lays the chip
     * out. Nothing here sends anything to it or takes anything from it. */
    {
        struct usb_device_info di;
        struct usb_gen_descriptor gd;
        unsigned char cfg[512];
        char name[64];

        memset(&di, 0, sizeof di);
        if (ioctl(c->fd, USB_GET_DEVICEINFO, &di) == 0)
            log_line("diag: \"%.40s\" by \"%.40s\", bus %u address %u, config %u, speed %u, power %u mA, suspended %u",
                     di.udi_product, di.udi_vendor, di.udi_bus, di.udi_addr, di.udi_config_no,
                     di.udi_speed, di.udi_power, di.udi_suspended);

        memset(&gd, 0, sizeof gd);
        gd.ugd_data = cfg;
        gd.ugd_maxlen = sizeof cfg;
        gd.ugd_config_index = 0xFF;                 /* the current configuration */
        if (ioctl(c->fd, USB_GET_FULL_DESC, &gd) == 0) {
            int total = gd.ugd_actlen < (int)sizeof cfg ? gd.ugd_actlen : (int)sizeof cfg;
            log_line("diag: configuration descriptor, %d bytes", total);
            for (j = 0; j < total; j += 24) {
                char line[3 * 24 + 1];
                int m = total - j < 24 ? total - j : 24;
                for (k = 0; k < m; k++) snprintf(line + 3 * k, 4, "%02x ", cfg[j + k]);
                log_line("diag: cfg[%03d] %s", j, line);
            }
        } else {
            log_line("diag: the configuration descriptor could not be read (errno %d)", errno);
        }

        for (j = 0; j < 6; j++) {
            memset(&gd, 0, sizeof gd);
            memset(name, 0, sizeof name);
            gd.ugd_data = name;
            gd.ugd_maxlen = sizeof name - 1;
            gd.ugd_iface_index = (uint8_t)j;
            if (ioctl(c->fd, USB_GET_IFACE_DRIVER, &gd) == 0)
                log_line("diag: interface %d is held by driver \"%.40s\"", j, name);
        }
    }
}


typedef struct {
    uint16_t vid, pid;
    const char *why;
} acl_out_quirk;

static const acl_out_quirk k_acl_out_quirks[] = {
    { 0x0E8D, 0x3603, "the first bulk OUT runs written ACL as an HCI command (opcode 0x2033)" },
};

static const acl_out_quirk *quirk_for(uint16_t vid, uint16_t pid)
{
    size_t i;

    for (i = 0; i < sizeof k_acl_out_quirks / sizeof k_acl_out_quirks[0]; i++)
        if (k_acl_out_quirks[i].vid == vid && k_acl_out_quirks[i].pid == pid)
            return &k_acl_out_quirks[i];
    return NULL;
}

/* Swaps the two ACL OUT roles. out_idx keeps naming the primary transfer, so
 * the timeout switch still goes from the primary to the fallback. */
static void swap_acl_out(usb_ctx *c)
{
    const uint8_t first = c->ep_acl_out;

    c->ep_acl_out = c->ep_acl_out2;
    c->ep_acl_out2 = first;
    c->acl_out_second = !c->acl_out_second;
}

static void choose_acl_out(usb_ctx *c)
{
    const acl_out_quirk *q;

    /* One bulk OUT: there is nothing to choose, and the timeout switch has
     * no second pipe to go to. */
    if (!c->ep_acl_out2) return;
    q = c->dd_ok ? quirk_for((uint16_t)UGETW(c->dd.idVendor),
                             (uint16_t)UGETW(c->dd.idProduct)) : NULL;
    if (!q) {
        log_line("usb: ACL out on %#x, %#x kept for a write that stalls",
                 c->ep_acl_out, c->ep_acl_out2);
        return;
    }
    swap_acl_out(c);
    log_line("usb: %04x:%04x: ACL out on %#x, not %#x: %s; %#x kept as fallback",
             UGETW(c->dd.idVendor), UGETW(c->dd.idProduct),
             c->ep_acl_out, c->ep_acl_out2, q->why, c->ep_acl_out2);
}

/* The HCI function and endpoints to use, from the chip's own descriptor: the
 * first function in descriptor order. (The second one is where the system
 * runs the DualSense; the first was silent in every trial.) */
static int choose_endpoints(usb_ctx *c)
{
    struct usb_gen_descriptor gd;
    unsigned char cfg[512];
    hci_function f[HCI_FUNCTIONS_MAX];
    int n = 0, i;

    memset(&gd, 0, sizeof gd);
    gd.ugd_data = cfg;
    gd.ugd_maxlen = sizeof cfg;
    gd.ugd_config_index = 0xFF;                 /* the current configuration */
    if (ioctl(c->fd, USB_GET_FULL_DESC, &gd) == 0)
        n = usb_find_hci_functions(cfg, gd.ugd_actlen < (int)sizeof cfg ? gd.ugd_actlen : (int)sizeof cfg, f);
    c->n_functions = n;
    if (n > 0) {
        for (i = 0; i < n; i++)
            log_line("usb: HCI function %d: interface %d, events %#x (%u bytes), ACL in %#x, ACL out %#x",
                     i, f[i].iface, f[i].ep_events, f[i].mps_events, f[i].ep_acl_in, f[i].ep_acl_out);
        c->iface = f[0].iface;
        c->ep_events = f[0].ep_events;
        c->ep_acl_in = f[0].ep_acl_in;
        c->ep_acl_out = f[0].ep_acl_out;
        for (i = 0; i < f[0].n_other; i++)
            if (!(f[0].other[i] & 0x80)) { c->ep_acl_out2 = f[0].other[i]; break; }
        c->ev_read_len = f[0].mps_events >= 8 && f[0].mps_events <= 64 ? f[0].mps_events : MPS_EVENTS_DEFAULT;
        c->from_descriptor = 1;
        return 1;
    }
    log_line("usb: no HCI function in the descriptor; refusing to open (no descriptor evidence; the legacy guessed layout was removed, BT-R02)");
    return 0;
}

/* USB port reset: recovers from a hung controller (rest mode can leave the
 * device open but deaf).  The kernel's USBDEVFS_RESET re-enumerates the
 * device in place. */
static int op_reset(void *ctx)
{
    usb_ctx *c = ctx;
    if (c->fd < 0) return -1;
    /* This payload SDK's usb_ioctl.h has no USB_DEV_RESET. Do not invent the
     * number. Pairing still opens the chip; only hung-chip recovery is absent. */
#ifndef USB_DEV_RESET
    log_line("usb: port reset unavailable");
    return -1;
#else
    if (ioctl(c->fd, USB_DEV_RESET, 0) < 0) {
        log_line("usb: port reset failed: errno %d", errno);
        return -1;
    }
    log_line("usb: port reset ok");
    return 0;
#endif
}

static void op_stats(void *ctx, hci_stats *out)
{
    const usb_ctx *c = ctx;

    memset(out, 0, sizeof *out);
    out->have_device = c->dd_ok;
    if (c->dd_ok) {
        out->vid = (unsigned short)UGETW(c->dd.idVendor);
        out->pid = (unsigned short)UGETW(c->dd.idProduct);
    }
    out->iface = c->iface;
    out->ep_events = c->ep_events;
    out->ep_acl_in = c->ep_acl_in;
    out->ep_acl_out = c->ep_acl_out;
    out->hci_functions = c->n_functions;
    out->from_descriptor = c->from_descriptor;
    out->reads_done = c->reads_done;
    out->reads_empty = c->reads_empty;
    out->events_seen = c->events_seen;
    out->acl_seen = c->acl_seen;
    out->event_ring_drops = c->rx.dropped_events;
    out->acl_ring_drops = c->rx.dropped_acl;
    out->stream_drops = (unsigned)c->rx.failed;
    out->acl_out_switches = c->acl_out_switches;
    out->out_latency_ms = c->out_latency_ms;
    out->stall_limit_ms = c->stall_limit_ms ? c->stall_limit_ms : 500u;
}

static const hci_ops k_usb_ops = {
    op_cmd, op_acl_send, op_pump, NULL, NULL, op_close, op_reset, op_diag,
    op_stats, op_next_packet, op_close_checked, op_release_blocker,
};

int hci_usb_open(hci_t *out)
{
    usb_ctx *c = &g_ctx;
    struct usb_fs_init init;
    int i;

    /* Publish even partial-open ownership so the engine can attest cleanup.
     * Never overwrite evidence retained by a previous failed close. */
    out->ops = &k_usb_ops;
    out->ctx = c;
    if (c->fd >= 0 || c->close_uncertain) return 0;
    memset(c, 0, sizeof *c);
    c->fd = open(NODE, O_RDWR);
    if (c->fd < 0) {
        log_line("usb: open %s: errno %d", NODE, errno);
        return 0;
    }

    c->dd_ok = ioctl(c->fd, USB_GET_DEVICE_DESC, &c->dd) == 0;
    if (c->dd_ok)
        log_line("usb: %s is %04x:%04x", NODE, UGETW(c->dd.idVendor), UGETW(c->dd.idProduct));
    if (!choose_endpoints(c)) {
        op_close(c);
        return 0;
    }
    choose_acl_out(c);
    hci_rx_init(&c->rx);

    for (i = 0; i < N_XFER; i++) {
        c->bufp[i][0] = c->buf[i];
        c->lenp[i][0] = ACL_BUF;
        c->ep[i].ppBuffer = c->bufp[i];
        c->ep[i].pLength = c->lenp[i];
        c->ep[i].nFrames = 1;
        c->ep[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    }
    /* A bulk OUT transfer that is a multiple of the packet size must end
     * with a zero-length packet. */
    c->ep[X_ACL_OUT].flags = USB_FS_FLAG_FORCE_SHORT;
    c->ep[X_ACL_OUT2].flags = USB_FS_FLAG_FORCE_SHORT;
    c->out_idx = X_ACL_OUT;

    memset(&init, 0, sizeof init);
    init.pEndpoints = c->ep;
    init.ep_index_max = N_XFER;
    if (ioctl(c->fd, USB_FS_INIT, &init) != 0) {
        log_line("usb: init: errno %d", errno);
        op_close(c);
        return 0;
    }

    c->fs_initialized = 1;

    /* The OUT transfer first: if the kernel's budget runs out, it must not
     * be the one left without a transfer. */
    if (!xfer_open(c, X_ACL_OUT, c->ep_acl_out, ACL_BUF)) {
        /* The pipe chosen above will not open here: go back to descriptor
         * order rather than lose the radio over it. */
        if (!c->acl_out_second) {
            op_close(c);
            return 0;
        }
        {
            const uint8_t refused = c->ep_acl_out;

            swap_acl_out(c);
            log_line("usb: ACL out %#x could not be opened; starting on %#x",
                     refused, c->ep_acl_out);
        }
        if (!xfer_open(c, X_ACL_OUT, c->ep_acl_out, ACL_BUF)) {
            op_close(c);
            return 0;
        }
    }
    /* The spare OUT is opened only if it is there; its absence is no failure. */
    if (c->ep_acl_out2) c->out2_open = xfer_open(c, X_ACL_OUT2, c->ep_acl_out2, ACL_BUF);
    for (c->n_event = 0; c->n_event < N_EVENT_RD; c->n_event++)
        if (!xfer_open(c, c->n_event, c->ep_events, c->ev_read_len)) break;
    for (c->n_acl = 0; c->n_acl < N_ACL_RD; c->n_acl++)
        if (!xfer_open(c, N_EVENT_RD + c->n_acl, c->ep_acl_in, ACL_BUF)) break;
    if (!c->n_event || !c->n_acl) {
        op_close(c);
        return 0;
    }

    log_line("usb: controller open beside the system: %d event reads, %d ACL reads",
             c->n_event, c->n_acl);
    out->ops = &k_usb_ops;
    out->ctx = c;
    return 1;
}

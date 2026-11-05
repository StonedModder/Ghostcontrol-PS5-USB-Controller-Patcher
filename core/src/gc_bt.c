/* SPDX-License-Identifier: GPL-3.0-or-later */
/* In-core Bluetooth controller input source. See gc_bt.h.
 *
 * One engine thread owns the userland Bluetooth host for its whole life: it
 * opens the transport, pumps it, advances every link, and services the
 * Toolbox's pair/retry/forget/enable requests between pumps. The host's
 * events are translated into the GhostControl-facing ops and a status cache
 * the Toolbox reads without touching the host. All host access stays on this
 * one thread; only the request flags and the status cache cross threads, both
 * under the engine lock.
 */
#define _POSIX_C_SOURCE 200809L
#include "gc_bt.h"

#include "host.h"
#include "hci.h"
#include "pad.h"
#include "version.h"
#include "log.h"

#include <pthread.h>
#include <stdatomic.h>
/* Optional internal logger provider contract, not a native ABI declaration.
 * log.c is outside this change's ownership; void close cannot attest release. */
extern int log_close_checked(void) __attribute__((weak));
extern const char *log_release_blocker(void) __attribute__((weak));
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef struct { int pending; GcBtOrigin origin; pad_output out; } GcBtOutReq;

enum { BT_TRYING, BT_READY, BT_FAILED, BT_STOPPED };
#define BT_OPEN_TRIES   5
#define BT_RETRY_MS  3000
#define BT_PAIR_SECONDS 60

struct GcBt {
    pthread_t       thread;
    int             thread_ok;
    int             lock_uncertain;
    pthread_mutex_t lock;

    GcBtOps         ops;
    char            db_path[256];

    /* engine-thread only */
    host_t         *host;
    hci_t           retained_hci;
    int             retained_hci_owned;
    const char     *release_error;
    const char     *observed_release_blocker;
    int             worker_close_failed;
    int             log_owned;
    int             attempt;
    long            next_try;

    /* shared, guarded by lock */
    _Atomic int     stop;
    int             enabled;
    int             state;
    char            reason[200];
    int             req_pair_seconds;
    int             req_retry;
    int             req_forget;
    unsigned char   req_forget_addr[6];
    int             virtual_live[GC_BT_MAX_PADS];
    GcBtOutReq      req_out[GC_BT_MAX_PADS];

    GcBtController metadata[GC_BT_MAX_PADS]; /* engine-thread owned */
    int metadata_valid[GC_BT_MAX_PADS];
    GcBtOrigin origins[GC_BT_MAX_PADS]; /* guarded by lock; copied from actual host */
    int admitted[GC_BT_MAX_PADS]; /* ready rejection cannot authorize data/output */
    GcBtStatus      snap;   /* cache refreshed each loop */

    /* Diagnostics (BT-R01). failures/attempts under lock; radio_diag is
     * engine-thread only and keeps the last opened radio's measurements
     * after it closes, which is when they matter most. */
    unsigned        open_attempts, open_failures, transport_losses;
    char            last_failure[120];
    long            last_failure_ms;
    GcBtDiag        radio_diag;
};

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- host event bridge ---------------------------------------------------
 * The host identifies slots 0..HOST_MAX_PADS-1, the same range the core uses
 * for bt_slot. */

static void fill_controller(GcBtController *c, const pad_info *info)
{
    memset(c, 0, sizeof *c);
    c->vid = info->vid;
    c->pid = info->pid;
    memcpy(c->addr, info->addr, 6);
    c->le = info->le;
    c->instance_transport = info->instance_transport;
    c->instance_proof = info->instance_proof;
    if (info->profile)
        snprintf(c->profile, sizeof c->profile, "%s", info->profile);
    if (info->hid_desc && info->hid_desc_len > 0) {
        int n = info->hid_desc_len;
        if (n > GC_BT_DESC_MAX) n = GC_BT_DESC_MAX;
        memcpy(c->desc, info->hid_desc, (size_t)n);
        c->desc_len = n;
    }
}

static int origin_equal(const GcBtOrigin *a, const GcBtOrigin *b)
{
    return a && b && a->occurrence && a->occurrence == b->occurrence && a->handle == b->handle;
}
static GcBtOrigin copy_origin(const host_origin *origin)
{
    GcBtOrigin result = { origin->occurrence, origin->handle }; return result;
}
static int is_admitted(GcBt *bt, int slot, const GcBtOrigin *origin)
{
    int yes;
    pthread_mutex_lock(&bt->lock);
    yes = bt->admitted[slot] && origin_equal(origin, &bt->origins[slot]);
    pthread_mutex_unlock(&bt->lock);
    return yes;
}
static void ev_connect(void *ud, int slot, const host_origin *origin, const pad_info *info)
{
    GcBt *bt = ud;
    if (slot < 0 || slot >= GC_BT_MAX_PADS || !origin || !origin->occurrence || origin->handle > 0xfff) return;
    pthread_mutex_lock(&bt->lock);
    if (bt->origins[slot].occurrence &&
        (origin->occurrence < bt->origins[slot].occurrence ||
         (origin->occurrence == bt->origins[slot].occurrence && origin->handle != bt->origins[slot].handle))) {
        pthread_mutex_unlock(&bt->lock); return;
    }
    bt->virtual_live[slot] = 0;
    bt->req_out[slot].pending = 0;
    bt->origins[slot] = copy_origin(origin);
    bt->admitted[slot] = 0;
    pthread_mutex_unlock(&bt->lock);
    GcBtController c;
    fill_controller(&c, info);
    bt->metadata[slot] = c;
    bt->metadata_valid[slot] = 1;
    GcBtOrigin captured = copy_origin(origin);
    int accepted = bt->ops.ready && bt->ops.ready(bt->ops.ud, slot, &captured, &c) == 0;
    pthread_mutex_lock(&bt->lock);
    bt->admitted[slot] = accepted;
    pthread_mutex_unlock(&bt->lock);
}

/* Engine-thread reconciliation, never under the Bluetooth mutex. Repeated
 * ready updates metadata for the existing observed occurrence. */
static void reconcile_controller(GcBt *bt, int slot)
{
    pad_info info; pad_state state; GcBtController current; host_origin origin;
    if (!bt->host || !bt->metadata_valid[slot] ||
        !host_pad_get_qualified(bt->host, slot, &origin, &info, &state)) return;
    GcBtOrigin captured = copy_origin(&origin);
    if (!is_admitted(bt, slot, &captured)) return;
    fill_controller(&current, &info);
    if (!memcmp(&current, &bt->metadata[slot], sizeof current)) return;
    bt->metadata[slot] = current;
    if (bt->ops.ready && bt->ops.ready(bt->ops.ud, slot, &captured, &current) != 0) {
        pthread_mutex_lock(&bt->lock);
        bt->admitted[slot] = 0; bt->req_out[slot].pending = 0; bt->virtual_live[slot] = 0;
        pthread_mutex_unlock(&bt->lock);
    }
}

static void ev_report(void *ud, int slot, const host_origin *origin, const uint8_t *rep, int len)
{
    GcBt *bt = ud;
    if (slot < 0 || slot >= GC_BT_MAX_PADS) return;
    reconcile_controller(bt, slot);
    GcBtOrigin captured = copy_origin(origin);
    if (is_admitted(bt, slot, &captured) && bt->ops.report)
        bt->ops.report(bt->ops.ud, slot, &captured, rep, len);
}

static void ev_state(void *ud, int slot, const host_origin *origin, const pad_state *st)
{
    GcBt *bt = ud;
    if (slot < 0 || slot >= GC_BT_MAX_PADS) return;
    reconcile_controller(bt, slot);
    GcBtOrigin captured = copy_origin(origin);
    if (!is_admitted(bt, slot, &captured) || !bt->ops.state) return;
    GcBtPad p;
    memset(&p, 0, sizeof p);
    p.buttons = st->buttons;
    p.lx = st->lx; p.ly = st->ly; p.rx = st->rx; p.ry = st->ry;
    p.l2 = st->l2; p.r2 = st->r2;
    p.has_motion = st->has_motion;
    memcpy(p.gyro, st->gyro, sizeof p.gyro);
    memcpy(p.accel, st->accel, sizeof p.accel);
    p.has_touch = st->has_touch;
    for (int i = 0; i < 2; ++i) {
        p.touch[i].down = st->touch[i].down;
        p.touch[i].x = st->touch[i].x;
        p.touch[i].y = st->touch[i].y;
    }
    bt->ops.state(bt->ops.ud, slot, &captured, &p);
}

static void ev_disconnect(void *ud, int slot, const host_origin *origin)
{
    GcBt *bt = ud;
    if (slot < 0 || slot >= GC_BT_MAX_PADS || !origin) return;
    GcBtOrigin captured = copy_origin(origin);
    pthread_mutex_lock(&bt->lock);
    if (!origin_equal(&captured, &bt->origins[slot])) { pthread_mutex_unlock(&bt->lock); return; }
    bt->virtual_live[slot] = 0;
    bt->req_out[slot].pending = 0;
    pthread_mutex_unlock(&bt->lock);
    pthread_mutex_lock(&bt->lock);
    bt->admitted[slot] = 0;
    memset(&bt->origins[slot], 0, sizeof bt->origins[slot]);
    pthread_mutex_unlock(&bt->lock);
    bt->metadata_valid[slot] = 0;
    if (bt->ops.lost)
        bt->ops.lost(bt->ops.ud, slot, &captured);
}

/* ---- transport lifecycle ------------------------------------------------- */

static void note_failure(GcBt *bt, const char *what)
{
    pthread_mutex_lock(&bt->lock);
    snprintf(bt->last_failure, sizeof bt->last_failure, "%s", what);
    bt->last_failure_ms = now_ms();
    pthread_mutex_unlock(&bt->lock);
}

/* Engine thread: copies what the open host and its transport measured. */
static void capture_radio(GcBt *bt)
{
    host_diag hd;
    GcBtDiag *d = &bt->radio_diag;

    if (!bt->host) return;
    host_diag_get(bt->host, &hd);
    d->measured = 1;
    d->have_usb = hd.transport.have_device;
    d->usb_vid = hd.transport.vid;
    d->usb_pid = hd.transport.pid;
    d->iface = hd.transport_measured ? hd.transport.iface : -1;
    d->ep_events = hd.transport.ep_events;
    d->ep_acl_in = hd.transport.ep_acl_in;
    d->ep_acl_out = hd.transport.ep_acl_out;
    d->hci_functions = hd.transport.hci_functions;
    d->endpoints_from_descriptor = hd.transport.from_descriptor;
    d->acl_mtu = hd.acl_mtu;
    d->acl_buffers = hd.acl_buffers;
    d->le = hd.le;
    d->reads = hd.transport.reads_done;
    d->reads_empty = hd.transport.reads_empty;
    d->events = hd.transport_measured ? hd.transport.events_seen : hd.events_handled;
    d->acl = hd.transport_measured ? hd.transport.acl_seen : hd.acl_handled;
    d->event_queue_drops = hd.transport.event_ring_drops;
    d->acl_queue_drops = hd.transport.acl_ring_drops;
    d->partial_event_drops = hd.transport.stream_drops;
    d->acl_stale = hd.acl_stale;
    d->acl_malformed = hd.acl_malformed;
    d->tx_no_credit = hd.tx_no_credit;
    d->tx_failed = hd.tx_failed;
    d->probes = hd.probes_sent;
    d->reset_attempts = hd.reset_attempts;
    d->at_last_rx_ms = hd.last_rx_ms;
}

static void set_state(GcBt *bt, int state, const char *reason)
{
    pthread_mutex_lock(&bt->lock);
    bt->state = state;
    snprintf(bt->reason, sizeof bt->reason, "%s", reason ? reason : "");
    pthread_mutex_unlock(&bt->lock);
}

static host_events bt_events(GcBt *bt)
{
    host_events ev = {0};
    ev.ud = bt;
    ev.on_connect_qualified = ev_connect;
    ev.on_state_qualified = ev_state;
    ev.on_disconnect_qualified = ev_disconnect;
    ev.on_report_qualified = ev_report;
    return ev;
}

static void engine_try_open(GcBt *bt)
{
    hci_t hci = {0};
    int attempt = ++bt->attempt;
    pthread_mutex_lock(&bt->lock);
    bt->open_attempts++;
    pthread_mutex_unlock(&bt->lock);
    if (gc_bt_open_hci(&hci)) {
        host_events ev = bt_events(bt);
        int opened = host_open_checked(hci, bt->db_path, &ev, &bt->host);
        if (opened == 0) {
            bt->attempt = 0;
            memset(&bt->radio_diag, 0, sizeof bt->radio_diag); /* a new radio session */
            capture_radio(bt);
            set_state(bt, BT_READY, "");
            return;
        }
        if (bt->host) {
            /* The transport opened but host setup failed, so this attempt did
             * fail: count it, or the UI undercounts failed opens. */
            pthread_mutex_lock(&bt->lock);
            bt->open_failures++;
            pthread_mutex_unlock(&bt->lock);
            bt->release_error = "bt_host_setup_cleanup_pending";
            set_state(bt, BT_FAILED, "Bluetooth setup cleanup pending");
            return;
        }
        if (hci.ops) {
            if (!hci.ops->close_checked || hci.ops->close_checked(hci.ctx) != 0) {
                bt->retained_hci = hci; bt->retained_hci_owned = 1;
                pthread_mutex_lock(&bt->lock);
                bt->open_failures++;
                pthread_mutex_unlock(&bt->lock);
                set_state(bt, BT_FAILED, "Bluetooth transport cleanup pending");
                return;
            }
        }
        pthread_mutex_lock(&bt->lock);
        bt->open_failures++;
        pthread_mutex_unlock(&bt->lock);
        note_failure(bt, "controller did not answer the host setup");
    } else {
        if (hci.ops && (!hci.ops->close_checked || hci.ops->close_checked(hci.ctx) != 0)) {
            bt->retained_hci = hci; bt->retained_hci_owned = 1;
            set_state(bt, BT_FAILED, "Bluetooth partial-open cleanup pending");
            return;
        }
        note_failure(bt, "Bluetooth USB transport did not open");
    }
    pthread_mutex_lock(&bt->lock);
    bt->open_failures++;
    pthread_mutex_unlock(&bt->lock);
    if (attempt >= BT_OPEN_TRIES)
        set_state(bt, BT_FAILED,
                  "The radio did not open. Another payload may already own it. Exit that payload, then Retry.");
    else
        bt->next_try = now_ms() + BT_RETRY_MS;
}

static void engine_lost(GcBt *bt)
{
    capture_radio(bt); /* keep the counters as they were at the loss */
    pthread_mutex_lock(&bt->lock);
    bt->transport_losses++;
    pthread_mutex_unlock(&bt->lock);
    note_failure(bt, "Bluetooth transport stopped responding");
    /* A lost transport is released: the radio must not keep reporting itself
     * open just because the host object survives its failed close. The radio
     * session's counters are already captured above and the release error
     * below keeps pairing/output fail-closed until an explicit Retry. */
    if (host_close_checked(bt->host) != 0) {
        bt->release_error = "bt_host_release_failed";
        host_abandon(bt->host); /* release failed: free it, stay fail-closed */
    }
    bt->host = NULL;
    set_state(bt, BT_FAILED,
              "The Bluetooth transport stopped responding. Use Retry in the Toolbox.");
}

/* ---- requests ------------------------------------------------------------ */

static void engine_service_requests(GcBt *bt)
{
    int retry_pending = 0;
    pthread_mutex_lock(&bt->lock);
    retry_pending = bt->req_retry != 0;
    pthread_mutex_unlock(&bt->lock);
    /* An explicit Retry is the user's remedy for a failed release: clear the
     * fail-closed latch so the radio can be reopened. Ownership of an HCI still
     * pending release keeps blocking until it is actually released. */
    if (retry_pending && !bt->retained_hci_owned) bt->release_error = NULL;
    if (bt->release_error || bt->retained_hci_owned) return;
    int pair = 0, retry = 0, forget = 0, enabled;
    unsigned char forget_addr[6];
    GcBtOutReq out_req[GC_BT_MAX_PADS];

    pthread_mutex_lock(&bt->lock);
    enabled = bt->enabled;
    /* Pair/forget are host operations. Keep them queued while the radio is
     * still opening so a Toolbox request is not discarded before a host
     * exists. */
    if (bt->host && bt->req_pair_seconds > 0) {
        pair = bt->req_pair_seconds;
        bt->req_pair_seconds = 0;
    }
    if (bt->req_retry) { retry = 1; bt->req_retry = 0; }
    if (bt->host && bt->req_forget) {
        forget = 1;
        memcpy(forget_addr, bt->req_forget_addr, 6);
        bt->req_forget = 0;
    }
    for (int i = 0; i < GC_BT_MAX_PADS; ++i) {
        out_req[i] = bt->req_out[i];
        bt->req_out[i].pending = 0;
    }
    pthread_mutex_unlock(&bt->lock);

    if (!enabled) {
        pthread_mutex_lock(&bt->lock);
        if (bt->enabled) {
            /* Enable landed after this pass sampled the flag. Do not drop
             * the pair request or force the radio back off. */
            pthread_mutex_unlock(&bt->lock);
            return;
        }
        bt->req_pair_seconds = 0;
        bt->req_forget = 0;
        pthread_mutex_unlock(&bt->lock);
        if (bt->host) {
            capture_radio(bt);
            if (host_close_checked(bt->host) == 0) bt->host = NULL;
            else bt->release_error = "bt_host_release_failed";
        }
        set_state(bt, BT_STOPPED, "Bluetooth is turned off.");
        return;
    }

    if (retry && !bt->host && !bt->retained_hci_owned) {
        bt->attempt = 0;
        bt->next_try = 0;
        set_state(bt, BT_TRYING, "");
    }
    if (bt->host && forget)
        (void)host_forget(bt->host, forget_addr);
    if (bt->host && pair)
        (void)host_pair(bt->host, pair);
    if (bt->host)
        for (int i = 0; i < GC_BT_MAX_PADS; ++i)
            if (out_req[i].pending && is_admitted(bt, i, &out_req[i].origin)) {
                host_origin origin; pad_info info; pad_state state;
                if (host_pad_get_qualified(bt->host, i, &origin, &info, &state) &&
                    origin.occurrence == out_req[i].origin.occurrence && origin.handle == out_req[i].origin.handle)
                    (void)host_set_output(bt->host, i, &out_req[i].out);
            }
}

/* ---- status cache -------------------------------------------------------- */

static void engine_refresh_snapshot(GcBt *bt)
{
    GcBtStatus s;
    memset(&s, 0, sizeof s);
    s.ok = 1;
    snprintf(s.version, sizeof s.version, "%s", GHOSTTOOTH_VERSION);

    pthread_mutex_lock(&bt->lock);
    static const char *const names[] = { "trying", "ready", "failed", "stopped" };
    snprintf(s.state, sizeof s.state, "%s", names[bt->state & 3]);
    snprintf(s.reason, sizeof s.reason, "%s", bt->reason);
    int live[GC_BT_MAX_PADS];
    for (int i = 0; i < GC_BT_MAX_PADS; ++i) live[i] = bt->virtual_live[i];
    s.diag.open_attempts = bt->open_attempts;
    s.diag.open_failures = bt->open_failures;
    s.diag.transport_losses = bt->transport_losses;
    snprintf(s.diag.last_failure, sizeof s.diag.last_failure, "%s", bt->last_failure);
    s.diag.at_last_failure_ms = bt->last_failure[0] ? bt->last_failure_ms : 0;
    pthread_mutex_unlock(&bt->lock);

    if (bt->host) capture_radio(bt);
    {
        const GcBtDiag *r = &bt->radio_diag; /* engine-thread only */
        const uint32_t attempts = s.diag.open_attempts, failures = s.diag.open_failures,
                       losses = s.diag.transport_losses;
        const long failure_at = s.diag.at_last_failure_ms;
        char failure[sizeof s.diag.last_failure];
        memcpy(failure, s.diag.last_failure, sizeof failure);
        s.diag = *r;
        s.diag.open_attempts = attempts;
        s.diag.open_failures = failures;
        s.diag.transport_losses = losses;
        s.diag.at_last_failure_ms = failure_at;
        memcpy(s.diag.last_failure, failure, sizeof failure);
        if (!r->measured) s.diag.iface = -1;
    }
    s.diag.radio_open = bt->host != NULL;
    s.diag.at_sample_ms = now_ms();

    if (bt->host) {
        s.pairing = host_pairing_left(bt->host);
        int n = host_paired_count(bt->host);
        for (int i = 0; i < n && s.paired_count < GC_BT_DB_MAX; ++i) {
            host_paired_info pi;
            if (!host_paired_get(bt->host, i, &pi)) break;
            memcpy(s.paired[s.paired_count].addr, pi.addr, 6);
            s.paired[s.paired_count].vid = pi.vid;
            s.paired[s.paired_count].pid = pi.pid;
            s.paired[s.paired_count].le = pi.le;
            s.paired[s.paired_count].connected = pi.connected;
            ++s.paired_count;
        }
        for (int slot = 0; slot < GC_BT_MAX_PADS; ++slot) {
            pad_info info;
            pad_state st;
            host_origin origin;
            if (!host_pad_get_qualified(bt->host, slot, &origin, &info, &st)) continue;
            reconcile_controller(bt, slot);
            int k = s.controller_count++;
            s.controllers[k].slot = slot + 1;
            snprintf(s.controllers[k].profile, sizeof s.controllers[k].profile,
                     "%s", info.profile ? info.profile : "");
            memcpy(s.controllers[k].addr, info.addr, 6);
            s.controllers[k].vid = info.vid;
            s.controllers[k].pid = info.pid;
            s.controllers[k].le = info.le;
            s.controllers[k].virtual_pad = live[slot];
        }
    }

    pthread_mutex_lock(&bt->lock);
    bt->snap = s;
    bt->observed_release_blocker = bt->host ? host_release_blocker(bt->host) : NULL;
    if (bt->retained_hci_owned) {
        const hci_ops *ops = bt->retained_hci.ops;
        if (!ops || !ops->close_checked)
            bt->observed_release_blocker = "bt_transport_checked_close_unavailable";
        else if (ops->release_blocker)
            bt->observed_release_blocker = ops->release_blocker(bt->retained_hci.ctx);
    }
    pthread_mutex_unlock(&bt->lock);
}

/* ---- thread -------------------------------------------------------------- */

static void *engine_thread(void *arg)
{
    GcBt *bt = arg;
    while (!bt->stop) {
        engine_service_requests(bt);
        int enabled;
        int state;
        pthread_mutex_lock(&bt->lock);
        enabled = bt->enabled;
        state = bt->state;
        pthread_mutex_unlock(&bt->lock);

        if (enabled && !bt->host && !bt->retained_hci_owned && state == BT_TRYING && now_ms() >= bt->next_try)
            engine_try_open(bt);

        if (bt->host && !bt->release_error) {
            host_poll(bt->host, 4);
            if (host_transport_lost(bt->host))
                engine_lost(bt);
        } else {
            sleep_ms(4);
        }
        engine_refresh_snapshot(bt);
    }
    if (bt->host) {
        if (host_close_checked(bt->host) == 0) bt->host = NULL;
        else bt->worker_close_failed = 1;
    }
    set_state(bt, BT_STOPPED, "stopped");
    engine_refresh_snapshot(bt);
    return NULL;
}

/* ---- public API ---------------------------------------------------------- */

static GcBt *bt_start(const GcBtOps *ops, const char *db_path, int enabled)
{
    GcBt *bt = calloc(1, sizeof *bt);
    if (!bt) return NULL;
    if (ops) bt->ops = *ops;
    snprintf(bt->db_path, sizeof bt->db_path, "%s",
             db_path ? db_path : "/data/ghostcontrol/bt/controllers.db");
    if (pthread_mutex_init(&bt->lock, NULL) != 0) { free(bt); return NULL; }
    {
        char logpath[300];
        const char *slash = strrchr(bt->db_path, '/');
        int dir = slash ? (int)(slash - bt->db_path) : 1;
        snprintf(logpath, sizeof logpath, "%.*s/bluetooth.log",
                 dir, slash ? bt->db_path : ".");
        bt->log_owned = log_open(logpath) != 0;
        /* open may acquire the stream and then fail while publishing/unlocking.
         * A failed checked logger retains its uncertain owner, not an empty bit. */
        if (!bt->log_owned && log_release_blocker && log_release_blocker()) {
            bt->log_owned = 1;
            atomic_store(&bt->stop, 1);
            set_state(bt, BT_FAILED, "Bluetooth logger ownership uncertain");
            engine_refresh_snapshot(bt);
            return bt;
        }
    }
    bt->enabled = enabled ? 1 : 0;
    bt->state = enabled ? BT_TRYING : BT_STOPPED;
    engine_refresh_snapshot(bt);
    if (pthread_create(&bt->thread, NULL, engine_thread, bt) != 0) {
        atomic_store(&bt->stop, 1);
        set_state(bt, BT_FAILED, "Bluetooth worker creation failed");
        engine_refresh_snapshot(bt);
        /* A non-NULL failed owner is intentional when cleanup is uncertain. */
        if (gc_bt_stop_checked(bt) == 0) return NULL;
        return bt;
    }
    bt->thread_ok = 1;
    return bt;
}

GcBt *gc_bt_start(const GcBtOps *ops, const char *db_path)
{
    return bt_start(ops, db_path, 1);
}

GcBt *gc_bt_start_disabled(const GcBtOps *ops, const char *db_path)
{
    return bt_start(ops, db_path, 0);
}

const char *gc_bt_release_blocker(GcBt *bt)
{
    if (!bt) return NULL;
    if (bt->lock_uncertain) return "bt_lock_destroy_uncertain";
    if (bt->log_owned) {
        if (!log_close_checked) return "bt_log_checked_close_unavailable";
        if (log_release_blocker) {
            const char *why = log_release_blocker();
            if (why) return why;
        }
    }
    /* The worker alone owns host and partial-open HCI while joinable.
     * No unsafe snapshots/getters, no prediction of future API failures. */
    if (bt->thread_ok) {
        if (pthread_mutex_lock(&bt->lock) != 0) return "bt_lock_unavailable";
        const char *why = bt->observed_release_blocker;
        pthread_mutex_unlock(&bt->lock);
        return why;
    }
    if (bt->host) return host_release_blocker(bt->host);
    if (bt->retained_hci_owned) {
        const hci_ops *ops = bt->retained_hci.ops;
        if (!ops || !ops->close_checked) return "bt_transport_checked_close_unavailable";
        if (ops->release_blocker) return ops->release_blocker(bt->retained_hci.ctx);
    }
    return NULL;
}
int gc_bt_stop_checked(GcBt *bt)
{
    if (!bt) return 0;
    atomic_store(&bt->stop, 1);
    if (bt->thread_ok) {
        if (pthread_join(bt->thread, NULL) != 0) return -1;
        bt->thread_ok = 0;
        if (bt->worker_close_failed) { bt->worker_close_failed = 0; return -1; }
    }
    if (bt->lock_uncertain) return -1;
    if (bt->host) {
        if (host_close_checked(bt->host) != 0) return -1;
        bt->host = NULL;
    }
    if (bt->retained_hci_owned) {
        if (!bt->retained_hci.ops || !bt->retained_hci.ops->close_checked) return -1;
        if (bt->retained_hci.ops->release_blocker &&
            bt->retained_hci.ops->release_blocker(bt->retained_hci.ctx)) return -1;
        if (bt->retained_hci.ops->close_checked(bt->retained_hci.ctx) != 0) return -1;
        bt->retained_hci_owned = 0;
    }
    if (bt->log_owned) {
        if (!log_close_checked || log_close_checked() != 0) return -1;
        bt->log_owned = 0;
    }
    if (pthread_mutex_destroy(&bt->lock) != 0) { bt->lock_uncertain = 1; return -1; }
    free(bt);
    return 0;
}
void gc_bt_stop(GcBt *bt) { (void)gc_bt_stop_checked(bt); }

void gc_bt_set_enabled(GcBt *bt, int enabled)
{
    if (!bt || atomic_load(&bt->stop)) return;
    pthread_mutex_lock(&bt->lock);
    bt->enabled = enabled ? 1 : 0;
    if (enabled && bt->state == BT_STOPPED) {
        bt->state = BT_TRYING;
        bt->attempt = 0;
        bt->next_try = 0;
        bt->reason[0] = '\0';
    }
    pthread_mutex_unlock(&bt->lock);
}

int gc_bt_pair(GcBt *bt, int seconds)
{
    if (!bt || atomic_load(&bt->stop)) return -1;
    if (seconds <= 0) seconds = BT_PAIR_SECONDS;
    pthread_mutex_lock(&bt->lock);
    bt->req_pair_seconds = seconds;
    pthread_mutex_unlock(&bt->lock);
    return 0;
}

int gc_bt_retry(GcBt *bt)
{
    if (!bt || atomic_load(&bt->stop)) return -1;
    pthread_mutex_lock(&bt->lock);
    bt->req_retry = 1;
    pthread_mutex_unlock(&bt->lock);
    return 0;
}

static int parse_addr(const char *text, unsigned char addr[6])
{
    unsigned v[6];
    if (!text) return 0;
    if (sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x",
               &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6)
        return 0;
    for (int i = 0; i < 6; ++i) addr[i] = (unsigned char)v[i];
    return 1;
}

int gc_bt_forget(GcBt *bt, const char *addr)
{
    unsigned char a[6];
    if (!bt || atomic_load(&bt->stop) || !parse_addr(addr, a)) return -1;
    pthread_mutex_lock(&bt->lock);
    memcpy(bt->req_forget_addr, a, 6);
    bt->req_forget = 1;
    pthread_mutex_unlock(&bt->lock);
    return 0;
}

void gc_bt_snapshot(GcBt *bt, GcBtStatus *out)
{
    if (!out) return;
    if (!bt) {
        memset(out, 0, sizeof *out);
        out->ok = 1;
        snprintf(out->state, sizeof out->state, "stopped");
        snprintf(out->reason, sizeof out->reason, "Bluetooth is not available.");
        out->diag.iface = -1; /* at_sample_ms 0: formatted as no diagnostics */
        return;
    }
    pthread_mutex_lock(&bt->lock);
    *out = bt->snap;
    pthread_mutex_unlock(&bt->lock);
    /* Ages are computed at hand-out so a stalled engine shows as stale. */
    const long now = now_ms();
    GcBtDiag *d = &out->diag;
    d->sample_age_ms = d->at_sample_ms ? now - d->at_sample_ms : -1;
    d->last_rx_age_ms = d->at_last_rx_ms ? now - d->at_last_rx_ms : -1;
    d->last_failure_age_ms = d->at_last_failure_ms ? now - d->at_last_failure_ms : -1;
}

void gc_bt_set_virtual(GcBt *bt, int bt_slot, const GcBtOrigin *origin, int live)
{
    if (!bt || atomic_load(&bt->stop) || bt_slot < 0 || bt_slot >= GC_BT_MAX_PADS) return;
    pthread_mutex_lock(&bt->lock);
    if (!bt->admitted[bt_slot] || !origin_equal(origin, &bt->origins[bt_slot])) { pthread_mutex_unlock(&bt->lock); return; }
    bt->virtual_live[bt_slot] = live ? 1 : 0;
    pthread_mutex_unlock(&bt->lock);
}

void gc_bt_set_output(GcBt *bt, int bt_slot, const GcBtOrigin *origin, uint8_t strong, uint8_t weak,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t player)
{
    if (!bt || atomic_load(&bt->stop) || bt_slot < 0 || bt_slot >= GC_BT_MAX_PADS) return;
    pthread_mutex_lock(&bt->lock);
    if (!bt->admitted[bt_slot] || !origin_equal(origin, &bt->origins[bt_slot])) { pthread_mutex_unlock(&bt->lock); return; }
    bt->req_out[bt_slot].origin = *origin;
    bt->req_out[bt_slot].out.strong = strong;
    bt->req_out[bt_slot].out.weak = weak;
    bt->req_out[bt_slot].out.r = r;
    bt->req_out[bt_slot].out.g = g;
    bt->req_out[bt_slot].out.b = b;
    bt->req_out[bt_slot].out.player = player;
    bt->req_out[bt_slot].pending = 1;
    pthread_mutex_unlock(&bt->lock);
}

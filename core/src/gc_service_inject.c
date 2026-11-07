/* SPDX-License-Identifier: GPL-3.0-or-later
 * Turns a service-owned USB or Bluetooth pad into a virtual DualSense.
 * Identification uses the kernel log line for the device this process just
 * added. It does not attach to another process.
 */
#include "gc_service_inject.h"
#include "gc_service_session_private.h"
#include "gc_service_features.h"
#include <stdatomic.h>
#include <stdlib.h>
#include "gc_config.h"
#include "gc_rawmap.h"
#include "gc_types.h"
#include "gc_usb.h"
#include "controller_ds4.h"
#include "controller_mamba.h"
#include "controller_nintendo.h"
#include "usb_helpers.h"
#include <stdarg.h>
#include "ps5_vpad.h"
#include "host.h"
#include "log.h"
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define USB_SLOTS 4
#define USB_REPORT 64

static int inject_ready;
static int sources_active;
static uint64_t policy_generation[HOST_MAX_PADS];
static int bt_owned[HOST_MAX_PADS];
static int usb_owned[HOST_MAX_PADS];
/* Transport-local indices are not resident slot numbers. Claims/removal
 * fences share one mutex; a lost slot is reserved until teardown finishes. */
static int bt_map[GC_BT_MAX_PADS];
/* Process-lifetime per-producer history, deliberately not reset by close/init. */
static GcBtOrigin bt_seen[GC_BT_MAX_PADS];
static GcBtOrigin bt_active[GC_BT_MAX_PADS];
static int origin_valid(const GcBtOrigin *o)
{ return o && o->occurrence && o->handle <= 0xfff; }
static int origin_same(const GcBtOrigin *a, const GcBtOrigin *b)
{ return origin_valid(a) && b && a->occurrence == b->occurrence && a->handle == b->handle; }
static int bt_source[HOST_MAX_PADS];
static int slot_live[HOST_MAX_PADS];
static uint64_t slot_epoch[HOST_MAX_PADS];
/* Do not reset on init/close: a restarted resident loop must not reuse tokens. */
static uint64_t next_connection_generation;
static uint64_t next_occurrence_generation;
static pthread_once_t session_once = PTHREAD_ONCE_INIT;
static char process_session[GC_INSTANCE_SESSION_MAX];
static int session_ready;
static GcConfigStore *prepared_store;
static _Atomic int source_fault;
static _Thread_local unsigned source_depth;
static char candidate_selector[HOST_MAX_PADS][GC_INSTANCE_ID_MAX];
static int retirement_pending[HOST_MAX_PADS];
static int policy_revoked[HOST_MAX_PADS];
static void initialize_session(void)
{
    unsigned char random[16];
    static const char hex[] = "0123456789abcdef";
#if defined(__PROSPERO__)
    /* The PS5 SDK libc exports arc4random_buf but no getentropy. */
    arc4random_buf(random, sizeof random);
#else
    if (getentropy(random, sizeof random) != 0) return;
#endif
    for (size_t i = 0; i < sizeof random; ++i) {
        process_session[i*2] = hex[random[i] >> 4];
        process_session[i*2+1] = hex[random[i] & 15];
    }
    session_ready = 1;
}
static long add_retry_at[HOST_MAX_PADS];
static pad_state last_state[HOST_MAX_PADS];
static int last_valid[HOST_MAX_PADS];
/* One stderr note per slot while a device is owned but not yet decodable, so a
 * held-back virtual pad is explained once instead of every tick. */
static int no_input_noted[HOST_MAX_PADS];
static int binding_observed[HOST_MAX_PADS];
static uint16_t slot_vid[HOST_MAX_PADS];
static uint16_t slot_pid[HOST_MAX_PADS];
static int want_add[HOST_MAX_PADS];
static int want_remove[HOST_MAX_PADS];
static pad_state queued[HOST_MAX_PADS];
static int queued_valid[HOST_MAX_PADS];
static pthread_mutex_t inject_mu = PTHREAD_MUTEX_INITIALIZER;
static GcBt *inject_bt;
static GcConfigStore *inject_config;
static GcServiceSource source_info[HOST_MAX_PADS];
static uint8_t source_raw[HOST_MAX_PADS][GC_SERVICE_REPORT_MAX];
static size_t source_raw_length[HOST_MAX_PADS];
/* Descriptor-qualified physical values are separate from original normalized
 * input. The per-slot cache spans report IDs and is protected by inject_mu. */
static GcHidLayout source_hid_layout[HOST_MAX_PADS];
static GcHidState source_hid_values[HOST_MAX_PADS];
static int output_allowed[HOST_MAX_PADS];
static int backend_added[HOST_MAX_PADS];

/* Source transitions serialize with backend add/update, not with IPC readers.
 * Recursive only for same-thread source callbacks from a bounded backend call.
 * Lock order: config writer lease -> dispatch_mu -> inject_mu. Source callbacks
 * never take a config writer lease. No external operation holds inject_mu. */
static pthread_once_t dispatch_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t dispatch_mu;
static int dispatch_ready;
static void initialize_dispatch(void)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return;
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        pthread_mutex_init(&dispatch_mu, &attr) == 0) dispatch_ready = 1;
    pthread_mutexattr_destroy(&attr);
}
static int source_dispatch_lock(void)
{
    if (pthread_once(&dispatch_once, initialize_dispatch) != 0 || !dispatch_ready) return 0;
    if (pthread_mutex_lock(&dispatch_mu) != 0) {
        atomic_store_explicit(&source_fault, 1, memory_order_release); return 0;
    }
    if (source_depth == UINT_MAX) {
        atomic_store_explicit(&source_fault, 1, memory_order_release);
        pthread_mutex_unlock(&dispatch_mu); return 0;
    }
    ++source_depth;
    return 1;
}
static void source_dispatch_unlock(void)
{
    if (!source_depth) abort();
    --source_depth;
    if (pthread_mutex_unlock(&dispatch_mu) != 0) abort();
}
/* Every cache/logical-state access shares the transition fence. The hook owns
 * that fence only, so it can clear state without snapshot/writer recursion. */
static void source_snapshot_lock(void)
{
    if (!source_dispatch_lock()) abort(); /* unrecoverable synchronization fault */
    if (pthread_mutex_lock(&inject_mu) != 0) abort();
}
static void source_snapshot_unlock(void)
{
    if (pthread_mutex_unlock(&inject_mu) != 0) abort();
    source_dispatch_unlock();
}
static int begin_dispatch(int slot, GcConfigStore *config, uint64_t generation,
                          uint64_t epoch, int needs_input)
{
    if (atomic_load_explicit(&source_fault, memory_order_acquire)) return 0;
    if (config && gc_config_policy_acquire(config, generation) != 0) return 0;
    if (!source_dispatch_lock()) {
        if (config) gc_config_policy_release(config);
        return 0;
    }
    source_snapshot_lock();
    int allowed = inject_config == config && inject_ready &&
        !retirement_pending[slot] && output_allowed[slot] && policy_generation[slot] == generation &&
        slot_epoch[slot] == epoch && source_info[slot].connected &&
        source_info[slot].output_type == GC_CONTROLLER_OUTPUT_DS5 &&
        (bt_owned[slot] || usb_owned[slot]) && !want_remove[slot] &&
        (!needs_input || last_valid[slot]);
    if (allowed && config && source_info[slot].binding.identity.state == GC_INSTANCE_STABLE) {
        GcControllerResolution resolved;
        if (gc_config_resolve_controller(config, &source_info[slot].binding, &resolved) != 0 ||
            (resolved.kind == GC_RESOLVE_INSTANCE &&
             gc_config_instance_check_held(config, GC_INSTANCE_REQUIRE_LIVE, NULL, 0) != 0)) allowed = 0;
    }
    source_snapshot_unlock();
    if (!allowed) {
        source_dispatch_unlock();
        if (config) gc_config_policy_release(config);
    }
    return allowed;
}
static void end_dispatch(GcConfigStore *config)
{
    source_dispatch_unlock();
    if (config) gc_config_policy_release(config);
}

void gc_service_inject_set_config(GcConfigStore *config)
{
    source_snapshot_lock();
    inject_config = config;
    /* NULL is a quiescent constructor/destructor operation, after unregister. */
    if (!config && !sources_active) prepared_store = NULL;
    source_snapshot_unlock();
}

/* Caller holds inject_mu. Never infer virtual identity from physical input. */
static void clear_sample(int slot)
{
    binding_observed[slot] = 0;
    memset(&source_info[slot], 0, sizeof source_info[slot]);
    memset(source_raw[slot], 0, sizeof source_raw[slot]);
    source_raw_length[slot] = 0;
    memset(&source_hid_layout[slot], 0, sizeof source_hid_layout[slot]);
    memset(&source_hid_values[slot], 0, sizeof source_hid_values[slot]);
    output_allowed[slot] = 0;
    policy_generation[slot] = 0;
    candidate_selector[slot][0] = 0;
}

static void raw_sample(int slot, const uint8_t *raw, size_t length)
{
    if (length > GC_SERVICE_REPORT_MAX) length = GC_SERVICE_REPORT_MAX;
    memcpy(source_raw[slot], raw, length);
    source_raw_length[slot] = length;
}

typedef struct {
    int used;
    int fd;
    int fd_owned, fs_initialized, close_uncertain;
    int slot;
    char path[32];
    uint16_t vid, pid;
    GcUsbInterfaceKind kind;
    int interface_index, interface_number, alternate_setting;
    uint8_t in_ep;
    uint8_t report[USB_REPORT];
    /* Two pipes: index 0 reads input, index 1 carries Nintendo output reports.
     * The array is registered with USB_FS_INIT and must outlive the session. */
    struct usb_fs_endpoint ep[2];
    uint8_t *bufp;
    uint32_t lenp;
    /* Bytes requested per IN transfer: the endpoint's wMaxPacketSize, capped
     * at USB_REPORT. A transfer longer than one packet on a small interrupt
     * endpoint (e.g. 8 bytes on a DragonRise 0079:0006 pad) is not reliably
     * completed on PS5, so never ask for more than one packet. */
    uint32_t in_xfer_len;
    int busy;
    /* Nintendo pads stay silent until 0x80 0x02 / 0x80 0x04 arrive, then speak
     * 0x30/0x3F. The handshake state machine is the resident line's proven one. */
    int nintendo;
    int hs_state;
    uint8_t nintendo_seq;
    unsigned char out_buf[64];
    uint32_t out_len;
    uint8_t *out_ptr;
    ScePadData decoded; /* Preserve controls split across HID report IDs. */
} UsbFeed;

static UsbFeed usb_feed[USB_SLOTS];
static int usb_release_fd(UsbFeed *feed)
{
    if (feed->close_uncertain) return -1;
    if (!feed->fd_owned) return 0;
    if (feed->fs_initialized) {
        struct usb_fs_uninit un;
        memset(&un, 0, sizeof un);
        if (ioctl(feed->fd, USB_FS_UNINIT, &un) != 0) return -1;
        feed->fs_initialized = 0;
    }
    if (close(feed->fd) != 0) { feed->close_uncertain = 1; return -1; }
    feed->fd_owned = 0;
    feed->fd = -1;
    return 0;
}

static void remember(int slot, const pad_state *st, uint16_t vid, uint16_t pid)
{
    if (slot < 0 || slot >= HOST_MAX_PADS || !st) return;
    source_snapshot_lock();
    last_state[slot] = *st;
    last_valid[slot] = binding_observed[slot] = 1;
    if (vid) slot_vid[slot] = vid;
    if (pid) slot_pid[slot] = pid;
    source_snapshot_unlock();
}

/* Caller holds inject_mu. Reserve a source slot before calling any backend. */
static int available_slot(int preferred)
{
    if (preferred >= 0 && preferred < HOST_MAX_PADS &&
        !bt_owned[preferred] && !usb_owned[preferred] && !want_remove[preferred])
        return preferred;
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot)
        if (!bt_owned[slot] && !usb_owned[slot] && !want_remove[slot]) return slot;
    return -1;
}

static void retire_samples(int slot);

/* Caller holds inject_mu. Snapshot intent even before the first resident tick. */
static void describe_output(int slot)
{
    GcServiceSource *info = &source_info[slot];
    GcControllerResolution resolved = {0};
    int found = inject_config ? gc_config_resolve_controller(inject_config, &info->binding, &resolved) : 0;
    uint64_t generation = resolved.generation;
    if (policy_generation[slot] && generation != policy_generation[slot]) {
        int observed = binding_observed[slot];
        retire_samples(slot);
        binding_observed[slot] = observed; /* publication is not physical revocation */
        for (int i = 0; i < USB_SLOTS; ++i) if (usb_feed[i].used && usb_feed[i].slot == slot) {
            memset(&usb_feed[i].decoded, 0, sizeof usb_feed[i].decoded);
            usb_feed[i].decoded.leftStick.x = usb_feed[i].decoded.leftStick.y = 128;
            usb_feed[i].decoded.rightStick.x = usb_feed[i].decoded.rightStick.y = 128;
        }
    }
    info->output_type = resolved.has_rule ? resolved.rule.output_type : GC_CONTROLLER_OUTPUT_DS5;
    const char *error = slot_live[slot] ? "" : "backend_pending";
    if (found != 0) error = "source_binding_mismatch";
    else if (resolved.kind == GC_RESOLVE_POLICY_UNAVAILABLE) error = "instance_policy_quarantined";
    else if (info->binding.identity.state == GC_INSTANCE_COLLISION || resolved.kind == GC_RESOLVE_IDENTITY_COLLISION) error = "identity_collision";
    else if (resolved.kind == GC_RESOLVE_IDENTITY_UNAVAILABLE) error = "identity_unavailable";
    else if (resolved.kind == GC_RESOLVE_BINDING_MISMATCH) error = "source_binding_mismatch";
    else if (resolved.kind == GC_RESOLVE_PROFILE_UNAVAILABLE) error = "profile_association_unavailable";
    else if (resolved.kind == GC_RESOLVE_VETO) error = "disabled_or_ignored";
    else if (info->output_type != GC_CONTROLLER_OUTPUT_DS5) error = "unsupported_output_type";
    else if (!inject_ready) error = "backend_unavailable";
    if (retirement_pending[slot]) error = "output_retirement_pending";
    if (policy_revoked[slot]) error = "instance_policy_quarantined";
    if (atomic_load_explicit(&source_fault, memory_order_acquire)) error = "instance_capability_unavailable";
    policy_generation[slot] = generation;
    snprintf(info->output_error, sizeof info->output_error, "%s", error);
    int allowed = !*error || !strcmp(error, "backend_pending");
    if (output_allowed[slot] != allowed) {
        output_allowed[slot] = allowed;
        add_retry_at[slot] = 0;
        want_add[slot] = allowed;
    }
    if (!allowed) slot_live[slot] = 0;
}

/* Transition-fenced retirement preserves inventory, never cached input. */
static void retire_samples(int slot)
{
    binding_observed[slot] = 0;
    memset(&last_state[slot], 0, sizeof last_state[slot]);
    memset(&queued[slot], 0, sizeof queued[slot]);
    last_valid[slot] = queued_valid[slot] = slot_live[slot] = 0;
    source_info[slot].input_available = source_info[slot].virtual_live = 0;
    memset(source_raw[slot], 0, sizeof source_raw[slot]); source_raw_length[slot] = 0;
    memset(&source_hid_values[slot], 0, sizeof source_hid_values[slot]);
    memset(&source_hid_layout[slot], 0, sizeof source_hid_layout[slot]);
    output_allowed[slot] = want_add[slot] = 0;
    policy_generation[slot] = 0;
    fprintf(stderr, "inject: slot %d retire_samples (backend_added=%d)\n", slot, backend_added[slot]);
    retirement_pending[slot] = backend_added[slot];
}
static void binding_common(int slot, GcInstanceTransport transport, uint64_t occurrence)
{
    GcServiceSource *info = &source_info[slot];
    GcControllerMatch *m = &info->binding;
    memset(m, 0, sizeof *m);
    m->transport = transport;
    snprintf(m->session_id, sizeof m->session_id, "%s", process_session);
    snprintf(m->path, sizeof m->path, "%s", info->path);
    snprintf(m->protocol, sizeof m->protocol, "%s", info->protocol);
    m->vid = info->vid; m->pid = info->pid;
    m->connection_generation = info->connection_generation;
    m->occurrence_generation = occurrence;
    m->interface_index = m->interface_number = m->alternate_setting = -1;
}
static void reevaluate_collisions(void)
{
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        if (!source_info[slot].connected || !candidate_selector[slot][0]) continue;
        int collision = 0;
        for (int other = 0; other < HOST_MAX_PADS; ++other)
            if (other != slot && source_info[other].connected &&
                candidate_selector[other][0] &&
                source_info[other].binding.occurrence_generation != source_info[slot].binding.occurrence_generation &&
                !strcmp(candidate_selector[slot], candidate_selector[other])) collision = 1;
        GcControllerIdentity *id = &source_info[slot].binding.identity;
        GcInstanceState next = collision ? GC_INSTANCE_COLLISION : GC_INSTANCE_STABLE;
        if (id->state != next) retire_samples(slot);
        id->state = next;
        id->proof = collision ? GC_INSTANCE_PROOF_NONE : GC_INSTANCE_PROOF_CLASSIC_PEER;
        snprintf(id->instance_id, sizeof id->instance_id, "%s", collision ? "" : candidate_selector[slot]);
    }
}

/* Caller owns the source transition/snapshot fence. History survives retirement. */
static void retire_bt(int source)
{
    int slot = bt_map[source];
    bt_map[source] = -1;
    memset(&bt_active[source], 0, sizeof bt_active[source]);
    if (slot < 0) return;
    ++slot_epoch[slot]; want_remove[slot] = 1; want_add[slot] = 0;
    queued_valid[slot] = last_valid[slot] = slot_live[slot] = 0;
    memset(&queued[slot], 0, sizeof queued[slot]);
    memset(&last_state[slot], 0, sizeof last_state[slot]);
    slot_vid[slot] = slot_pid[slot] = 0;
    clear_sample(slot);
}
static int on_ready(void *ud, int source, const GcBtOrigin *origin, const GcBtController *controller)
{
    int slot;
    (void)ud;
    if (source < 0 || source >= GC_BT_MAX_PADS || !controller || !origin_valid(origin)) return -1;
    if (!source_dispatch_lock()) return -1;
    source_snapshot_lock();
    int existing = bt_map[source];
    int refresh = existing >= 0 && origin_same(origin, &bt_active[source]);
    if (!refresh && origin->occurrence <= bt_seen[source].occurrence) {
        source_snapshot_unlock(); source_dispatch_unlock(); return -1;
    }
    if (!refresh) {
        bt_seen[source] = *origin; /* failed adoption is also nonauthorizing history */
        if (existing >= 0) { retire_bt(source); reevaluate_collisions(); }
        existing = -1;
    }
    if (!sources_active || (existing < 0 && (
        next_connection_generation == UINT64_MAX || next_occurrence_generation == UINT64_MAX ||
        (slot = available_slot(source)) < 0))) {
        source_snapshot_unlock();
        source_dispatch_unlock();
        return -1;
    }
    uint64_t connection, occurrence;
    if (existing >= 0) {
        slot = existing;
        connection = source_info[slot].connection_generation;
        occurrence = source_info[slot].binding.occurrence_generation;
        retire_samples(slot);
    } else {
        connection = ++next_connection_generation;
        occurrence = ++next_occurrence_generation;
    }
    bt_map[source] = slot;
    bt_active[source] = *origin;
    bt_source[slot] = source;
    bt_owned[slot] = 1;
    ++slot_epoch[slot];
    want_add[slot] = 1;
    add_retry_at[slot] = 0;
    queued_valid[slot] = last_valid[slot] = slot_live[slot] = 0;
    slot_vid[slot] = controller->vid;
    slot_pid[slot] = controller->pid;
    clear_sample(slot);
    GcServiceSource *info = &source_info[slot];
    info->connection_generation = connection;
    snprintf(info->path, sizeof info->path, "bt-%02X:%02X:%02X:%02X:%02X:%02X",
             controller->addr[0], controller->addr[1], controller->addr[2],
             controller->addr[3], controller->addr[4], controller->addr[5]);
    /* Host profile is a display name, not a persisted protocol token. BT
     * controllers otherwise arrive through HID, not USB Xbox GIP/XUSB. */
    snprintf(info->protocol, sizeof info->protocol, "%s",
             ds4_is_compatible_vidpid(controller->vid, controller->pid) ||
             !strcmp(controller->profile, "DualShock 4") ||
             !strcmp(controller->profile, "ds4") ? "ds4" : "hid");
    snprintf(info->name, sizeof info->name, "%s", controller->profile);
    snprintf(info->transport, sizeof info->transport, "bluetooth");
    info->vid = controller->vid; info->pid = controller->pid;
    info->slot = slot; info->connected = 1;
    binding_common(slot, controller->instance_transport, occurrence);
    unsigned nonzero = 0, nonff = 0;
    for (size_t i = 0; i < 6; ++i) { nonzero |= controller->addr[i]; nonff |= controller->addr[i] ^ 255u; }
    if (session_ready && nonzero && nonff &&
        controller->instance_transport == GC_INSTANCE_TRANSPORT_CLASSIC &&
        controller->instance_proof == GC_INSTANCE_PROOF_CLASSIC_PEER) {
        snprintf(candidate_selector[slot], sizeof candidate_selector[slot],
                 "bt-classic:%02X%02X%02X%02X%02X%02X", controller->addr[0],
                 controller->addr[1], controller->addr[2], controller->addr[3], controller->addr[4], controller->addr[5]);
    }
    reevaluate_collisions();
    describe_output(slot);
    source_snapshot_unlock();
    source_dispatch_unlock();
    return 0;
}

static void on_state(void *ud, int slot, const GcBtOrigin *origin, const GcBtPad *pad)
{
    pad_state st;
    (void)ud;
    if (!pad || slot < 0 || slot >= HOST_MAX_PADS) return;
    memset(&st, 0, sizeof st);
    st.buttons = pad->buttons;
    st.lx = pad->lx;
    st.ly = pad->ly;
    st.rx = pad->rx;
    st.ry = pad->ry;
    st.l2 = pad->l2;
    st.r2 = pad->r2;
    st.has_motion = pad->has_motion;
    if (pad->has_motion) {
        memcpy(st.gyro, pad->gyro, sizeof st.gyro);
        memcpy(st.accel, pad->accel, sizeof st.accel);
    }
    st.has_touch = pad->has_touch;
    for (int i = 0; i < 2; ++i) {
        st.touch[i].down = pad->touch[i].down;
        st.touch[i].x = pad->touch[i].x;
        st.touch[i].y = pad->touch[i].y;
    }
    source_snapshot_lock();
    slot = origin_same(origin, &bt_active[slot]) ? bt_map[slot] : -1;
    if (slot >= 0 && bt_owned[slot] && !want_remove[slot] &&
        source_info[slot].binding.identity.state != GC_INSTANCE_COLLISION &&
        !policy_revoked[slot] && !atomic_load_explicit(&source_fault, memory_order_acquire)) {
        queued[slot] = st;
        queued_valid[slot] = 1;
        last_state[slot] = st;
        last_valid[slot] = binding_observed[slot] = 1;
        source_info[slot].input_available = 1;
    }
    source_snapshot_unlock();
}

static void on_report(void *ud, int source, const GcBtOrigin *origin, const uint8_t *raw, int length)
{
    (void)ud;
    if (!raw || length <= 0 || source < 0 || source >= GC_BT_MAX_PADS) return;
    source_snapshot_lock();
    int slot = origin_same(origin, &bt_active[source]) ? bt_map[source] : -1;
    if (slot >= 0 && bt_owned[slot] && !want_remove[slot] &&
        source_info[slot].binding.identity.state != GC_INSTANCE_COLLISION &&
        !policy_revoked[slot] && !atomic_load_explicit(&source_fault, memory_order_acquire))
        raw_sample(slot, raw, (size_t)length);
    source_snapshot_unlock();
}

static void on_lost(void *ud, int source, const GcBtOrigin *origin)
{
    (void)ud;
    if (source < 0 || source >= GC_BT_MAX_PADS) return;
    if (!source_dispatch_lock()) return;
    source_snapshot_lock();
    if (origin_same(origin, &bt_active[source])) {
        retire_bt(source);
        reevaluate_collisions();
    }
    source_snapshot_unlock();
    source_dispatch_unlock();
}

static const GcBtOps inject_ops = {
    NULL, on_ready, on_report, on_state, on_lost
};

const GcBtOps *gc_service_inject_ops(void)
{
    return &inject_ops;
}

void gc_service_inject_set_bt(GcBt *bt)
{
    source_snapshot_lock();
    inject_bt = bt;
    source_snapshot_unlock();
}

static void clear_source_cache(void)
{
    source_snapshot_lock();
    memset(bt_active, 0, sizeof bt_active);
    memset(bt_owned, 0, sizeof bt_owned);
    memset(usb_owned, 0, sizeof usb_owned);
    memset(last_state, 0, sizeof last_state);
    memset(last_valid, 0, sizeof last_valid);
    memset(slot_vid, 0, sizeof slot_vid);
    memset(slot_pid, 0, sizeof slot_pid);
    memset(want_add, 0, sizeof want_add);
    memset(want_remove, 0, sizeof want_remove);
    memset(queued, 0, sizeof queued);
    memset(queued_valid, 0, sizeof queued_valid);
    memset(slot_live, 0, sizeof slot_live);
    memset(slot_epoch, 0, sizeof slot_epoch);
    memset(add_retry_at, 0, sizeof add_retry_at);
    memset(backend_added, 0, sizeof backend_added);
    memset(retirement_pending, 0, sizeof retirement_pending);
    memset(policy_revoked, 0, sizeof policy_revoked);
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) clear_sample(slot);
    for (int i = 0; i < GC_BT_MAX_PADS; ++i) bt_map[i] = -1;
    for (int i = 0; i < HOST_MAX_PADS; ++i) bt_source[i] = -1;
    source_snapshot_unlock();
}

void gc_service_inject_init(void)
{
    int i;
    if (sources_active && gc_service_inject_close_checked() != 0) return;
    for (i = 0; i < USB_SLOTS; ++i)
        if (usb_feed[i].fd_owned || usb_feed[i].close_uncertain) return;
    if (pthread_once(&session_once, initialize_session) != 0) session_ready = 0;
    if (!source_dispatch_lock()) return;
    clear_source_cache();
    for (i = 0; i < USB_SLOTS; ++i) usb_feed[i].fd = -1;
    sources_active = 1;
    inject_ready = vpad_init();
    if (!inject_ready) {
        fprintf(stderr, "inject: virtual DualSense setup did not start\n");
    } else {
        fprintf(stderr, "inject: backend initialized; no virtual device confirmed yet\n");
    }
    source_dispatch_unlock();
}

const char *gc_service_inject_release_blocker(void)
{
    if (!source_dispatch_lock()) return "inject_dispatch_unavailable";
    const char *why = NULL;
    for (int i = 0; i < USB_SLOTS; ++i)
        if (usb_feed[i].close_uncertain) { why = "inject_usb_close_uncertain"; break; }
    if (!why) why = vpad_release_blocker();
    source_dispatch_unlock();
    return why;
}
int gc_service_inject_close_checked(void)
{
    if (!source_dispatch_lock()) return -1;
    for (int i = 0; i < USB_SLOTS; ++i) {
        if (usb_release_fd(&usb_feed[i]) != 0) {
            source_dispatch_unlock(); return -1;
        }
    }
    if (vpad_shutdown_checked() != 0) { source_dispatch_unlock(); return -1; }
    memset(usb_feed, 0, sizeof usb_feed);
    for (int i = 0; i < USB_SLOTS; ++i) usb_feed[i].fd = -1;
    inject_ready = 0;
    sources_active = 0;
    inject_bt = NULL;
    clear_source_cache();
    source_dispatch_unlock();
    return 0;
}
void gc_service_inject_close(void) { (void)gc_service_inject_close_checked(); }

static void apply_bt(long now)
{
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        int remove, add;
        uint64_t epoch, generation;
        GcConfigStore *config;
        if (!source_dispatch_lock()) continue;
        source_snapshot_lock();
        remove = want_remove[slot];
        epoch = slot_epoch[slot];
        source_snapshot_unlock();
        if (remove) {
            if (inject_ready) vpad_remove(slot);
            source_snapshot_lock();
            if (epoch == slot_epoch[slot]) {
                want_remove[slot] = bt_owned[slot] = backend_added[slot] = 0;
                bt_source[slot] = -1;
            }
            source_snapshot_unlock();
            source_dispatch_unlock();
            continue;
        }
        source_dispatch_unlock();
        source_snapshot_lock();
        epoch = slot_epoch[slot];
        generation = policy_generation[slot];
        config = inject_config;
        add = inject_ready && bt_owned[slot] && output_allowed[slot] &&
              want_add[slot] && now >= add_retry_at[slot];
        source_snapshot_unlock();
        if (add && begin_dispatch(slot, config, generation, epoch, 0)) {
            int accepted = vpad_add(slot);
            source_snapshot_lock();
            if (epoch == slot_epoch[slot] && !want_remove[slot]) {
                if (accepted) { want_add[slot] = 0; backend_added[slot] = 1; }
                else add_retry_at[slot] = now + 1000;
            }
            source_snapshot_unlock();
            end_dispatch(config);
            if (!accepted) fprintf(stderr, "inject: backend add slot %d refused; retry pending\n", slot);
        }
        source_snapshot_lock();
        if (bt_owned[slot] && !want_remove[slot] && queued_valid[slot]) {
            last_state[slot] = queued[slot];
            last_valid[slot] = binding_observed[slot] = 1;
            queued_valid[slot] = 0;
        }
        source_snapshot_unlock();
    }
}

/* The reservation is atomic with the Bluetooth ready callback. */
static int free_slot(void)
{
    int slot;
    if (!source_dispatch_lock()) return -1;
    source_snapshot_lock();
    slot = next_connection_generation == UINT64_MAX || next_occurrence_generation == UINT64_MAX ? -1 : available_slot(-1);
    if (slot >= 0) {
        usb_owned[slot] = 1;
        ++slot_epoch[slot];
        last_valid[slot] = slot_live[slot] = 0;
        clear_sample(slot);
        source_info[slot].connection_generation = ++next_connection_generation;
    }
    source_snapshot_unlock();
    source_dispatch_unlock();
    return slot;
}

/* Read one coherent configuration generation before publishing output policy.
 * Source epochs remain independent: policy edits never release physical slots. */
static void refresh_policy(void)
{
    source_snapshot_lock();
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        GcServiceSource *info = &source_info[slot];
        if (!info->connected || want_remove[slot]) continue;
        describe_output(slot);
    }
    source_snapshot_unlock();
    /* Removal runs outside the snapshot lock: backend callbacks may lose sources. */
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        if (!source_dispatch_lock()) continue;
        source_snapshot_lock();
        int remove = backend_added[slot] && !output_allowed[slot] && !want_remove[slot];
        if (remove) backend_added[slot] = 0;
        source_snapshot_unlock();
        if (remove && inject_ready) vpad_remove(slot);
        source_dispatch_unlock();
    }
}

/* Caller holds inject_mu: physical values and original input share a slot. */
static void remap_state(int slot, const GcProfile *profile, pad_state *state)
{
    ScePadData pad;
    memset(&pad, 0, sizeof pad);
    pad.buttons = state->buttons;
    pad.leftStick.x = state->lx; pad.leftStick.y = state->ly;
    pad.rightStick.x = state->rx; pad.rightStick.y = state->ry;
    pad.analogButtons.l2 = state->l2; pad.analogButtons.r2 = state->r2;
    if (!strcmp(source_info[slot].protocol, "hid") && source_hid_values[slot].count)
        gc_hid_apply_profile_values(&source_hid_layout[slot],
            source_hid_values[slot].values, source_hid_values[slot].count, profile, &pad);
    /* Physical bindings precede semantic transforms; neither touches source input. */
    gc_profile_apply(profile, &pad);
    state->buttons = pad.buttons;
    state->lx = pad.leftStick.x; state->ly = pad.leftStick.y;
    state->rx = pad.rightStick.x; state->ry = pad.rightStick.y;
    state->l2 = pad.analogButtons.l2; state->r2 = pad.analogButtons.r2;
}

static void push_sources(void)
{
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        pad_state state;
        uint64_t epoch;
        int active, valid, live;
        source_snapshot_lock();
        active = inject_ready && output_allowed[slot] &&
                 (bt_owned[slot] || usb_owned[slot]) && !want_remove[slot];
        valid = last_valid[slot];
        state = last_state[slot];
        epoch = slot_epoch[slot];
        GcConfigStore *config = inject_config;
        GcControllerResolution resolved = {0};
        int result = config ? gc_config_resolve_controller(config, &source_info[slot].binding, &resolved) : 0;
        uint64_t generation = resolved.generation;
        if (result || (config && generation != policy_generation[slot]) ||
            (resolved.kind != GC_RESOLVE_NONE && resolved.kind != GC_RESOLVE_MODEL && resolved.kind != GC_RESOLVE_INSTANCE)) active = 0;
        if (resolved.has_profile) remap_state(slot, &resolved.profile, &state);
        source_snapshot_unlock();
        live = active && vpad_live(slot);
        if (live && valid) {
            /* Probe first; exclude store commits only across bounded dispatch.
             * Backend operations must not mutate/join writers on this store. */
            if (begin_dispatch(slot, config, generation, epoch, 1)) {
                vpad_update(slot, &state);
                end_dispatch(config);
                live = vpad_live(slot);
                if (config && gc_config_generation(config) != generation) live = 0;
            } else live = 0;
        }
        source_snapshot_lock();
        if (epoch == slot_epoch[slot] && !want_remove[slot]) {
            slot_live[slot] = live;
            source_info[slot].virtual_live = live;
            source_info[slot].input_available = last_valid[slot];
            if (output_allowed[slot])
                snprintf(source_info[slot].output_error, sizeof source_info[slot].output_error,
                         "%s", live ? "" : "backend_pending");
        }
        source_snapshot_unlock();
    }
    if (source_dispatch_lock()) {
        int virtual_live[GC_BT_MAX_PADS];
        GcBtOrigin origins[GC_BT_MAX_PADS];
        source_snapshot_lock();
        GcBt *bt = inject_bt;
        for (int source = 0; source < GC_BT_MAX_PADS; ++source) {
            int slot = bt_map[source];
            virtual_live[source] = slot >= 0 && slot_live[slot];
            origins[source] = bt_active[source];
        }
        source_snapshot_unlock();
        source_dispatch_unlock();
        if (bt)
            for (int source = 0; source < GC_BT_MAX_PADS; ++source)
                gc_bt_set_virtual(bt, source, &origins[source], virtual_live[source]);
    }
}

static int usb_xusb_qualified(const GcUsbDevice *device)
{
    /* kind comes from descriptor selection, not payload sniffing. Recheck the
     * selected interface metadata; VID/PID alone never grants admission. */
    return device && device->kind == GC_USB_INTERFACE_XUSB && !device->hid &&
        device->alternate_setting == 0 && device->interface_index >= 0 &&
        device->interface_number >= 0 && device->interface_number <= 255 &&
        mamba_is_xinput_interface(device->subclass, device->protocol) &&
        device->in_count > 0 && device->in_count <= 15 &&
        (device->in[0] & 0x80u) && (device->in[0] & 0x0fu) &&
        !(device->in[0] & 0x70u);
}

static int usb_supported(const GcUsbDevice *device)
{
    if (!device) return 0;
    if (usb_xusb_qualified(device)) return 1;
    if ((device->kind == GC_USB_INTERFACE_HID || device->kind == GC_USB_INTERFACE_HID_RAW) &&
        ds4_is_compatible_vidpid(device->vid, device->pid)) return 1;
    return device->kind == GC_USB_INTERFACE_HID && device->layout.count > 0;
}

/* Compare descriptor meaning, not C padding or unused field storage. */
static int hid_layout_same(const GcHidLayout *left, const GcHidLayout *right)
{
    if (left->count != right->count || left->count > GC_HID_MAX_FIELDS ||
        left->numbered != right->numbered || left->axes != right->axes ||
        left->features != right->features) return 0;
    for (size_t index = 0; index < left->count; ++index) {
        const GcHidField *a = &left->fields[index], *b = &right->fields[index];
        if (a->offset != b->offset || a->page != b->page || a->usage != b->usage ||
            a->array_base != b->array_base || a->bits != b->bits || a->report != b->report ||
            a->minimum != b->minimum || a->maximum != b->maximum ||
            a->array_usage_min != b->array_usage_min || a->array_usage_max != b->array_usage_max ||
            a->is_array != b->is_array || a->null_state != b->null_state) return 0;
    }
    return 1;
}

/* The profile a slot resolved to, if the wizard saved one for this controller.
 * Raw bit maps are the only way in for a device whose descriptor declares no
 * standard usages, and they must be usable without shipping a new app. */
static int slot_profile(int slot, GcProfile *out)
{
    int have = 0;
    if (!out || slot < 0 || slot >= HOST_MAX_PADS) return 0;
    source_snapshot_lock();
    GcConfigStore *config = inject_config;
    GcControllerResolution resolved = {0};
    if (config &&
        gc_config_resolve_controller(config, &source_info[slot].binding, &resolved) == 0 &&
        resolved.has_profile) {
        *out = resolved.profile;
        have = 1;
    }
    source_snapshot_unlock();
    return have;
}

static int usb_decode(UsbFeed *feed, const GcUsbDevice *device, pad_state *state)
{
    ScePadData decoded = feed->decoded;
    int ok;
    /* Bring-up trace: reports either arrive and decode, or they never arrive.
     * Without this the two look identical from outside ("no input"). */
    {
        static int traced;
        if (traced < 16) {
            ++traced;
            log_line("usb feed: slot %d len=%u id=%#x kind=%d",
                     feed->slot, (unsigned)feed->lenp,
                     (unsigned)(feed->lenp ? feed->report[0] : 0),
                     (int)device->kind);
        }
    }
    if (feed->nintendo) {
        /* The resident line's Nintendo state machine: it answers the 0x81
         * handshake reports by sending subcommands over the OUT pipe and only
         * then parses 0x30/0x3F. A generic HID decode never sees a report at all. */
        const int handled = nintendo_handle_packet(feed->fd, feed->ep, feed->report,
                                                   feed->lenp, &feed->hs_state,
                                                   &feed->nintendo_seq, &decoded);
        ok = handled > 0;
        if (handled < 0)
            log_line("usb: %s nintendo OUT failed (%s)", feed->path, strerror(errno));
    } else if (usb_xusb_qualified(device))
        ok = mamba_xinput_handle_packet(-1, NULL, feed->report, feed->lenp, &decoded);
    else if ((device->kind == GC_USB_INTERFACE_HID || device->kind == GC_USB_INTERFACE_HID_RAW) &&
        ds4_is_compatible_vidpid(device->vid, device->pid))
        ok = ds4_handle_packet(-1, NULL, feed->report, feed->lenp, &decoded);
    else if (device->kind == GC_USB_INTERFACE_HID) {
        source_snapshot_lock();
        GcHidState physical = source_hid_values[feed->slot];
        if (!hid_layout_same(&source_hid_layout[feed->slot], &device->layout)) {
            memset(&physical, 0, sizeof physical);
            memset(&decoded, 0, sizeof decoded);
            decoded.leftStick.x = decoded.leftStick.y = 128;
            decoded.rightStick.x = decoded.rightStick.y = 128;
            decoded.quat.w = 1.0f;
        }
        ok = gc_hid_input(&device->layout, feed->report, feed->lenp, &decoded) &&
             gc_hid_state_update(&physical, &device->layout, feed->report, feed->lenp);
        /* Cache the descriptor even when the decode fails: a report with no
         * standard usages decodes to nothing (gc_hid_input returns 0), yet its
         * field layout is exactly what the setup learner needs to know which
         * bytes carry buttons. */
        if (device->layout.count || device->layout.raw_count)
            source_hid_layout[feed->slot] = device->layout;
        if (ok)
            source_hid_values[feed->slot] = physical;
        source_snapshot_unlock();
        /* A saved profile applies to every HID source. It can add bindings the
         * descriptor cannot express, and for a pad that declares no standard
         * usages it is the only decoder there is. Profiles are data files, so a
         * newly supported controller arrives without an app update. */
        GcProfile slot_prof;
        if (slot_profile(feed->slot, &slot_prof)) {
            if (!ok) {
                memset(&decoded, 0, sizeof decoded);
                decoded.leftStick.x = decoded.leftStick.y = 128;
                decoded.rightStick.x = decoded.rightStick.y = 128;
                decoded.quat.w = 1.0f;
            }
            gc_profile_apply(&slot_prof, &decoded);
            gc_rawmap_apply(&slot_prof, feed->report, feed->lenp, &decoded);
            ok = 1;   /* configured by profile, even when nothing mapped yet */
        }
    } else
        return 0; /* An unknown protocol is not a DS4 report. */
    if (!ok) {
        /* A descriptor that declares no standard usages yields no fields, and
         * with no saved profile yet there is nothing to map. The report is
         * still real input from a real device: publish a neutral state so the
         * source counts as readable and Custom Controller Setup can learn its
         * raw bits. Without this the wizard refuses to list the only device it
         * could use to build the profile -- a deadlock, not a dead pad. */
        if (feed->lenp > 0) {
            memset(&decoded, 0, sizeof decoded);
            decoded.leftStick.x = decoded.leftStick.y = 128;
            decoded.rightStick.x = decoded.rightStick.y = 128;
            decoded.quat.w = 1.0f;
            feed->decoded = decoded;
            pad_state_reset(state);
            return 1;
        }
        return 0;
    }
    feed->decoded = decoded;
    pad_state_reset(state);
    state->buttons = decoded.buttons;
    state->lx = decoded.leftStick.x;
    state->ly = decoded.leftStick.y;
    state->rx = decoded.rightStick.x;
    state->ry = decoded.rightStick.y;
    state->l2 = decoded.analogButtons.l2;
    state->r2 = decoded.analogButtons.r2;
    return 1;
}

/* Switch Pro and compatibles: they answer nothing until the two 0x80 reports
 * arrive, which is why they look dead to a generic HID bring-up. */
static int usb_is_nintendo(uint16_t vid, uint16_t pid)
{
    return vid == 0x057e &&
           (pid == 0x2009 || pid == 0x2017 || pid == 0x2019 || pid == 0x201a);
}

/* controller_nintendo.c and usb_helpers.c log through the resident line's status
 * log; vpad.log is this build's retrievable equivalent. */
void ghostpad_status_log(const char *fmt, ...)
{
    char buffer[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof buffer, fmt, ap);
    va_end(ap);
    log_line("%s", buffer);
}

/* The two reports the resident line sends before its handshake state machine. */
static void usb_nintendo_kick(UsbFeed *feed)
{
    const int ok1 = usb_send_cmd(feed->fd, &feed->ep[1], 0x80, 0x02) == 0;
    usleep(30000);
    const int ok2 = usb_send_cmd(feed->fd, &feed->ep[1], 0x80, 0x04) == 0;
    usleep(50000);
    log_line("usb: %s nintendo kick [80 02]=%d [80 04]=%d", feed->path, ok1, ok2);
}

static int usb_open_feed(UsbFeed *feed, const GcUsbDevice *device)
{
    struct usb_fs_init init;
    struct usb_fs_open op;
    if (feed->fd_owned || feed->close_uncertain) return 0;
    memset(feed, 0, sizeof *feed);
    feed->fd = -1;
    if (!device->in_count || !usb_supported(device)) return 0;
    feed->decoded.leftStick.x = feed->decoded.leftStick.y = 128;
    feed->decoded.rightStick.x = feed->decoded.rightStick.y = 128;
    feed->decoded.quat.w = 1.0f;
    feed->fd = open(device->path, O_RDWR | O_NONBLOCK);
    if (feed->fd < 0) return 0;
    feed->fd_owned = 1;
    /* The kernel HID driver owns the interface on a stock console. Without
     * detaching it first, our interrupt transfer never completes: FS_COMPLETE
     * stays EBUSY forever and the pad reports no input at all, for every HID
     * device rather than just the one that exposed it. The resident line
     * (gc_main.c) always detaches before it opens a pad; this path did not. */
    {
        int detach_index = device->interface_index;
        if (ioctl(feed->fd, USB_IFACE_DRIVER_DETACH, &detach_index) != 0) {
            static int noted_detach;
            if (noted_detach++ < 4)
                log_line("usb: %s detach iface %d failed errno=%d (%s)",
                         device->path, detach_index, errno, strerror(errno));
        }
    }
    /* Same story as the detach: the resident line sends SET_IDLE(0) after it
     * opens a HID pad and this path did not. A device that waits for traffic
     * before it starts reporting has nothing to answer until this arrives. */
    if (device->kind == GC_USB_INTERFACE_HID)
        (void)gc_usb_hid_set_idle(feed->fd, (uint8_t)device->interface_number, 0);
    feed->slot = -1;
    feed->bufp = feed->report;
    feed->lenp = USB_REPORT;
    memset(feed->ep, 0, sizeof feed->ep);
    feed->ep[0].ppBuffer = (void **)&feed->bufp;
    feed->ep[0].pLength = &feed->lenp;
    feed->ep[0].nFrames = 1;
    feed->ep[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    feed->nintendo = usb_is_nintendo(device->vid, device->pid) && device->out_count;
    const int ep_max = feed->nintendo ? 2 : 1;
    /* ep[1] is left zeroed on purpose: usb_send_out() owns the OUT buffers and
     * refuses to write through an endpoint that already has pointers installed. */
    memset(&init, 0, sizeof init);
    init.pEndpoints = feed->ep;
    init.ep_index_max = ep_max;
    if (ioctl(feed->fd, USB_FS_INIT, &init) != 0) {
        (void)usb_release_fd(feed);
        return 0;
    }
    feed->fs_initialized = 1;
    memset(&op, 0, sizeof op);
    op.ep_index = 0;
    op.ep_no = device->in[0];
    op.max_bufsize = USB_REPORT;
    op.max_frames = 1;
    if (ioctl(feed->fd, USB_FS_OPEN, &op) != 0) {
        (void)usb_release_fd(feed);
        return 0;
    }
    feed->in_xfer_len = (op.max_packet_length > 0 && op.max_packet_length <= USB_REPORT)
                            ? op.max_packet_length : USB_REPORT;
    feed->lenp = feed->in_xfer_len;
    if (feed->nintendo) {
        memset(&op, 0, sizeof op);
        op.ep_index = 1;
        op.ep_no = device->out[0];
        op.max_bufsize = sizeof feed->out_buf;
        op.max_frames = 1;
        if (ioctl(feed->fd, USB_FS_OPEN, &op) != 0) {
            log_line("usb: %s OUT open errno=%d (%s)", device->path, errno, strerror(errno));
            (void)usb_release_fd(feed);
            return 0;
        }
        /* Ask the pad to start talking before anything waits on its IN pipe. */
        usb_nintendo_kick(feed);
        feed->hs_state = HS_WAIT_81_01;
        feed->nintendo_seq = 1;
    }
    snprintf(feed->path, sizeof feed->path, "%s", device->path);
    feed->vid = device->vid; feed->pid = device->pid; feed->kind = device->kind;
    feed->interface_index = device->interface_index;
    feed->interface_number = device->interface_number;
    feed->alternate_setting = device->alternate_setting;
    feed->in_ep = device->in[0];
    feed->used = 1;
    feed->slot = -1;
    return 1;
}

static void usb_close_feed(UsbFeed *feed)
{
    if (!feed->used) return;
    if (!source_dispatch_lock()) return;
    if (usb_release_fd(feed) != 0) { source_dispatch_unlock(); return; }
    if (feed->slot >= 0) {
        source_snapshot_lock();
        ++slot_epoch[feed->slot];
        last_valid[feed->slot] = slot_live[feed->slot] = 0;
        slot_vid[feed->slot] = slot_pid[feed->slot] = 0;
        clear_sample(feed->slot);
        backend_added[feed->slot] = 0;
        source_snapshot_unlock();
        if (inject_ready) vpad_remove(feed->slot);
        source_snapshot_lock();
        usb_owned[feed->slot] = 0;
        source_snapshot_unlock();
    }
    memset(feed, 0, sizeof *feed);
    feed->fd = -1;
    source_dispatch_unlock();
}

/* 1 if the feed was retired. Transient faults stay on the same slot. */
static int usb_note_io(UsbFeed *feed, int err, int endpoint_status, int short_invalid)
{
    GcUsbIoClass kind = gc_usb_io_classify(err, endpoint_status, short_invalid);
    if (kind == GC_USB_IO_OK) return 0;
    if (kind == GC_USB_IO_FATAL) {
        fprintf(stderr, "inject: usb %s retired: fatal io (errno=%d endpoint_status=%#x short_invalid=%d)\n",
                feed->path, err, endpoint_status, short_invalid);
        usb_close_feed(feed);
        return 1;
    }
    if (feed->slot >= 0) {
        source_snapshot_lock();
        if (source_info[feed->slot].connected &&
            source_info[feed->slot].io_faults != UINT_MAX)
            ++source_info[feed->slot].io_faults;
        source_snapshot_unlock();
    }
    return 0;
}

/* A path is a location, not an identity or endpoint binding. Retire the old
 * source before accepting an observed model/interface change at that location.
 * A transient HID->HID_RAW classification downgrade (the report-descriptor
 * read can fail while this service holds the interface) is not a model
 * change: the physical identity and endpoints are unchanged, so the source
 * is kept and the scan is tolerated until qualification returns. */
static int usb_same_source(const UsbFeed *feed, const GcUsbDevice *device)
{
    if (!device->in_count || strcmp(feed->path, device->path) ||
        feed->vid != device->vid || feed->pid != device->pid ||
        feed->interface_index != device->interface_index ||
        feed->interface_number != device->interface_number ||
        feed->alternate_setting != device->alternate_setting ||
        feed->in_ep != device->in[0])
        return 0;
    if (usb_supported(device))
        return 1;
    /* While this service holds the interface, the report-descriptor read can
     * fail transiently, downgrading the scan's classification from HID to
     * HID_RAW for one tick. The physical identity and endpoints are
     * unchanged, so keep the source; a genuine loss of qualification (a HID
     * scan whose parsed layout is gone) still retires it. */
    return feed->kind == GC_USB_INTERFACE_HID &&
        device->kind == GC_USB_INTERFACE_HID_RAW;
}

static void usb_tick(void)
{
    GcUsbDevice devices[8];
    size_t count, i, n;
    count = gc_usb_devices(devices, 8);
    for (n = 0; n < USB_SLOTS; ++n) {
        int still = 0;
        if (!usb_feed[n].used) continue;
        for (i = 0; i < count; ++i)
            if (usb_same_source(&usb_feed[n], &devices[i])) {
                still = 1;
                if (usb_feed[n].kind != devices[i].kind)
                    fprintf(stderr, "inject: usb %s classification %d->%d tolerated (identity unchanged, source kept)\n",
                            usb_feed[n].path, (int)usb_feed[n].kind, (int)devices[i].kind);
            }
        if (!still) {
            size_t j; int seen = 0;
            for (j = 0; j < count; ++j)
                if (!strcmp(usb_feed[n].path, devices[j].path)) {
                    seen = 1;
                    fprintf(stderr, "inject: usb %s retired: field change dev[vid %04x pid %04x kind %d iface %d/%d alt %d in %u in_count %u] feed[vid %04x pid %04x kind %d iface %d/%d alt %d in %u]\n",
                            usb_feed[n].path, devices[j].vid, devices[j].pid, devices[j].kind,
                            devices[j].interface_index, devices[j].interface_number, devices[j].alternate_setting,
                            devices[j].in[0], devices[j].in_count,
                            usb_feed[n].vid, usb_feed[n].pid, usb_feed[n].kind,
                            usb_feed[n].interface_index, usb_feed[n].interface_number,
                            usb_feed[n].alternate_setting, usb_feed[n].in_ep);
                }
            if (!seen)
                fprintf(stderr, "inject: usb %s retired: absent from scan (count=%zu)\n", usb_feed[n].path, count);
            usb_close_feed(&usb_feed[n]);
        }
    }
    for (i = 0; i < count; ++i) {
        int have = 0;
        UsbFeed *feed = NULL;
        struct usb_fs_start st;
        struct usb_fs_complete done;
        pad_state state;
        for (n = 0; n < USB_SLOTS; ++n)
            if (usb_feed[n].used && !strcmp(usb_feed[n].path, devices[i].path)) {
                have = 1;
                feed = &usb_feed[n];
                break;
            }
        if (!have) {
            for (n = 0; n < USB_SLOTS; ++n)
                if (!usb_feed[n].used) { feed = &usb_feed[n]; break; }
            if (!feed || !usb_open_feed(feed, &devices[i])) {
                /* stderr is not captured anywhere a session can read, so the one
                 * line that explains "no input" was invisible. */
                log_line("usb: %s %04x:%04x open failed (%s)",
                         devices[i].path, devices[i].vid, devices[i].pid,
                         strerror(errno));
                continue;
            }
            log_line("usb: %s %04x:%04x opened", devices[i].path,
                     devices[i].vid, devices[i].pid);
        }
        if (feed->slot < 0) {
            int slot = free_slot();
            if (slot < 0) continue;
            feed->slot = slot;
            source_snapshot_lock();
            GcServiceSource *info = &source_info[slot];
            snprintf(info->path, sizeof info->path, "%s", devices[i].path);
            snprintf(info->protocol, sizeof info->protocol, "%s",
                     usb_xusb_qualified(&devices[i]) ? "xusb" :
                     ds4_is_compatible_vidpid(devices[i].vid, devices[i].pid) ? "ds4" : "hid");
            if (usb_xusb_qualified(&devices[i]))
                snprintf(info->name, sizeof info->name, "%s", mamba_name(devices[i].vid, devices[i].pid));
            else
                snprintf(info->name, sizeof info->name, "USB %04x:%04x", devices[i].vid, devices[i].pid);
            snprintf(info->transport, sizeof info->transport, "usb");
            info->vid = devices[i].vid; info->pid = devices[i].pid;
            info->slot = slot; info->connected = 1;
            uint64_t occurrence = 0;
            for (int other = 0; other < HOST_MAX_PADS; ++other)
                if (other != slot && source_info[other].connected &&
                    source_info[other].binding.transport == GC_INSTANCE_TRANSPORT_USB &&
                    !strcmp(source_info[other].path, info->path)) occurrence = source_info[other].binding.occurrence_generation;
            if (!occurrence) occurrence = ++next_occurrence_generation;
            binding_common(slot, GC_INSTANCE_TRANSPORT_USB, occurrence);
            info->binding.interface_index = devices[i].interface_index;
            info->binding.interface_number = devices[i].interface_number;
            info->binding.alternate_setting = devices[i].alternate_setting;
            info->binding.in_endpoint = devices[i].in[0];
            info->binding.out_endpoint = devices[i].out_count ? devices[i].out[0] : 0;
            describe_output(slot);
            source_snapshot_unlock();
        }
        /* A HID device can be silent on its interrupt pipe until it is asked for
         * a report: a Switch Pro Controller over USB sends nothing at all until
         * polled. GET_REPORT is standard HID and works on any device, so ask for
         * the first described input report before waiting on the pipe forever.
         * Only feeds that have never delivered a sample are polled, so a pad
         * that already streams correctly is never disturbed. */
        if (devices[i].kind == GC_USB_INTERFACE_HID) {
            static unsigned polls[USB_SLOTS];
            static int polled_ok[USB_SLOTS];
            const unsigned index = (unsigned)(feed - usb_feed);
            int streaming_now;
            source_snapshot_lock();
            streaming_now = feed->slot >= 0 && last_valid[feed->slot];
            source_snapshot_unlock();
            if (index < USB_SLOTS && !polled_ok[index] && !streaming_now &&
                ++polls[index] >= 6) {
                uint8_t reply[USB_REPORT];
                struct usb_ctl_request poll;
                const unsigned report_id =
                    devices[i].layout.numbered && devices[i].layout.count
                    ? devices[i].layout.fields[0].report : 0;
                memset(&poll, 0, sizeof poll);
                memset(reply, 0, sizeof reply);
                poll.ucr_data = reply;
                poll.ucr_flags = USB_SHORT_XFER_OK;
                poll.ucr_request.bmRequestType = 0x81;      /* device to host, class */
                poll.ucr_request.bRequest = 6;              /* GET_REPORT */
                USETW(poll.ucr_request.wValue, (uint16_t)(0x0100u | report_id));
                USETW(poll.ucr_request.wIndex, (uint16_t)devices[i].interface_number);
                USETW(poll.ucr_request.wLength, (uint16_t)sizeof reply);
                const int rc = ioctl(feed->fd, USB_DO_REQUEST, &poll);
                const int poll_errno = errno;
                static int noted;
                if (noted++ < 4)
                    log_line("usb: %s GET_REPORT id=%#x rc=%d actlen=%d errno=%d",
                             devices[i].path, report_id, rc, poll.ucr_actlen, poll_errno);
                if (rc == 0 &&
                    poll.ucr_actlen > 0 && (size_t)poll.ucr_actlen <= sizeof reply) {
                    memcpy(feed->report, reply, (size_t)poll.ucr_actlen);
                    feed->lenp = (uint32_t)poll.ucr_actlen;
                    polls[index] = 0;
                    source_snapshot_lock();
                    raw_sample(feed->slot, feed->report, feed->lenp);
                    source_snapshot_unlock();
                    if (usb_decode(feed, &devices[i], &state)) {
                        polled_ok[index] = 1;
                        remember(feed->slot, &state, devices[i].vid, devices[i].pid);
                        continue;
                    }
                }
            }
        }
        if (!feed->busy) {
            feed->lenp = feed->in_xfer_len ? feed->in_xfer_len : USB_REPORT;
            feed->ep[0].nFrames = 1;
            feed->ep[0].aFrames = 0;
            feed->ep[0].status = 0;
            memset(&st, 0, sizeof st);
            st.ep_index = 0;
            if (ioctl(feed->fd, USB_FS_START, &st) == 0) feed->busy = 1;
            else {
                int err = errno;
                static int noted_start;
                if (noted_start++ < 4)
                    log_line("usb: %s FS_START failed errno=%d (%s)",
                             feed->path, err, strerror(err));
                if (usb_note_io(feed, err, 0, 0)) continue;
                continue;
            }
        }
        memset(&done, 0, sizeof done);
        if (ioctl(feed->fd, USB_FS_COMPLETE, &done) != 0) {
            int err = errno;
            static int noted_complete;
            if (noted_complete++ < 6)
                log_line("usb: %s FS_COMPLETE failed errno=%d (%s) busy=%d",
                         feed->path, err, strerror(err), feed->busy);
            /* In-flight EAGAIN/ETIMEDOUT keeps busy so the transfer is not restarted. */
            if (usb_note_io(feed, err, 0, 0)) continue;
            continue;
        }
        if (feed->ep[0].status) {
            int status = feed->ep[0].status;
            static int noted_status;
            if (noted_status++ < 6)
                log_line("usb: %s endpoint status %#x", feed->path, status);
            feed->busy = 0;
            if (usb_note_io(feed, 0, status, 0)) continue;
            continue;
        }
        feed->busy = 0;
        if (done.ep_index != 0 || feed->ep[0].aFrames != 1 ||
            feed->lenp == 0 || feed->lenp > sizeof feed->report) {
            usb_note_io(feed, 0, 0, 1);
            continue;
        }
        source_snapshot_lock();
        if (policy_revoked[feed->slot] || atomic_load_explicit(&source_fault,memory_order_acquire)) {
            source_snapshot_unlock(); continue;
        }
        raw_sample(feed->slot, feed->report, feed->lenp);
        source_snapshot_unlock();
        if (!usb_decode(feed, &devices[i], &state)) continue;
        remember(feed->slot, &state, devices[i].vid, devices[i].pid);
    }
}

void gc_service_inject_tick(long now)
{
    source_snapshot_lock();
    int active = sources_active;
    GcConfigStore *recovery_config = inject_config;
    source_snapshot_unlock();
    if (!active) return;
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        if (!source_dispatch_lock()) return;
        if (retirement_pending[slot]) {
            fprintf(stderr, "inject: slot %d retiring (retirement_pending)\n", slot);
            if (inject_ready) vpad_remove(slot);
            backend_added[slot] = retirement_pending[slot] = 0;
        }
        source_dispatch_unlock();
    }
    /* Recovery never reuses samples captured during quarantine. Writer precedes
     * source, and only a fresh complete publication reopens sample collection. */
    if (recovery_config && gc_config_policy_acquire(recovery_config,gc_config_generation(recovery_config)) == 0) {
        if (source_dispatch_lock()) {
            for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
                if (policy_revoked[slot] && !retirement_pending[slot]) {
                    GcControllerResolution resolved;
                    if (gc_config_resolve_controller(recovery_config,&source_info[slot].binding,&resolved) == 0 &&
                        resolved.kind != GC_RESOLVE_POLICY_UNAVAILABLE) policy_revoked[slot] = 0;
                }
            }
            source_dispatch_unlock();
        }
        gc_config_policy_release(recovery_config);
    }
    if (source_dispatch_lock()) { usb_tick(); source_dispatch_unlock(); }
    refresh_policy();
    apply_bt(now);
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        char held_path[96];
        char held_protocol[24];
        held_path[0] = 0;
        held_protocol[0] = 0;
        source_snapshot_lock();
        /* Readable input is required before the virtual pad is announced, for
         * every protocol. A device that is owned but whose reports do not decode
         * yet used to be announced anyway (only "xusb" was gated), and the system
         * then raises "Who's using this controller?" locked to that virtual
         * device: nothing can move the selection, so the console softlocks.
         * Announce only a pad we can actually drive. */
        int add = inject_ready && usb_owned[slot] && output_allowed[slot] &&
                  last_valid[slot] &&
                  !backend_added[slot] && now >= add_retry_at[slot];
        int waiting_for_input = inject_ready && usb_owned[slot] && output_allowed[slot] &&
                  !last_valid[slot] && !backend_added[slot];
        if (waiting_for_input) {
            snprintf(held_path, sizeof held_path, "%s",
                     source_info[slot].path[0] ? source_info[slot].path : "source");
            snprintf(held_protocol, sizeof held_protocol, "%s",
                     source_info[slot].protocol[0] ? source_info[slot].protocol : "unknown");
        }
        uint64_t epoch = slot_epoch[slot], generation = policy_generation[slot];
        GcConfigStore *config = inject_config;
        source_snapshot_unlock();
        if (waiting_for_input && !no_input_noted[slot]) {
            /* Say why no virtual pad appeared, in the log a session can retrieve
             * over FTP. Silence here is what made the softlock look like an
             * input-forwarding fault with no evidence either way. */
            no_input_noted[slot] = 1;
            log_line("slot %d: virtual pad held back, no readable input from %s (%s)",
                     slot, held_path, held_protocol);
        }
        if (!waiting_for_input && !backend_added[slot]) no_input_noted[slot] = 0;
        if (add && begin_dispatch(slot, config, generation, epoch, 0)) {
            int accepted = vpad_add(slot);
            source_snapshot_lock();
            if (epoch == slot_epoch[slot] && !want_remove[slot]) {
                if (accepted) backend_added[slot] = 1;
                else add_retry_at[slot] = now + 1000;
            }
            source_snapshot_unlock();
            end_dispatch(config);
        }
    }
    if (inject_ready) vpad_poll(now);
    refresh_policy();
    push_sources();
}

int gc_service_inject_live(int slot)
{
    int live;
    if (slot < 0 || slot >= HOST_MAX_PADS) return 0;
    source_snapshot_lock();
    live = slot_live[slot];
    source_snapshot_unlock();
    return live;
}

int gc_service_inject_identity(int slot, uint16_t *vid, uint16_t *pid)
{
    int valid;
    if (slot < 0 || slot >= HOST_MAX_PADS) return 0;
    source_snapshot_lock();
    valid = last_valid[slot];
    if (vid) *vid = valid ? slot_vid[slot] : 0;
    if (pid) *pid = valid ? slot_pid[slot] : 0;
    source_snapshot_unlock();
    return valid;
}

static int copy_state(pad_state st, ScePadData *out)
{
    memset(out, 0, sizeof *out);
    out->buttons = st.buttons;
    out->leftStick.x = st.lx;
    out->leftStick.y = st.ly;
    out->rightStick.x = st.rx;
    out->rightStick.y = st.ry;
    out->analogButtons.l2 = st.l2;
    out->analogButtons.r2 = st.r2;
    out->quat.w = 1.0f;
    if (st.has_motion) {
        /* Match the normalized Sony sensor-count convention of this source.
         * Calibration/alternate sensor scales need per-protocol handling. */
        out->vel.x = st.gyro[0] / 1024.0f * 0.01745329252f;
        out->vel.y = st.gyro[1] / 1024.0f * 0.01745329252f;
        out->vel.z = st.gyro[2] / 1024.0f * 0.01745329252f;
        out->accel.x = st.accel[0] / 8192.0f;
        out->accel.y = st.accel[1] / 8192.0f;
        out->accel.z = st.accel[2] / 8192.0f;
    }
    if (st.has_touch) {
        for (int i = 0; i < 2; ++i) {
            if (!st.touch[i].down) continue;
            ScePadTouch *touch = &out->touchData.touch[out->touchData.fingers++];
            touch->finger = (uint8_t)i;
            touch->x = st.touch[i].x;
            touch->y = st.touch[i].y;
        }
    }
    out->connected = 1;
    return 1;
}

int gc_service_inject_copy(int slot, ScePadData *out)
{
    if (!out || slot < 0 || slot >= HOST_MAX_PADS) return 0;
    memset(out, 0, sizeof *out);
    source_snapshot_lock();
    int valid = last_valid[slot];
    if (valid) copy_state(last_state[slot], out);
    source_snapshot_unlock();
    return valid;
}

size_t gc_service_inject_sources(GcServiceSource *out, size_t capacity)
{
    size_t count = 0;
    source_snapshot_lock();
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        if (!source_info[slot].connected || want_remove[slot]) continue;
        if (out && count < capacity) {
            out[count] = source_info[slot];
            out[count].input_available = last_valid[slot];
            out[count].virtual_live = slot_live[slot];
        }
        ++count;
    }
    source_snapshot_unlock();
    return out ? (count < capacity ? count : capacity) : count;
}

int gc_service_inject_read_source(const char *path, ScePadData *out,
                                 uint8_t *raw, size_t raw_capacity,
                                 size_t *raw_length)
{
    int valid = 0;
    if (out) memset(out, 0, sizeof *out);
    if (raw_length) *raw_length = 0;
    if (!path || !out) return 0;
    source_snapshot_lock();
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        if (!source_info[slot].connected || want_remove[slot] ||
            strcmp(source_info[slot].path, path)) continue;
        size_t length = source_raw_length[slot];
        if (length > raw_capacity) length = raw_capacity;
        if (!raw) length = 0;
        if (length) memcpy(raw, source_raw[slot], length);
        if (raw_length) *raw_length = length;
        valid = last_valid[slot];
        if (valid) copy_state(last_state[slot], out);
        break;
    }
    source_snapshot_unlock();
    return valid;
}

static int match_equal(const GcControllerMatch *a, const GcControllerMatch *b)
{
    return a->transport == b->transport &&
        !strncmp(a->session_id, b->session_id, sizeof a->session_id) &&
        !strncmp(a->path, b->path, sizeof a->path) &&
        a->connection_generation == b->connection_generation &&
        a->occurrence_generation == b->occurrence_generation &&
        a->vid == b->vid && a->pid == b->pid &&
        !strncmp(a->protocol, b->protocol, sizeof a->protocol) &&
        a->interface_index == b->interface_index && a->interface_number == b->interface_number &&
        a->alternate_setting == b->alternate_setting &&
        a->in_endpoint == b->in_endpoint && a->out_endpoint == b->out_endpoint &&
        a->identity.state == b->identity.state && a->identity.proof == b->identity.proof &&
        !strncmp(a->identity.instance_id, b->identity.instance_id, sizeof a->identity.instance_id);
}

/* Caller holds inject_mu. A path/model alone cannot reject same-model ABA. */
static int checked_source_slot(const GcServiceSource *expected)
{
    if (!expected || !expected->connection_generation || !sources_active) return -1;
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        const GcServiceSource *current = &source_info[slot];
        if (current->connected && !want_remove[slot] && binding_observed[slot] &&
            current->connection_generation == expected->connection_generation &&
            current->vid == expected->vid && current->pid == expected->pid &&
            !strncmp(current->path, expected->path, sizeof current->path) &&
            !strncmp(current->protocol, expected->protocol, sizeof current->protocol) &&
            match_equal(&current->binding, &expected->binding) &&
            !policy_revoked[slot] && !atomic_load_explicit(&source_fault, memory_order_acquire)) return slot;
    }
    return -1;
}


static void digital_span_from_layout(const GcHidLayout *layout,
                                     unsigned *offset, uint32_t *mask)
{
    const GcHidField *lists[2];
    size_t counts[2];
    size_t l, i;
    unsigned last = 0, w, shift, noise_start = 32;
    int any = 0;
    *offset = 0; *mask = 0;
    if (!layout) return;
    /* Both arrays matter: the parsed list holds the claimed byte-wide fields,
     * the raw list holds everything the descriptor declared, including the
     * non-byte-aligned counter that must not be treated as a control. */
    lists[0] = layout->fields; counts[0] = layout->count;
    lists[1] = layout->raw_fields; counts[1] = layout->raw_count;
    for (l = 0; l < 2; ++l)
        for (i = 0; i < counts[l]; ++i) {
            const GcHidField *f = &lists[l][i];
            if (f->bits > 1 && (f->offset % 8u) != 0) {
                const unsigned byte = f->offset / 8u;
                if (byte < noise_start) noise_start = byte;
            }
        }
    for (l = 0; l < 2; ++l)
        for (i = 0; i < counts[l]; ++i) {
            const GcHidField *f = &lists[l][i];
            unsigned end;
            if (f->bits == 0 || f->bits > 8) continue;
            end = (f->offset + f->bits - 1u) / 8u;
            if (end >= noise_start) continue;
            if (!any) { last = end; any = 1; }
            else if (end > last) last = end;
        }
    if (!any) return;
    /* Buttons sit after the analog bytes: anchor on the last digital byte and
     * keep one byte of headroom in front of it (the 32-bit mask limit). */
    w = last >= 1 ? last - 1 : 0;
    shift = layout->numbered ? 1u : 0u;
    for (l = 0; l < 2; ++l)
        for (i = 0; i < counts[l]; ++i) {
            const GcHidField *f = &lists[l][i];
            unsigned bit;
            if (f->bits == 0 || f->bits > 8) continue;
            if ((f->offset + f->bits - 1u) / 8u >= noise_start) continue;
            for (bit = 0; bit < f->bits; ++bit) {
                const unsigned absolute = f->offset + bit;
                const unsigned byte = absolute / 8u;
                if (byte < w || byte > w + 3) continue;
                *mask |= 1u << ((byte - w) * 8u + (absolute % 8u));
            }
        }
    if (!*mask) return;
    *offset = w + shift;
}

int gc_service_inject_source_digital_span(int slot, const char *path,
                                          unsigned *offset, uint32_t *mask)
{
    GcUsbDevice devices[HOST_MAX_PADS];
    size_t count, i;
    if (!offset || !mask) return -1;
    *offset = 0; *mask = 0;
    if (slot >= 0 && slot < HOST_MAX_PADS) {
        source_snapshot_lock();
        digital_span_from_layout(&source_hid_layout[slot], offset, mask);
        source_snapshot_unlock();
    }
    if (*mask) return 0;
    if (!path) return -1;
    count = gc_usb_devices(devices, HOST_MAX_PADS);
    {
        size_t k; int found = 0;
        for (k = 0; k < count; ++k)
            if (!strcmp(devices[k].path, path)) {
                found = 1;
                log_line("span: slot=%d path=%s inv=%zu fields=%zu raw=%zu kind=%d"
                         " f0=%u/%u f1=%u/%u f2=%u/%u r0=%u/%u r1=%u/%u",
                         slot, path, count, devices[k].layout.count,
                         devices[k].layout.raw_count, (int)devices[k].kind,
                         devices[k].layout.count > 0 ? devices[k].layout.fields[0].offset : 0,
                         devices[k].layout.count > 0 ? devices[k].layout.fields[0].bits : 0,
                         devices[k].layout.count > 1 ? devices[k].layout.fields[1].offset : 0,
                         devices[k].layout.count > 1 ? devices[k].layout.fields[1].bits : 0,
                         devices[k].layout.count > 2 ? devices[k].layout.fields[2].offset : 0,
                         devices[k].layout.count > 2 ? devices[k].layout.fields[2].bits : 0,
                         devices[k].layout.raw_count > 0 ? devices[k].layout.raw_fields[0].offset : 0,
                         devices[k].layout.raw_count > 0 ? devices[k].layout.raw_fields[0].bits : 0,
                         devices[k].layout.raw_count > 1 ? devices[k].layout.raw_fields[1].offset : 0,
                         devices[k].layout.raw_count > 1 ? devices[k].layout.raw_fields[1].bits : 0);
            }
        if (!found)
            log_line("span: slot=%d path=%s not in inventory (count=%zu)", slot, path, count);
    }
    for (i = 0; i < count; ++i) {
        const char *have = devices[i].path;
        const char *want = path;
        if (!strncmp(have, "/dev/", 5)) have += 5;
        if (!strncmp(want, "/dev/", 5)) want += 5;
        if (strcmp(have, want)) continue;
        digital_span_from_layout(&devices[i].layout, offset, mask);
        break;
    }
    return *mask ? 0 : -1;
}

int gc_service_inject_read_source_checked(const GcServiceSource *expected,
    ScePadData *out, uint8_t *raw, size_t raw_capacity, size_t *raw_length)
{
    if (out) memset(out, 0, sizeof *out);
    if (raw && raw_capacity) memset(raw, 0, raw_capacity);
    if (raw_length) *raw_length = 0;
    if (!out) return 0;
    source_snapshot_lock();
    int slot = checked_source_slot(expected);
    if (slot >= 0 && !last_valid[slot]) slot = -1;
    if (slot >= 0) {
        copy_state(last_state[slot], out);
        size_t length = source_raw_length[slot];
        if (length > raw_capacity) length = raw_capacity;
        if (!raw) length = 0;
        if (length) memcpy(raw, source_raw[slot], length);
        if (raw_length) *raw_length = length;
    }
    source_snapshot_unlock();
    return slot >= 0;
}

int gc_service_inject_source_acquire(const GcServiceSource *expected)
{
    if (!source_dispatch_lock()) return -1;
    source_snapshot_lock();
    int slot = checked_source_slot(expected);
    source_snapshot_unlock();
    if (slot < 0) source_dispatch_unlock();
    return slot >= 0 ? 0 : -1;
}

void gc_service_inject_source_release(void)
{
    source_dispatch_unlock();
}

int gc_service_inject_instance_consumer_prepare(GcConfigStore *store)
{
    if (!store || !gc_service_features_instance_revocation_epoch() || !source_dispatch_lock()) return -1;
    int result = -1;
    if (!prepared_store && !sources_active && !atomic_load_explicit(&source_fault, memory_order_acquire)) {
        if (pthread_once(&session_once, initialize_session) == 0 && session_ready) {
            prepared_store = store; result = 0;
        }
    }
    source_dispatch_unlock();
    return result;
}
void gc_service_inject_instance_policy_invalidated(void *context,
    GcInstanceInvalidationReason reason, uint64_t generation)
{
    if (!context || context != prepared_store || !generation ||
        (reason != GC_INSTANCE_INVALIDATE_EXTERNAL_RELOAD_FAILURE &&
         reason != GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK)) {
        atomic_store_explicit(&source_fault, 1, memory_order_release); return;
    }
    int acquired = source_depth == 0;
    if (acquired && !source_dispatch_lock()) {
        atomic_store_explicit(&source_fault, 1, memory_order_release);
        gc_service_features_instance_policy_invalidated(reason, generation); return;
    }
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot) {
        retire_samples(slot); policy_revoked[slot] = 1;
    }
    for (int i = 0; i < USB_SLOTS; ++i) {
        memset(&usb_feed[i].decoded,0,sizeof usb_feed[i].decoded);
        usb_feed[i].decoded.leftStick.x = usb_feed[i].decoded.leftStick.y = 128;
        usb_feed[i].decoded.rightStick.x = usb_feed[i].decoded.rightStick.y = 128;
    }
    gc_service_features_instance_policy_invalidated(reason, generation);
    uint64_t epoch = gc_service_features_instance_revocation_epoch();
    if (!epoch || epoch == UINT64_MAX) atomic_store_explicit(&source_fault,1,memory_order_release);
    if (acquired) source_dispatch_unlock();
}
int gc_service_inject_instance_acquire(const GcControllerMatch *expected)
{
    if (!expected || !session_ready || !prepared_store ||
        atomic_load_explicit(&source_fault, memory_order_acquire) ||
        expected->identity.state != GC_INSTANCE_STABLE ||
        expected->identity.proof != GC_INSTANCE_PROOF_CLASSIC_PEER ||
        expected->transport != GC_INSTANCE_TRANSPORT_CLASSIC || !source_dispatch_lock()) return -1;
    int found = 0;
    for (int slot = 0; slot < HOST_MAX_PADS; ++slot)
        if (source_info[slot].connected && !want_remove[slot] && binding_observed[slot] &&
            match_equal(expected, &source_info[slot].binding)) found = 1;
    if (!found) source_dispatch_unlock();
    return found ? 0 : -1;
}
void gc_service_inject_instance_release(void) { source_dispatch_unlock(); }

int gc_service_inject_process_session(char *out, size_t capacity)
{
    if (!out || capacity < sizeof process_session ||
        pthread_once(&session_once, initialize_session) != 0 || !session_ready) return -1;
    memcpy(out, process_session, sizeof process_session);
    return 0;
}

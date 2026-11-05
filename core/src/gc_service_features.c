/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wires the toolbox screens into GhosttoothAPI.
 * Setup reads the selected identified USB/BT source. Bluetooth stays off
 * until Pair, because
 * opening the host takes the console Bluetooth chip from the system stack.
 * Update commands are not handled here.
 */
#include "gc_service_features.h"
#include "gc_service_session_private.h"
#include "gc_bluetooth.h"
#include "gc_service_inject.h"
#include "gc_service_output.h"
#include "bt/log.h"
#include "gc_config.h"
#include "gc_controller_commands.h"
#include "gc_console_id.h"
#include "gc_ipc.h"
#include "gc_profile.h"
#include "gc_rawmap.h"
#include "gc_setup.h"
#include "gc_setup_preview.h"
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <limits.h>
#include <time.h>

#define PAD_PATH "app-pad-0"
/* Normalized scePad input does not contain physical VID/PID identity. */
#define PAD_VID 0u
#define PAD_PID 0u
#define PAD_PROTO "native"

static GcConfigStore *feat_config;
static GcSetup feat_setup;
static GcServiceSource feat_setup_source;
static int feat_setup_source_valid;
static GcBt *feat_bt;
static int consumer_registered;
/* Failed partial cleanup must never resume input or reset its native owners.
 * The app serializes tick/output/commands; callbacks may observe this fence. */
static _Atomic int feature_retiring;
static pthread_mutex_t feat_mu = PTHREAD_MUTEX_INITIALIZER;
static ScePadData feat_pad;
static int feat_pad_ok;
static int feat_setup_input_ok;
static uint64_t feat_pad_ms;
static uint64_t feat_motion_published;
static _Atomic uint64_t revocation_epoch;
static _Atomic uint64_t invalidation_generation;
static _Atomic int revocation_pending, revocation_fault;
static uint64_t draft_epoch;
static int draft_instance;
static char draft_controller_id[GC_PROFILE_ID_MAX], draft_profile_id[GC_PROFILE_ID_MAX];
static void discard_draft(void)
{
    memset(&feat_setup, 0, sizeof feat_setup); feat_setup.target = -1;
    memset(&feat_setup_source, 0, sizeof feat_setup_source);
    feat_setup_source_valid = feat_setup_input_ok = draft_instance = 0;
    draft_epoch = 0; draft_controller_id[0] = draft_profile_id[0] = 0;
}
void gc_service_features_instance_policy_invalidated(GcInstanceInvalidationReason reason, uint64_t generation)
{
    if (!generation || !atomic_load_explicit(&revocation_epoch, memory_order_acquire) ||
        (reason != GC_INSTANCE_INVALIDATE_EXTERNAL_RELOAD_FAILURE && reason != GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK)) {
        atomic_store_explicit(&revocation_fault, 1, memory_order_release); return;
    }
    uint64_t old = atomic_load_explicit(&invalidation_generation, memory_order_relaxed);
    if (old != generation) {
        uint64_t epoch = atomic_load_explicit(&revocation_epoch, memory_order_relaxed);
        if (epoch == UINT64_MAX) atomic_store_explicit(&revocation_fault, 1, memory_order_release);
        else atomic_store_explicit(&revocation_epoch, epoch+1, memory_order_release);
        atomic_store_explicit(&invalidation_generation, generation, memory_order_release);
    }
    atomic_store_explicit(&revocation_pending, 1, memory_order_release);
}
uint64_t gc_service_features_instance_revocation_epoch(void)
{
    return atomic_load_explicit(&revocation_epoch, memory_order_acquire);
}
int gc_service_features_instance_draft_check_held(uint64_t captured_epoch)
{
    uint64_t current = gc_service_features_instance_revocation_epoch();
    int pending = atomic_exchange_explicit(&revocation_pending, 0, memory_order_acq_rel);
    if (!current || !captured_epoch || !feat_setup.active || pending || captured_epoch != current ||
        atomic_load_explicit(&revocation_fault, memory_order_acquire)) {
        discard_draft(); return -1;
    }
    return 0;
}
/* Feature-owned consumers hold writer throughout check/use, never source->writer. */
static int draft_consumer_acquire(void)
{
    if (!feat_setup.active || !feat_config) return -1;
    int rc = draft_instance ? gc_config_instance_acquire(feat_config, GC_INSTANCE_REQUIRE_LIVE, NULL, 0) :
        gc_config_policy_acquire(feat_config, gc_config_generation(feat_config));
    if (rc) { discard_draft(); return -1; }
    if (gc_service_features_instance_draft_check_held(draft_epoch)) {
        gc_config_policy_release(feat_config); return -1;
    }
    return 0;
}
static void draft_consumer_release(void) { gc_config_policy_release(feat_config); }
static void instance_caps(struct json_object *o)
{
    GcInstanceCapabilities c = {0};
    (void)gc_config_instance_capabilities(feat_config, &c);
    json_object_object_add(o,"instance_contract_version",json_object_new_int(c.instance_contract_version));
    json_object_object_add(o,"instance_storage_available",json_object_new_boolean(c.instance_storage_available));
    json_object_object_add(o,"instance_live_available",json_object_new_boolean(c.instance_live_available));
    json_object_object_add(o,"instance_available",json_object_new_boolean(c.instance_available));
    json_object_object_add(o,"instance_policy_valid",json_object_new_boolean(c.instance_policy_valid));
}
static void decimal_field(struct json_object *o, const char *key, uint64_t value)
{
    char text[21]; snprintf(text,sizeof text,"%llu",(unsigned long long)value);
    json_object_object_add(o,key,json_object_new_string(text));
}
static void match_json(struct json_object *o, const GcControllerMatch *m)
{
    const char *transport = m->transport == GC_INSTANCE_TRANSPORT_CLASSIC ? "classic" :
        m->transport == GC_INSTANCE_TRANSPORT_LE ? "le" : m->transport == GC_INSTANCE_TRANSPORT_USB ? "usb" : "unknown";
    json_object_object_add(o,"transport",json_object_new_string(transport));
    json_object_object_add(o,"session_id",json_object_new_string(m->session_id));
    json_object_object_add(o,"path",json_object_new_string(m->path));
    json_object_object_add(o,"protocol",json_object_new_string(m->protocol));
    decimal_field(o,"connection_generation",m->connection_generation);
    decimal_field(o,"occurrence_generation",m->occurrence_generation);
    json_object_object_add(o,"vid",json_object_new_int(m->vid));
    json_object_object_add(o,"pid",json_object_new_int(m->pid));
    json_object_object_add(o,"interface_index",json_object_new_int(m->interface_index));
    json_object_object_add(o,"interface_number",json_object_new_int(m->interface_number));
    json_object_object_add(o,"alternate_setting",json_object_new_int(m->alternate_setting));
    json_object_object_add(o,"in_endpoint",json_object_new_int(m->in_endpoint));
    json_object_object_add(o,"out_endpoint",json_object_new_int(m->out_endpoint));
    json_object_object_add(o,"instance_identity_known",json_object_new_boolean(m->identity.state == GC_INSTANCE_STABLE));
    json_object_object_add(o,"identity_state",json_object_new_string(m->identity.state == GC_INSTANCE_STABLE ? "stable" : m->identity.state == GC_INSTANCE_COLLISION ? "collision" : "unknown"));
    json_object_object_add(o,"identity_proof",json_object_new_string(m->identity.proof == GC_INSTANCE_PROOF_CLASSIC_PEER ? "classic_peer" : "none"));
    json_object_object_add(o,"instance_id",json_object_new_string(m->identity.instance_id));
    json_object_object_add(o,"continuity_limit",json_object_new_string("observed_transitions_only"));
}

static int write_json(struct json_object *o, char *out, size_t cap)
{
    const char *text = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
    size_t n = text ? strlen(text) : SIZE_MAX;
    if (n > 32766 || !out || cap <= n+1) {
        int saved = 0; struct json_object *v;
        if (json_object_object_get_ex(o,"saved",&v)) saved = json_object_get_boolean(v);
        json_object_put(o); o = json_object_new_object(); instance_caps(o);
        json_object_object_add(o,"ok",json_object_new_boolean(0));
        json_object_object_add(o,"saved",json_object_new_boolean(saved));
        json_object_object_add(o,"verified",json_object_new_boolean(0));
        json_object_object_add(o,"applied",json_object_new_boolean(0));
        json_object_object_add(o,"error",json_object_new_string("reply_too_large"));
        text = json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN); n = strlen(text);
        if (out && cap > n+1) { memcpy(out,text,n); out[n]='\n'; out[n+1]=0; }
        else if (out && cap) out[0]=0;
        json_object_put(o); return -1;
    }
    memcpy(out,text,n); out[n]='\n'; out[n+1]=0;
    json_object_put(o); return 0;
}

static int fail(const char *code, const char *detail, char *out, size_t cap)
{
    struct json_object *o = json_object_new_object();
    instance_caps(o);
    json_object_object_add(o, "saved", json_object_new_boolean(0));
    json_object_object_add(o, "verified", json_object_new_boolean(0));
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "applied", json_object_new_boolean(0));
    json_object_object_add(o, "error", json_object_new_string(code));
    if (detail && *detail) {
        char hex[1025];
        size_t n = strlen(detail);
        if (n > 512) n = 512;
        if (!gc_ipc_hex_encode(detail, n, hex, sizeof(hex)))
            json_object_object_add(o, "detail", json_object_new_string(hex));
    }
    write_json(o, out, cap);
    return -1;
}

static int source_lookup(const char *path, uint16_t vid, uint16_t pid,
                         GcServiceSource *selected)
{
    GcServiceSource sources[GC_BT_MAX_PADS * 2];
    size_t count = gc_service_inject_sources(sources, GC_BT_MAX_PADS * 2);
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(sources[i].path, path) && sources[i].connected &&
            sources[i].vid == vid && sources[i].pid == pid) {
            *selected = sources[i];
            return 1;
        }
    }
    return 0;
}

static int source_sample(const char *path, uint16_t vid, uint16_t pid,
                         GcServiceSource *selected, ScePadData *data,
                         uint8_t *raw, size_t *raw_length)
{
    GcServiceSource sources[GC_BT_MAX_PADS * 2];
    size_t count = gc_service_inject_sources(sources, GC_BT_MAX_PADS * 2);
    int matched = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(sources[i].path, path) && sources[i].connected &&
            sources[i].input_available && sources[i].vid == vid && sources[i].pid == pid) {
            *selected = sources[i]; matched = 1; break;
        }
    }
    return matched && gc_service_inject_read_source_checked(selected, data, raw,
        GC_SERVICE_REPORT_MAX, raw_length) && data->connected;
}

/* Caller holds feat_mu. Never adopt a replacement connection implicitly. */
static void clear_setup_input(void)
{
    feat_setup_input_ok = 0;
    memset(&feat_setup.input, 0, sizeof feat_setup.input);
    memset(feat_setup.raw_report, 0, sizeof feat_setup.raw_report);
    feat_setup.raw_len = 0; feat_setup.raw_valid = 0;
}
static int setup_source_current(void)
{
    if (feat_setup.active && feat_setup_source_valid &&
        gc_service_inject_source_acquire(&feat_setup_source) == 0) {
        gc_service_inject_source_release(); return 1;
    }
    /* The inject feed owns the device while it is streaming, so the exclusive
     * acquire above can fail even though the very same connection is alive.
     * That made SETUP_ARM answer acknowledged=1 applied=0 forever. Accept a
     * source that is still listed and connected as current. */
    if (feat_setup.active && feat_setup_source_valid) {
        GcServiceSource list[GC_BT_MAX_PADS * 2];
        size_t n = gc_service_inject_sources(list, GC_BT_MAX_PADS * 2);
        for (size_t i = 0; i < n; ++i) {
            if (list[i].connected && !strcmp(list[i].path, feat_setup_source.path) &&
                list[i].connection_generation == feat_setup_source.connection_generation)
                return 1;
        }
    }
    if (feat_setup.active) {
        feat_setup_source_valid = 0;
        clear_setup_input();
        snprintf(feat_setup.message, sizeof feat_setup.message,
                 "Selected controller disconnected or changed. Begin setup again.");
    }
    return 0;
}

static int pad_fresh(uint64_t now)
{
    ScePadData data;
    int ok = gc_service_pad_read(&data);
    pthread_mutex_lock(&feat_mu);
    feat_pad_ok = ok;
    if (ok) feat_pad = data;
    else {
        memset(&feat_pad, 0, sizeof feat_pad);
        feat_motion_published = 0;
    }
    feat_pad_ms = now;
    int leased = feat_setup.active && draft_consumer_acquire() == 0;
    if (leased) {
        ScePadData input;
        uint8_t raw[GC_SERVICE_REPORT_MAX];
        size_t raw_length = 0;
        feat_setup_input_ok = setup_source_current() &&
            gc_service_inject_read_source_checked(&feat_setup_source, &input, raw, sizeof raw, &raw_length);
        if (feat_setup_input_ok) {
            /* Decoded input is the learning surface. A descriptor with no
             * standard usages decodes to an empty pad, so also offer the raw
             * report: the learner derives discrete bits from it (and uses it
             * for wake/release), which is what makes an unknown pad mappable
             * without shipping a new app. */
            gc_setup_feed_input(&feat_setup, feat_setup.path, &input, now);
            if (raw_length)
                gc_setup_feed_raw(&feat_setup, feat_setup.path, raw, raw_length,
                                  &input, now);
            /* Diagnostic: the raw report layout is device-specific and the
             * learner's byte window is a heuristic, so record what actually
             * arrives (once a second) while a draft is open. */
            if (raw_length && feat_setup.active) {
                static uint64_t diag_ms;
                if (now - diag_ms >= 1000) {
                    diag_ms = now;
                    {
                        const uint32_t noise =
                            ((feat_setup.raw_noise & (1u << 4)) ? 0x000000ffu : 0u) |
                            ((feat_setup.raw_noise & (1u << 5)) ? 0x0000ff00u : 0u) |
                            ((feat_setup.raw_noise & (1u << 6)) ? 0x00ff0000u : 0u) |
                            ((feat_setup.raw_noise & (1u << 7)) ? 0xff000000u : 0u);
                        const unsigned span = feat_setup.raw_span_offset;
                        const uint32_t win = gc_rawmap_window(raw, raw_length, span);
                        const uint32_t base = feat_setup.raw_len
                            ? gc_rawmap_window(feat_setup.wake_baseline,
                                               feat_setup.wake_baseline_len, span) : 0;
                        log_line("setup raw len=%zu %02x%02x%02x%02x%02x%02x%02x%02x"
                                 " win=%08x base=%08x noise=%08x bits=%08x"
                                 " span=%u/%08x target=%d bits2=%08x",
                                 raw_length, raw[0], raw[1], raw[2], raw[3], raw[4],
                                 raw[5], raw[6], raw[7], win, base, noise,
                                 (win ^ base) & ~noise,
                                 feat_setup.raw_span_offset, feat_setup.raw_span_mask,
                                 feat_setup.target,
                                 (win ^ base) & feat_setup.raw_span_mask);
                    }
                }
            }
            size_t copy = raw_length < sizeof feat_setup.raw_report ? raw_length : sizeof feat_setup.raw_report;
            memcpy(feat_setup.raw_report, raw, copy);
            feat_setup.raw_len = (uint16_t)copy;
            feat_setup.raw_valid = copy != 0;
        } else {
            /* A publication retires samples, not an unchanged physical draft. */
            clear_setup_input();
        }
        draft_consumer_release();
    } else feat_setup_input_ok = 0;
    pthread_mutex_unlock(&feat_mu);
    return ok;
}

void gc_service_features_init(struct GcConfigStore *config)
{
    if (atomic_load_explicit(&feature_retiring,memory_order_acquire)) return;
    if (consumer_registered) return; /* no revocation reset while registered */
    const char *root = config ? gc_config_root(config) : "/data/ghostcontrol";
    char db[GC_CONFIG_PATH_MAX];
    feat_config = config;
    /* Constructor quiescence: no installed hook or prior source/feature calls. */
    atomic_store_explicit(&invalidation_generation,0,memory_order_relaxed);
    atomic_store_explicit(&revocation_pending,0,memory_order_relaxed);
    atomic_store_explicit(&revocation_fault,0,memory_order_relaxed);
    atomic_store_explicit(&revocation_epoch,1,memory_order_release);
    pthread_mutex_lock(&feat_mu);
    memset(&feat_setup, 0, sizeof(feat_setup));
    memset(&feat_setup_source, 0, sizeof feat_setup_source);
    feat_setup_source_valid = 0;
    memset(&feat_pad, 0, sizeof(feat_pad));
    feat_pad_ok = feat_setup_input_ok = 0;
    feat_pad_ms = feat_motion_published = 0;
    pthread_mutex_unlock(&feat_mu);
    snprintf(db, sizeof(db), "%s/bt/controllers.db", root ? root : "/data/ghostcontrol");
    /* Receiver lifetime is installed before any resident source exposure.
     * Product authority remains DISABLED until independently accepted startup. */
    if (config && (gc_service_inject_instance_consumer_prepare(config) ||
        gc_config_instance_invalidation_register(config,
            gc_service_inject_instance_policy_invalidated,config,NULL,0))) {
        fprintf(stderr,"features: instance consumer preparation refused; sources disabled\n");
        return;
    }
    consumer_registered = config != NULL;
    gc_service_inject_init();
    gc_service_inject_set_config(config);
    feat_bt = gc_bt_start_disabled(gc_service_inject_ops(), db);
    gc_service_inject_set_bt(feat_bt);
    if (!feat_bt)
        fprintf(stderr, "features: bluetooth engine did not start\n");
}

const char *gc_service_features_release_blocker(void)
{
    const char *why=gc_service_output_release_blocker();
    if(!why) why=gc_service_inject_release_blocker();
    if(!why) why=gc_bt_release_blocker(feat_bt);
    /* Global logging may have been acquired before a failed BT constructor.
     * NULL feat_bt is not proof that this module acquired no logger resource. */
    if(!why) why=log_release_blocker();
    return why;
}
int gc_service_features_retiring(void)
{ return atomic_load_explicit(&feature_retiring,memory_order_acquire); }
int gc_service_features_close_checked(void)
{
    atomic_store_explicit(&feature_retiring,1,memory_order_release);
    /* Stop the callback producer before its inject/config dependencies. The
     * checked BT owner remains reachable when join or host cleanup fails. */
    if(gc_bt_stop_checked(feat_bt)) return -1;
    feat_bt=NULL;
    gc_service_inject_set_bt(NULL);
    if(gc_service_inject_close_checked()) return -1;
    if(gc_service_output_close_checked()) return -1;
    if(log_close_checked()) return -1;
    if(pthread_mutex_lock(&feat_mu)) return -1;
    discard_draft();
    memset(&feat_pad,0,sizeof feat_pad);
    feat_pad_ok=0; feat_pad_ms=feat_motion_published=0;
    if(pthread_mutex_unlock(&feat_mu)) return -1;
    if(consumer_registered) {
        if(gc_config_instance_invalidation_unregister(feat_config,
                gc_service_inject_instance_policy_invalidated,feat_config,NULL,0)) return -1;
        consumer_registered=0;
    }
    gc_service_inject_set_config(NULL);
    feat_config=NULL;
    atomic_store_explicit(&revocation_epoch,0,memory_order_release);
    atomic_store_explicit(&feature_retiring,0,memory_order_release);
    return 0;
}

void gc_service_features_close(void)
{
    if (feat_bt) {
        gc_bt_stop(feat_bt);
        feat_bt = NULL;
    }
    pthread_mutex_lock(&feat_mu);
    discard_draft();
    memset(&feat_pad, 0, sizeof feat_pad);
    feat_pad_ok = 0;
    feat_pad_ms = feat_motion_published = 0;
    pthread_mutex_unlock(&feat_mu);
    if (consumer_registered) {
        if (gc_config_instance_invalidation_unregister(feat_config,
                gc_service_inject_instance_policy_invalidated,feat_config,NULL,0)) abort();
        consumer_registered = 0;
    }
    gc_service_inject_close();
    gc_service_inject_set_config(NULL);
    feat_config = NULL;
    atomic_store_explicit(&revocation_epoch,0,memory_order_release);
}

void gc_service_features_tick(uint64_t now_ms)
{
    if(gc_service_features_retiring()) return;
    gc_service_inject_tick((long)now_ms);
    pad_fresh(now_ms);
}

void gc_service_features_slot(struct json_object *slots)
{
    struct json_object *slot = json_object_new_object();
    char vid_pid[16];
    int connected;
    pthread_mutex_lock(&feat_mu);
    connected = feat_pad_ok && feat_pad.connected;
    pthread_mutex_unlock(&feat_mu);
    vid_pid[0] = 0;
    json_object_object_add(slot, "slot", json_object_new_int(0));
    json_object_object_add(slot, "connected", json_object_new_boolean(connected));
    json_object_object_add(slot, "streaming", json_object_new_boolean(0));
    json_object_object_add(slot, "device_path", json_object_new_string(PAD_PATH));
    json_object_object_add(slot, "vid", json_object_new_int(PAD_VID));
    json_object_object_add(slot, "pid", json_object_new_int(PAD_PID));
    json_object_object_add(slot, "vid_pid", json_object_new_string(vid_pid));
    json_object_object_add(slot, "name", json_object_new_string("Initial-user pad (model unidentified)"));
    json_object_object_add(slot, "identity_known", json_object_new_boolean(0));
    json_object_object_add(slot, "protocol", json_object_new_string(PAD_PROTO));
    json_object_object_add(slot, "bound_user", json_object_new_int(-1));
    json_object_object_add(slot, "bound_user_known", json_object_new_boolean(0));
    json_object_array_add(slots, slot);
    for (int i = 0; i < GC_BT_MAX_PADS; ++i) {
        struct json_object *virt = json_object_new_object();
        char path[16], vid_text[16];
        uint16_t vid = 0, pid = 0;
        int live = gc_service_inject_live(i);
        gc_service_inject_identity(i, &vid, &pid);
        snprintf(path, sizeof path, "vpad-%d", i);
        snprintf(vid_text, sizeof vid_text, "%04X:%04X", vid, pid);
        json_object_object_add(virt, "slot", json_object_new_int(i + 1));
        json_object_object_add(virt, "connected", json_object_new_boolean(live));
        json_object_object_add(virt, "streaming", json_object_new_boolean(live));
        json_object_object_add(virt, "device_path", json_object_new_string(path));
        json_object_object_add(virt, "vid", json_object_new_int(vid));
        json_object_object_add(virt, "pid", json_object_new_int(pid));
        json_object_object_add(virt, "vid_pid", json_object_new_string(live ? vid_text : ""));
        json_object_object_add(virt, "name", json_object_new_string(live ? "Virtual DualSense" : ""));
        json_object_object_add(virt, "protocol", json_object_new_string("vpad"));
        json_object_object_add(virt, "bound_user", json_object_new_int(-1));
        json_object_object_add(virt, "bound_user_known", json_object_new_boolean(0));
        json_object_array_add(slots, virt);
    }
}

int gc_service_features_motion(float gyro[3], float accel[3])
{
    struct timespec ts;
    uint64_t now = 0;
    int live;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    pthread_mutex_lock(&feat_mu);
    /* Connection is not a motion producer. Timestamp 0, a stopped reader,
     * or a repeated device timestamp is unavailable — not a held pose. */
    live = feat_pad_ok && feat_pad.connected && feat_pad.timestamp &&
        now >= feat_pad_ms && now - feat_pad_ms < 250 &&
        feat_pad.timestamp != feat_motion_published;
    if (live) feat_motion_published = feat_pad.timestamp;
    if (live && gyro && accel) {
        gyro[0] = feat_pad.vel.x;
        gyro[1] = feat_pad.vel.y;
        gyro[2] = feat_pad.vel.z;
        accel[0] = feat_pad.accel.x;
        accel[1] = feat_pad.accel.y;
        accel[2] = feat_pad.accel.z;
    } else if (gyro && accel) {
        memset(gyro, 0, 3 * sizeof *gyro);
        memset(accel, 0, 3 * sizeof *accel);
    }
    pthread_mutex_unlock(&feat_mu);
    return live;
}

static int console_id(char *out, size_t cap)
{
    GcConsoleId id;
    struct json_object *o = json_object_new_object();
    int available = gc_console_id_read(&id) == 0;
    char openpsid[33] = "", system[33] = "";
    json_object_object_add(o, "ok", json_object_new_boolean(available));
    json_object_object_add(o, "serial", json_object_new_string(
        available && (id.flags & GC_CONSOLE_ID_HAS_SERIAL) ? id.serial : "Unavailable"));
    json_object_object_add(o, "model", json_object_new_string(
        available && (id.flags & GC_CONSOLE_ID_HAS_MODEL) ? id.model : "Unavailable"));
    if (available && (id.flags & GC_CONSOLE_ID_HAS_OPENPSID))
        gc_console_id_hex(id.openpsid, openpsid);
    if (available && (id.flags & GC_CONSOLE_ID_HAS_OPENPSID_SYS))
        gc_console_id_hex(id.openpsid_sys, system);
    json_object_object_add(o, "openpsid", json_object_new_string(openpsid[0] ? openpsid : "Unavailable"));
    json_object_object_add(o, "openpsid_system", json_object_new_string(system[0] ? system : "Unavailable"));
    return write_json(o, out, cap);
}

static int devices(char *out, size_t cap)
{
    struct json_object *o = json_object_new_object();
    struct json_object *list = json_object_new_array();
    int connected;
    pthread_mutex_lock(&feat_mu);
    connected = feat_pad_ok && feat_pad.connected;
    pthread_mutex_unlock(&feat_mu);
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "devices", list);
    instance_caps(o);
    char session[GC_INSTANCE_SESSION_MAX] = "";
    if (gc_service_inject_process_session(session, sizeof session) != 0) {
        json_object_object_add(o, "ok", json_object_new_boolean(0));
        json_object_object_add(o, "error", json_object_new_string("instance_capability_unavailable"));
        return write_json(o, out, cap);
    }
    json_object_object_add(o,"session_id",json_object_new_string(session));
    struct json_object *native = json_object_new_array();
    json_object_object_add(o,"native_observations",native);
    if (connected) {
        struct json_object *item = json_object_new_object();
        json_object_object_add(item, "path", json_object_new_string(PAD_PATH));
        json_object_object_add(item, "vid", json_object_new_int(PAD_VID));
        json_object_object_add(item, "pid", json_object_new_int(PAD_PID));
        json_object_object_add(item, "protocol", json_object_new_string(PAD_PROTO));
        json_object_object_add(item, "name", json_object_new_string("Initial-user pad (model unidentified)"));
        json_object_object_add(item, "identity_known", json_object_new_boolean(0));
        json_object_object_add(item, "connected", json_object_new_boolean(1));
        json_object_object_add(item, "input_available", json_object_new_boolean(1));
        json_object_object_add(item, "configured", json_object_new_boolean(0));
        json_object_object_add(item, "supported", json_object_new_boolean(0));
        json_object_object_add(item, "setup_required", json_object_new_boolean(0));
        json_object_object_add(item, "setup_error", json_object_new_string("physical_identity_unavailable"));
        json_object_object_add(item, "ignored", json_object_new_boolean(0));
        json_object_object_add(item, "streaming", json_object_new_boolean(0));
        json_object_object_add(item, "transport", json_object_new_string("app-pad"));
        /* Normalized app-pad has no physical capture. Contract section 2
         * forbids zero/fabricated occurrence tokens in physical inventory. */
        json_object_object_add(item,"observation_kind",json_object_new_string("normalized_native_pad"));
        json_object_object_add(item,"instance_identity_known",json_object_new_boolean(0));
        json_object_object_add(item,"identity_state",json_object_new_string("unknown"));
        json_object_object_add(item,"identity_proof",json_object_new_string("none"));
        json_object_object_add(item,"instance_id",json_object_new_string(""));
        json_object_object_add(item,"physical_binding_available",json_object_new_boolean(0));
        json_object_object_add(item,"instance_selectable",json_object_new_boolean(0));
        json_object_object_add(item,"block_reason",json_object_new_string("physical_identity_unavailable"));
        json_object_array_add(native, item);
    }
    /* Physical source inventory is independent of virtual-device readiness.
     * A configured preference must not be described as live game output. */
    {
        GcServiceSource sources[GC_BT_MAX_PADS * 2];
        size_t count = gc_service_inject_sources(sources, GC_BT_MAX_PADS * 2);
        for (size_t i = 0; i < count; ++i) {
            const GcServiceSource *source = &sources[i];
            GcControllerResolution resolution = {0};
            int resolved = feat_config && gc_config_resolve_controller(feat_config,&source->binding,&resolution) == 0;
            int found = resolved && resolution.has_rule;
            int mapped = resolved && resolution.has_profile;
            int blocked = !resolved || (resolution.kind != GC_RESOLVE_NONE && resolution.kind != GC_RESOLVE_MODEL && resolution.kind != GC_RESOLVE_INSTANCE);
            struct json_object *item = json_object_new_object();
            const char *type = gc_controller_output_name(source->output_type);
            json_object_object_add(item, "path", json_object_new_string(source->path));
            json_object_object_add(item, "vid", json_object_new_int(source->vid));
            json_object_object_add(item, "pid", json_object_new_int(source->pid));
            json_object_object_add(item, "connection_generation", json_object_new_int((int)source->connection_generation));
            json_object_object_add(item, "identity_known", json_object_new_boolean(source->vid != 0));
            json_object_object_add(item, "protocol", json_object_new_string(source->protocol));
            /* Exact persisted identity, not a display label or invented model. */
            json_object_object_add(item, "setup_profile_id", json_object_new_string(mapped ? resolution.profile.id : ""));
            json_object_object_add(item, "name", json_object_new_string(source->name));
            json_object_object_add(item, "transport", json_object_new_string(source->transport));
            json_object_object_add(item, "connected", json_object_new_boolean(source->connected));
            json_object_object_add(item, "input_available", json_object_new_boolean(source->input_available));
            json_object_object_add(item, "configured", json_object_new_boolean(!blocked && (found || mapped)));
            json_object_object_add(item, "supported", json_object_new_boolean(source->input_available));
            json_object_object_add(item, "setup_required", json_object_new_boolean(!blocked && !found && !mapped));
            json_object_object_add(item, "ignored", json_object_new_boolean(blocked));
            json_object_object_add(item, "streaming", json_object_new_boolean(source->virtual_live));
            json_object_object_add(item, "virtual_device_live", json_object_new_boolean(source->virtual_live));
            json_object_object_add(item, "output_type", json_object_new_string(type ? type : ""));
            json_object_object_add(item, "output_applied", json_object_new_boolean(0));
            json_object_object_add(item, "output_error", json_object_new_string(source->output_error));
            match_json(item,&source->binding);
            json_object_object_add(item,"matched_controller_id",json_object_new_string(found ? resolution.rule.id : ""));
            json_object_object_add(item,"profile_id",json_object_new_string(mapped ? resolution.profile.id : ""));
            json_object_object_add(item,"match_scope",json_object_new_string(blocked ? "blocked" : resolution.kind == GC_RESOLVE_INSTANCE ? "instance" : resolution.kind == GC_RESOLVE_MODEL ? "model" : "none"));
            json_object_object_add(item,"block_reason",json_object_new_string(blocked ? source->output_error : ""));
            json_object_array_add(list, item);
        }
    }
    return write_json(o, out, cap);
}

static int parse_hex_u16(const char *value, uint16_t *output)
{
    char *end = NULL;
    unsigned long parsed;
    if (!value || !*value) return -1;
    parsed = strtoul(value, &end, 16);
    if (!end || *end || parsed > 0xfffful) return -1;
    *output = (uint16_t)parsed;
    return 0;
}

static int setup_status(char *out, size_t cap, int with_input)
{
    struct json_object *o = json_object_new_object();
    pthread_mutex_lock(&feat_mu);
    int leased = feat_setup.active && draft_consumer_acquire() == 0;
    if (leased && !setup_source_current()) discard_draft();
    int active = feat_setup.active, target = feat_setup.target;
    instance_caps(o);
    if (active && draft_instance) {
        match_json(o,&feat_setup_source.binding);
        json_object_object_add(o,"scope",json_object_new_string("instance"));
        json_object_object_add(o,"controller_id",json_object_new_string(draft_controller_id));
        json_object_object_add(o,"profile_id",json_object_new_string(draft_profile_id));
    }
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "active", json_object_new_boolean(active));
    json_object_object_add(o, "source_valid", json_object_new_boolean(active && feat_setup_source_valid));
    json_object_object_add(o, "source_connection_generation", json_object_new_uint64(
        active ? feat_setup_source.connection_generation : 0));
    json_object_object_add(o, "message", json_object_new_string(feat_setup.message));
    if (gc_setup_preview_add(o,&feat_setup,leased && active && feat_setup_source_valid,
            draft_instance ? draft_profile_id : feat_setup.profile.id)) {
        if (leased) draft_consumer_release();
        pthread_mutex_unlock(&feat_mu);
        json_object_put(o);
        return fail("setup_preview_unavailable",NULL,out,cap);
    }
    json_object_object_add(o, "target", json_object_new_string(
        target >= 0 ? gc_setup_target_name(target) : ""));
    if (with_input) {
        int available = active ? (feat_setup_source_valid && feat_setup_input_ok) : (feat_pad_ok && feat_pad.connected);
        const ScePadData *input = active ? &feat_setup.input : &feat_pad;
        json_object_object_add(o, "connected", json_object_new_boolean(available));
        json_object_object_add(o, "buttons", json_object_new_int64(available ? (int64_t)input->buttons : 0));
        json_object_object_add(o, "path", json_object_new_string(active ? feat_setup.path : PAD_PATH));
        if (active) {
            char raw_hex[sizeof feat_setup.raw_report * 2 + 1];
            size_t raw_size = feat_setup.raw_valid ? feat_setup.raw_len : 0;
            if (!gc_ipc_hex_encode(feat_setup.raw_report, raw_size, raw_hex, sizeof raw_hex))
                json_object_object_add(o, "raw_report_hex", json_object_new_string(raw_hex));
            json_object_object_add(o, "raw_cached_bytes", json_object_new_int((int)raw_size));
            json_object_object_add(o, "raw_cache_limit", json_object_new_int((int)sizeof feat_setup.raw_report));
        }
    }
    if (leased) draft_consumer_release();
    pthread_mutex_unlock(&feat_mu);
    return write_json(o, out, cap);
}

static int valid_id(const char *id)
{
    size_t n;
    if (!id || !*id || !strcmp(id, ".") || !strcmp(id, "..") || !strcasecmp(id, "auto"))
        return 0;
    n = strlen(id);
    if (n >= GC_PROFILE_ID_MAX) return 0;
    return strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == n;
}

static int parse_connection_generation(const char *text, uint64_t *value)
{
    uint64_t parsed = 0;
    if (!text || !*text || *text == '0' || strlen(text) > 20) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        unsigned digit = *p - '0';
        if (parsed > (UINT64_MAX - digit) / 10) return -1;
        parsed = parsed * 10 + digit;
    }
    if (!parsed) return -1;
    *value = parsed;
    return 0;
}

static int setup_begin(const char *arg, char *out, size_t cap)
{
    char text[256];
    size_t length = 0;
    char *parts[6], *cursor;
    uint64_t requested_connection = 0;
    int part_count = 0;
    uint16_t vid = 0, pid = 0;
    GcProfile profile;
    GcServiceSource selected;
    ScePadData input;
    uint8_t raw[GC_SERVICE_REPORT_MAX];
    size_t raw_length = 0;
    if (gc_ipc_hex_decode(arg, text, sizeof(text) - 1, &length) || !length || memchr(text, 0, length))
        return fail("invalid_setup_selection", NULL, out, cap);
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] == 127)
            return fail("invalid_setup_selection", NULL, out, cap);
    text[length] = 0;
    cursor = text;
    while (part_count < 6) {
        parts[part_count++] = cursor;
        cursor = strchr(cursor, '|');
        if (!cursor) break;
        *cursor++ = 0;
    }
    if (cursor || (part_count != 5 && part_count != 6) ||
        (part_count == 6 && parse_connection_generation(parts[5], &requested_connection)))
        return fail("invalid_setup_selection", NULL, out, cap);
    if (!*parts[0] || strlen(parts[0]) >= sizeof feat_setup.path ||
        parse_hex_u16(parts[1], &vid) || !vid || parse_hex_u16(parts[2], &pid) ||
        !valid_id(parts[3]) || strcmp(parts[4], "identity"))
        return fail("invalid_setup_selection", "Select an identified source from DEVICES", out, cap);
    int sampled = source_sample(parts[0], vid, pid, &selected, &input, raw, &raw_length);
    if (!sampled && !source_lookup(parts[0], vid, pid, &selected))
        return fail("setup_reader_not_ready", "The selected source is missing or has no decoded input", out, cap);
    if (!sampled) {
        memset(&input, 0, sizeof input);
        memset(raw, 0, sizeof raw);
        raw_length = 0;
        input.connected = selected.connected;
    }
    if (requested_connection && requested_connection != selected.connection_generation)
        return fail("setup_source_changed", "The selected picker connection changed. Select the source again.", out, cap);
    int editing = feat_config && !gc_config_find_exact_profile(feat_config, vid, pid, selected.protocol, &profile, NULL);
    if (!editing) {
        if (!sampled)
            return fail("setup_reader_not_ready", "The selected source is missing or has no decoded input", out, cap);
        gc_profile_defaults(&profile);
        snprintf(profile.id, sizeof(profile.id), "%s", parts[3]);
        snprintf(profile.name, sizeof(profile.name), "%s", parts[3]);
        snprintf(profile.protocol, sizeof(profile.protocol), "%s", selected.protocol);
        profile.match_any_vid = profile.match_any_pid = 0;
        profile.match_vid = vid; profile.match_pid = pid;
    }
    profile.enabled = 1;
    pthread_mutex_lock(&feat_mu);
    uint64_t generation = gc_config_generation(feat_config);
    if (gc_config_policy_acquire(feat_config,generation)) {
        pthread_mutex_unlock(&feat_mu); return fail("instance_policy_quarantined",NULL,out,cap);
    }
    GcControllerResolution admission = {0};
    int resolved = gc_config_resolve_controller(feat_config,&selected.binding,&admission);
    int kind_ok = admission.kind == GC_RESOLVE_NONE || admission.kind == GC_RESOLVE_MODEL ||
        admission.kind == GC_RESOLVE_INSTANCE;
    int refuses = resolved || !kind_ok;
    int live = gc_service_inject_read_source_checked(&selected, &input, raw, sizeof raw, &raw_length);
    if (refuses || (!live && !editing)) {
        gc_config_policy_release(feat_config);
        pthread_mutex_unlock(&feat_mu);
        return fail("setup_source_changed", "The selected connection changed before setup began", out, cap);
    }
    if (feat_setup_source_valid &&
        feat_setup_source.connection_generation == selected.connection_generation)
        gc_setup_rebegin(&feat_setup, &profile, parts[0]);
    else gc_setup_begin(&feat_setup, &profile, parts[0]);
    feat_setup_source = selected; feat_setup_source_valid = 1;
    {
        unsigned span_offset = 0;
        uint32_t span_mask = 0;
        if (gc_service_inject_source_digital_span(selected.slot, selected.path, &span_offset, &span_mask) == 0)
            gc_setup_set_raw_span(&feat_setup, span_offset, span_mask);
        else
            gc_setup_set_raw_span(&feat_setup, 0, 0);
    }
    draft_instance = 0; draft_epoch = gc_service_features_instance_revocation_epoch();
    gc_setup_seed_buttons(&feat_setup, input.buttons);
    feat_setup.input = input; feat_setup.input_ms = feat_pad_ms;
    feat_setup_input_ok = live ? 1 : 0;
    snprintf(feat_setup.message, sizeof(feat_setup.message),
             editing ? "Editing %s. Arm the walk, then press each control."
                     : "Setup open for %s. Arm the walk, then press each control.",
             profile.id);
    gc_config_policy_release(feat_config);
    pthread_mutex_unlock(&feat_mu);
    return setup_status(out, cap, 0);
}

static int digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int clean(const char *s)
{
    for (; *s; ++s) if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f) return 0;
    return 1;
}
static struct json_object *strict_json(const char *text, size_t n)
{
    struct json_tokener *t = json_tokener_new();
    if (!t) return NULL;
    json_tokener_set_flags(t,JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *o = json_tokener_parse_ex(t,text,(int)n+1);
    if (json_tokener_get_error(t)!=json_tokener_success ||
        json_tokener_get_parse_end(t)!=n) { json_object_put(o); o=NULL; }
    json_tokener_free(t); return o;
}
/* json-c's UTF-8 flag is not a shortest-form/codepoint validator on every
 * supported host. Validate scalar values explicitly, before any mutation. */
static int utf8_scalars(const unsigned char *p)
{
    while (*p) {
        unsigned c=*p++, value, need, minimum;
        if (c<128) continue;
        if (c>=0xc2 && c<=0xdf) {value=c&31; need=1; minimum=128;}
        else if (c>=0xe0 && c<=0xef) {value=c&15; need=2; minimum=2048;}
        else if (c>=0xf0 && c<=0xf4) {value=c&7; need=3; minimum=65536;}
        else return 0;
        for (unsigned i=0;i<need;i++) {if ((*p&0xc0)!=0x80) return 0; value=(value<<6)|(*p++&63);}
        if (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return 0;
    }
    return 1;
}
static int unicode_escape(const char *p,unsigned *value)
{
    *value=0;
    for (int i=0;i<4;i++) {if (!p[i]) return 0; int d=digit((unsigned char)p[i]); if (d<0) return 0; *value=(*value<<4)|(unsigned)d;}
    return 1;
}
static const char *string_end(const char *p)
{
    if (*p++!='"') return NULL;
    while (*p) {
        if (*p=='"') return p+1;
        if (*p=='\\') {
            if (!p[1]) return NULL;
            if (p[1]=='u') {
                unsigned value;
                if (!unicode_escape(p+2,&value)) return NULL;
                p+=6;
                if (value>=0xd800 && value<=0xdbff) {
                    if (p[0]!='\\' || p[1]!='u' || !unicode_escape(p+2,&value) || value<0xdc00 || value>0xdfff) return NULL;
                    p+=6;
                } else if (value>=0xdc00 && value<=0xdfff) return NULL;
                continue;
            }
            p++;
        }
        p++;
    }
    return NULL;
}
static int unique_flat_keys(const char *text)
{
    char keys[14][64]; size_t count=0;
    const char *p=text; while (*p==' ') p++;
    if (*p++!='{') return 0;
    for (;;) {
        while (*p==' ') p++;
        if (*p=='}') return count==14;
        const char *end=string_end(p);
        if (!end || count==14 || (size_t)(end-p)>=GC_INSTANCE_JSON_MAX) return 0;
        char token[GC_INSTANCE_JSON_MAX]; memcpy(token,p,(size_t)(end-p)); token[end-p]=0;
        struct json_object *key=strict_json(token,strlen(token));
        if (!key || json_object_get_type(key)!=json_type_string) {json_object_put(key); return 0;}
        const char *k=json_object_get_string(key); size_t len=(size_t)json_object_get_string_len(key);
        if (len>=sizeof(keys[0]) || strlen(k)!=len) {json_object_put(key); return 0;}
        for (size_t i=0;i<count;i++) if (!strcmp(keys[i],k)) {json_object_put(key); return 0;}
        memcpy(keys[count++],k,len+1); json_object_put(key);
        p=end; while (*p==' ') p++; if (*p++!=':') return 0;
        while (*p==' ') p++;
        if (*p=='"') {p=string_end(p); if (!p) return 0;}
        else {while (*p && *p!=',' && *p!='}') {if (*p=='{' || *p=='[' || *p=='"') return 0; p++;}}
        while (*p==' ') p++;
        if (*p=='}') return count==14;
        if (*p++!=',') return 0;
    }
}
static int field_string(struct json_object *o,const char *key,char *out,size_t cap,int empty)
{
    struct json_object *v;
    if (!json_object_object_get_ex(o,key,&v) || json_object_get_type(v)!=json_type_string) return -1;
    const char *p=json_object_get_string(v); size_t n=(size_t)json_object_get_string_len(v);
    if ((!empty && !n) || n>=cap || strlen(p)!=n || !clean(p)) return -1;
    memcpy(out,p,n+1); return 0;
}
static int field_int(struct json_object *o,const char *key,int min,int max,int *out)
{
    struct json_object *v;
    if (!json_object_object_get_ex(o,key,&v) || json_object_get_type(v)!=json_type_int) return -1;
    int64_t n=json_object_get_int64(v);
    if (n<min || n>max) return -1;
    *out=(int)n; return 0;
}
static int setup_begin_instance(const char *arg,char *out,size_t cap)
{
    char text[GC_INSTANCE_JSON_MAX]; size_t n=strnlen(arg,GC_INSTANCE_JSON_MAX*2), decoded=0;
    const char *error="invalid_instance_request";
    if (!n || (n&1) || n>2*(GC_INSTANCE_JSON_MAX-1) || gc_ipc_hex_decode(arg,text,sizeof text-1,&decoded) ||
        memchr(text,0,decoded)) return fail(error,NULL,out,cap);
    text[decoded]=0;
    for (size_t i=0;i<decoded;i++) if ((unsigned char)text[i]<32 || (unsigned char)text[i]==127) return fail(error,NULL,out,cap);
    if (!utf8_scalars((const unsigned char *)text) || !unique_flat_keys(text)) return fail(error,NULL,out,cap);
    struct json_object *o=strict_json(text,decoded),*v;
    if (!o || json_object_get_type(o)!=json_type_object) {json_object_put(o); return fail(error,NULL,out,cap);}
    GcControllerMatch request={0}; char connection[21], controller_id[GC_PROFILE_ID_MAX], profile_id[GC_PROFILE_ID_MAX];
    int vid,pid,in,out_ep;
    if (!json_object_object_get_ex(o,"v",&v) || json_object_get_type(v)!=json_type_int) goto invalid;
    if (json_object_get_int64(v)!=1) {error="unsupported_instance_contract"; goto invalid;}
    if (json_object_object_length(o)!=14 ||
        field_string(o,"session_id",request.session_id,sizeof request.session_id,0) || strlen(request.session_id)!=32 ||
        strspn(request.session_id,"0123456789abcdef")!=32 ||
        field_string(o,"connection_generation",connection,sizeof connection,0) || parse_connection_generation(connection,&request.connection_generation) ||
        field_string(o,"path",request.path,sizeof request.path,0) ||
        field_string(o,"protocol",request.protocol,sizeof request.protocol,0) ||
        field_string(o,"controller_id",controller_id,sizeof controller_id,0) || !valid_id(controller_id) || !strcmp(controller_id,"default") ||
        field_string(o,"profile_id",profile_id,sizeof profile_id,0) || !valid_id(profile_id) || !strcmp(profile_id,"default") ||
        field_int(o,"vid",0,65535,&vid) || field_int(o,"pid",0,65535,&pid) ||
        field_int(o,"in_endpoint",0,255,&in) || field_int(o,"out_endpoint",0,255,&out_ep) ||
        field_int(o,"interface_index",-1,255,&request.interface_index) ||
        field_int(o,"interface_number",-1,255,&request.interface_number) ||
        field_int(o,"alternate_setting",-1,255,&request.alternate_setting)) goto invalid;
    request.vid=(uint16_t)vid; request.pid=(uint16_t)pid; request.in_endpoint=(uint8_t)in; request.out_endpoint=(uint8_t)out_ep;
    json_object_put(o);
    pthread_mutex_lock(&feat_mu);
    /* Cleanup is not authorization. A fresh Begin needs its own LIVE writer. */
    (void)gc_service_features_instance_draft_check_held(draft_epoch);
    char why[128]="";
    if (gc_config_instance_acquire(feat_config,GC_INSTANCE_REQUIRE_LIVE,why,sizeof why)) {
        pthread_mutex_unlock(&feat_mu); return fail(*why ? why : "instance_capability_unavailable",NULL,out,cap);
    }
    GcServiceSource sources[GC_BT_MAX_PADS*2],selected={0};
    size_t count=gc_service_inject_sources(sources,GC_BT_MAX_PADS*2); int matches=0;
    error="stale_instance_connection";
    for (size_t i=0;i<count;i++) {
        const GcControllerMatch *m=&sources[i].binding;
        if (strcmp(request.session_id,m->session_id)) { error="stale_instance_session"; continue; }
        if (request.connection_generation != m->connection_generation) continue;
        if (strcmp(request.path,m->path) || request.vid != m->vid || request.pid != m->pid ||
            strcmp(request.protocol,m->protocol) || request.interface_index != m->interface_index ||
            request.interface_number != m->interface_number || request.alternate_setting != m->alternate_setting ||
            request.in_endpoint != m->in_endpoint || request.out_endpoint != m->out_endpoint) {error="source_binding_mismatch"; break;}
        selected=sources[i]; ++matches;
    }
    int held=0;
    if (matches!=1) goto refused;
    if (selected.binding.identity.state != GC_INSTANCE_STABLE) {error=selected.binding.identity.state == GC_INSTANCE_COLLISION ? "identity_collision" : "identity_unavailable"; goto refused;}
    if (gc_service_inject_instance_acquire(&selected.binding)) {error="source_binding_mismatch"; goto refused;}
    held=1;
    GcControllerResolution resolution={0}; GcControllerRule rule;
    if (gc_config_resolve_controller(feat_config,&selected.binding,&resolution)) {error="source_binding_mismatch"; goto refused;}
    if (gc_config_find_controller_by_id(feat_config,controller_id,&rule,NULL)==0 &&
        (!rule.instance_id[0] || strcmp(rule.instance_id,selected.binding.identity.instance_id) ||
         rule.vid != selected.vid || rule.pid != selected.pid || (rule.profile_id[0] && strcmp(rule.profile_id,profile_id)))) {error="instance_retarget_forbidden"; goto refused;}
    GcProfile profile; gc_profile_defaults(&profile);
    if (resolution.kind == GC_RESOLVE_INSTANCE && resolution.has_profile) profile=resolution.profile;
    else if (resolution.has_rule && resolution.rule.instance_id[0] && resolution.rule.profile_id[0]) {
        GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES,sizeof *profiles);
        if (!profiles) {error="publication_failed"; goto refused;}
        size_t profile_count = gc_config_profiles(feat_config,profiles,GC_CONFIG_MAX_PROFILES,NULL);
        int found_profile = 0;
        for (size_t i=0;i<profile_count;i++) if (!strcmp(profiles[i].id,resolution.rule.profile_id)) {
            profile=profiles[i]; found_profile=1; break;
        }
        free(profiles);
        if (!found_profile) {error="profile_association_unavailable"; goto refused;}
    } else {
        /* New instance drafts copy only an unowned model mapping, never an
         * unrelated global selection or another pad's owned association. */
        (void)gc_config_find_exact_profile(feat_config,selected.vid,selected.pid,selected.protocol,&profile,NULL);
    }
    if (profile.owner_controller_id[0] && (strcmp(profile.owner_controller_id,controller_id) || strcmp(profile.id,profile_id))) {error="profile_owner_conflict"; goto refused;}
    snprintf(profile.id,sizeof profile.id,"%s",profile_id);
    snprintf(profile.name,sizeof profile.name,"%s",profile_id);
    snprintf(profile.protocol,sizeof profile.protocol,"%s",selected.protocol);
    profile.match_any_vid=profile.match_any_pid=0; profile.match_vid=selected.vid; profile.match_pid=selected.pid; profile.enabled=1;
    ScePadData input;
    if (!gc_service_inject_read_source_checked(&selected,&input,NULL,0,NULL)) {error="source_binding_mismatch"; goto refused;}
    if (gc_config_instance_check_held(feat_config,GC_INSTANCE_REQUIRE_LIVE,why,sizeof why)) {error=why; goto refused;}
    if (feat_setup.active && draft_instance && draft_epoch == gc_service_features_instance_revocation_epoch() &&
        feat_setup_source.binding.connection_generation == selected.binding.connection_generation &&
        !strcmp(draft_controller_id,controller_id) && !strcmp(draft_profile_id,profile_id))
        gc_setup_rebegin(&feat_setup,&profile,selected.path);
    else gc_setup_begin(&feat_setup,&profile,selected.path);
    feat_setup_source=selected; feat_setup_source_valid=feat_setup_input_ok=1;
    gc_setup_seed_buttons(&feat_setup,input.buttons); feat_setup.input=input;
    draft_instance=1; draft_epoch=gc_service_features_instance_revocation_epoch();
    snprintf(draft_controller_id,sizeof draft_controller_id,"%s",controller_id);
    snprintf(draft_profile_id,sizeof draft_profile_id,"%s",profile_id);
    gc_service_inject_instance_release(); gc_config_instance_release(feat_config);
    pthread_mutex_unlock(&feat_mu); return setup_status(out,cap,0);
refused:
    if (held) gc_service_inject_instance_release();
    gc_config_instance_release(feat_config); pthread_mutex_unlock(&feat_mu); return fail(error,NULL,out,cap);
invalid:
    json_object_put(o); return fail(error,NULL,out,cap);
}

static int setup_arm(const char *arg, char *out, size_t cap)
{
    int guided = arg && !strcmp(arg, "guided");
    int target = guided ? GC_BUTTON_CROSS : -1;
    int failed_arm;
    if (!guided && gc_setup_target_from_name(arg, &target))
        return fail("invalid_setup_target", NULL, out, cap);
    pthread_mutex_lock(&feat_mu);
    int leased = draft_consumer_acquire() == 0;
    if (!leased || !setup_source_current()) {
        if (leased) draft_consumer_release();
        pthread_mutex_unlock(&feat_mu);
        return fail("setup_source_changed", "Begin setup again for the current connection", out, cap);
    }
    failed_arm = gc_setup_arm(&feat_setup, target, guided, feat_pad_ms);
    draft_consumer_release();
    pthread_mutex_unlock(&feat_mu);
    if (failed_arm)
        return fail("setup_not_begun", "Begin setup with the connected pad first", out, cap);
    return setup_status(out, cap, 0);
}

typedef struct {
    GcServiceSource source;
    uint64_t epoch;
    int instance, rejected;
} SetupSourceLease;
static int setup_source_acquire(void *context)
{
    SetupSourceLease *lease = context;
    lease->rejected = gc_service_features_instance_draft_check_held(lease->epoch) != 0;
    if (!lease->rejected) lease->rejected = (lease->instance ?
        gc_service_inject_instance_acquire(&lease->source.binding) :
        gc_service_inject_source_acquire(&lease->source)) != 0;
    return lease->rejected ? -1 : 0;
}
static void setup_source_release(void *context)
{
    SetupSourceLease *lease = context;
    if (lease->instance) gc_service_inject_instance_release();
    else gc_service_inject_source_release();
}

static int setup_save(char *out, size_t cap)
{
    GcControllerRule rule;
    char error[256] = "";
    pthread_mutex_lock(&feat_mu);
    if (gc_service_features_instance_draft_check_held(draft_epoch)) {
        pthread_mutex_unlock(&feat_mu);
        return fail("source_binding_mismatch", "Begin setup first", out, cap);
    }
    if (!setup_source_current()) {
        pthread_mutex_unlock(&feat_mu);
        return fail("setup_source_changed", "The captured connection is no longer available", out, cap);
    }
    if (feat_setup.target >= 0) {
        pthread_mutex_unlock(&feat_mu);
        return fail("setup_capture_open", "Finish, skip or cancel the current control before saving", out, cap);
    }
    GcProfile profile = feat_setup.profile;
    SetupSourceLease lease = {.source=feat_setup_source, .epoch=draft_epoch, .instance=draft_instance};
    GcConfigSetupGuard guard = {setup_source_acquire, setup_source_release, &lease};
    uint64_t generation = 0;
    int committed = 0;
    /* Serialize this draft through commit/cancellation. The config writer lock
     * precedes source acquisition, matching resident dispatch lock ordering. */
    GcControllerResolution published = {0};
    int instance = draft_instance;
    int result = instance ? gc_config_save_setup_for_controller_guarded(feat_config,
        &profile,&lease.source.binding,draft_controller_id,draft_profile_id,&published,
        &guard,&committed,error,sizeof error) : feat_config ?
        gc_config_save_setup_preserving_rule_guarded(feat_config,
        &profile, &rule, &generation, &guard, &committed, error, sizeof error) : -1;
    if (instance) {
        /* Commit truth is independent of any subsequent revocation. Copies are
         * consumed only while a new LIVE lease and this same draft epoch hold. */
        int expose = result == 0 && gc_config_instance_acquire(feat_config,GC_INSTANCE_REQUIRE_LIVE,NULL,0) == 0;
        if (expose) {
            if (gc_service_features_instance_draft_check_held(lease.epoch)) { result=-1; memset(&published,0,sizeof published); snprintf(error,sizeof error,"source_binding_mismatch"); }
        } else if (result == 0) { result=-1; memset(&published,0,sizeof published); snprintf(error,sizeof error,"instance_policy_quarantined"); }
        if (committed) discard_draft();
        struct json_object *o=json_object_new_object(); instance_caps(o);
        json_object_object_add(o,"ok",json_object_new_boolean(result == 0));
        json_object_object_add(o,"saved",json_object_new_boolean(committed));
        json_object_object_add(o,"verified",json_object_new_boolean(result == 0));
        json_object_object_add(o,"applied",json_object_new_boolean(0));
        json_object_object_add(o,"output_supported",json_object_new_boolean(0));
        json_object_object_add(o,"effect",json_object_new_string("instance_setup_storage_only"));
        json_object_object_add(o,"scope",json_object_new_string("instance"));
        struct json_object *rule_object=NULL,*profile_object=NULL;
        if (result == 0) {
            decimal_field(o,"generation",published.generation);
            rule_object=json_object_new_object(); profile_object=json_object_new_object();
            json_object_object_add(rule_object,"id",json_object_new_string(published.rule.id));
            json_object_object_add(rule_object,"instance_id",json_object_new_string(published.rule.instance_id));
            json_object_object_add(rule_object,"profile_id",json_object_new_string(published.rule.profile_id));
            json_object_object_add(rule_object,"output_type",json_object_new_string(gc_controller_output_name(published.rule.output_type)));
            json_object_object_add(profile_object,"id",json_object_new_string(published.profile.id));
            json_object_object_add(profile_object,"owner_controller_id",json_object_new_string(published.profile.owner_controller_id));
            json_object_object_add(profile_object,"owner_instance_id",json_object_new_string(published.profile.owner_instance_id));
        } else json_object_object_add(o,"generation",NULL);
        json_object_object_add(o,"rule",rule_object); json_object_object_add(o,"profile",profile_object);
        json_object_object_add(o,"error",result ? json_object_new_string(*error ? error : "publication_failed") : NULL);
        int encoded=write_json(o,out,cap);
        if (expose) gc_config_instance_release(feat_config);
        pthread_mutex_unlock(&feat_mu);
        return result ? -1 : encoded;
    }
    if (result != 0 && !committed) {
        if (lease.rejected) {
            feat_setup_source_valid = 0;
            clear_setup_input();
            snprintf(feat_setup.message, sizeof feat_setup.message,
                     "Selected controller disconnected or changed. Begin setup again.");
        }
        pthread_mutex_unlock(&feat_mu);
        return fail(lease.rejected ? "setup_source_changed" : "setup_save_failed",
                    error[0] ? error : "save failed", out, cap);
    }
    gc_setup_cancel(&feat_setup, result == 0 ? "Profile saved." : "Profile committed; readback failed.");
    feat_setup_source_valid = 0; clear_setup_input();
    pthread_mutex_unlock(&feat_mu);
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(result == 0));
    json_object_object_add(o, "saved", json_object_new_boolean(1));
    json_object_object_add(o, "verified", json_object_new_boolean(result == 0));
    json_object_object_add(o, "accepted", json_object_new_boolean(1));
    json_object_object_add(o, "applied", json_object_new_boolean(0));
    json_object_object_add(o, "profile_id", json_object_new_string(profile.id));
    json_object_object_add(o, "source_connection_generation", json_object_new_uint64(lease.source.connection_generation));
    if (result == 0) {
        json_object_object_add(o, "controller_id", json_object_new_string(rule.id));
        json_object_object_add(o, "generation", json_object_new_uint64(generation));
    } else json_object_object_add(o, "error", json_object_new_string("setup_saved_readback_failed"));
    json_object_object_add(o, "active", json_object_new_boolean(0));
    json_object_object_add(o, "message", json_object_new_string(result == 0 ?
        "Profile saved." : "Profile committed but readback failed. Check saved state."));
    int serialized = write_json(o, out, cap);
    return result == 0 ? serialized : -1;
}

/* The frozen IPC worker has a 256 KiB stack. Its framing plus the frozen
 * guarded writer/load/parser chain exceeds that even without sanitizers.
 * Keep the transaction real and its ownership on ONE provider thread with an
 * explicit stack. The caller holds no locks/leases while joining, and the
 * worker acquires feature -> writer -> source and releases all before return. */
typedef struct { char *out; size_t cap; int result; } SetupSaveCall;
static void *setup_save_worker(void *context)
{
    SetupSaveCall *call = context;
    call->result = setup_save(call->out,call->cap);
    return NULL;
}
static int setup_save_on_stack(char *out,size_t cap)
{
    pthread_attr_t attr; pthread_t worker;
    SetupSaveCall call = {.out=out,.cap=cap,.result=-1};
    if (pthread_attr_init(&attr)) return fail("publication_failed",NULL,out,cap);
    int rc = pthread_attr_setstacksize(&attr,1024u*1024u);
    if (!rc) rc = pthread_create(&worker,&attr,setup_save_worker,&call);
    pthread_attr_destroy(&attr);
    if (rc) return fail("publication_failed",NULL,out,cap);
    if (pthread_join(worker,NULL)) abort(); /* never return a borrowed live buffer */
    return call.result;
}

static int bluetooth_status(char *out, size_t cap)
{
    GcBtStatus status;
    memset(&status, 0, sizeof(status));
    if (!feat_bt) {
        snprintf(status.version, sizeof(status.version), "off");
        snprintf(status.state, sizeof(status.state), "stopped");
        snprintf(status.reason, sizeof(status.reason), "Bluetooth engine did not start");
        return gc_bluetooth_format_status(&status, out, cap);
    }
    gc_bt_snapshot(feat_bt, &status);
    return gc_bluetooth_format_status(&status, out, cap);
}

static int bluetooth_command(const char *cmd, const char *arg, char *out, size_t cap)
{
    int ok = -1;
    if (!feat_bt) return fail("bluetooth_control_unavailable", NULL, out, cap);
    if (!strcmp(cmd, "BLUETOOTH_PAIR")) {
        gc_bt_set_enabled(feat_bt, 1);
        ok = gc_bt_pair(feat_bt, 60);
    } else if (!strcmp(cmd, "BLUETOOTH_RETRY")) {
        gc_bt_set_enabled(feat_bt, 1);
        ok = gc_bt_retry(feat_bt);
    } else if (!strcmp(cmd, "BLUETOOTH_FORGET")) {
        char canonical[18];
        if (gc_bluetooth_canonical_address(arg, canonical))
            return fail("invalid_bluetooth_address", NULL, out, cap);
        ok = gc_bt_forget(feat_bt, canonical);
    } else if (!strcmp(cmd, "BLUETOOTH_STOP")) {
        gc_bt_set_enabled(feat_bt, 0);
        ok = 0;
    } else {
        return fail("unknown_bluetooth_command", NULL, out, cap);
    }
    if (ok) return fail("bluetooth_control_unavailable", NULL, out, cap);
    {
        struct json_object *o = json_object_new_object();
        json_object_object_add(o, "ok", json_object_new_boolean(1));
        json_object_object_add(o, "command", json_object_new_string(cmd));
        json_object_object_add(o, "accepted", json_object_new_boolean(1));
        json_object_object_add(o, "pending", json_object_new_boolean(strcmp(cmd, "BLUETOOTH_STOP") != 0));
        json_object_object_add(o, "applied", json_object_new_boolean(!strcmp(cmd, "BLUETOOTH_STOP")));
        return write_json(o, out, cap);
    }
}

int gc_service_features_command(const char *cmd, const char *arg, char *out, size_t cap)
{
    if (!cmd) return 1;
    if (!arg) arg = "";
    if(gc_service_features_retiring() &&
       (!strncmp(cmd,"SETUP_",6) || !strcmp(cmd,"DEVICES") ||
        (!strncmp(cmd,"BLUETOOTH_",10) && strcmp(cmd,"BLUETOOTH_STATUS"))))
        return fail("input_release_uncertain","Native input cleanup is incomplete; no new input work was accepted",out,cap);
    int controller_result = gc_controller_command(feat_config, cmd, arg, out, cap);
    if (controller_result <= 0) return controller_result;
    if (!strcmp(cmd, "CONSOLE_ID")) return *arg ? fail("unexpected_argument", NULL, out, cap) : console_id(out, cap);
    if (!strcmp(cmd, "DEVICES")) return *arg ? fail("unexpected_argument", NULL, out, cap) : devices(out, cap);
    if (!strcmp(cmd, "SETUP_STATUS")) return setup_status(out, cap, 0);
    if (!strcmp(cmd, "SETUP_INPUT")) return setup_status(out, cap, 1);
    if (!strcmp(cmd, "SETUP_BEGIN_INSTANCE")) return setup_begin_instance(arg, out, cap);
    if (!strcmp(cmd, "SETUP_BEGIN")) return setup_begin(arg, out, cap);
    if (!strcmp(cmd, "SETUP_ARM")) return setup_arm(arg, out, cap);
    if (!strcmp(cmd, "SETUP_SKIP")) {
        pthread_mutex_lock(&feat_mu);
        int leased = draft_consumer_acquire() == 0;
        if (!leased || !setup_source_current()) {
            if (leased) draft_consumer_release();
            pthread_mutex_unlock(&feat_mu);
            return fail("setup_source_changed", "Begin setup again for the current connection", out, cap);
        }
        gc_setup_skip(&feat_setup, feat_pad_ms);
        draft_consumer_release();
        pthread_mutex_unlock(&feat_mu);
        return setup_status(out, cap, 0);
    }
    if (!strcmp(cmd, "SETUP_CANCEL")) {
        pthread_mutex_lock(&feat_mu);
        discard_draft();
        pthread_mutex_unlock(&feat_mu);
        return setup_status(out, cap, 0);
    }
    if (!strcmp(cmd, "SETUP_SAVE")) return setup_save_on_stack(out, cap);
    if (!strcmp(cmd, "BLUETOOTH_STATUS")) return bluetooth_status(out, cap);
    if (!strncmp(cmd, "BLUETOOTH_", 10)) return bluetooth_command(cmd, arg, out, cap);
    return 1;
}

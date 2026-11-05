/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ghost-Control v5: Multi-controller support
 * USB HID controllers → virtual DualSense devices on PS5
 *
 * Supports up to MAX_SLOTS (4) simultaneous controllers:
 *   - 8BitDo / Nintendo Switch Pro  (VID=057E PID=2009)
 *   - Xbox One S                    (VID=045E PID=02EA)
 *
 * Each detected controller gets its own VDA device, assignment dialog,
 * and VDI injection thread.
 * Hotplug: plug in any time, disconnect any time.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>
#include <dev/usb/usbdi.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif

#include "gc_types.h"
#include "gc_config.h"
#include "gc_state.h"
#include "gc_ipc.h"
#include "gc_notify.h"
#include "gc_update.h"
#include "gc_product_version.h"
#include "gc_ds4_bridge.h"
#include "gc_usb.h"
#include "gc_setup.h"
#include "gc_usb_retry.h"
#include "gc_raw_diag.h"
#include "gc_rawmap.h"
#include "gc_file_safety.h"
#include "gc_singleton.h"
#include "gc_bluetooth.h"
#include "gc_bt.h"
#include "gc_log_tail.h"
#include "gc_dualsense_fx.h"
#include "gc_console_id.h"
#include "gc_vda_klog.h"
#include "ghostPad/gc_ghostpad.h"
#include <json-c/json.h>
#include <time.h>
#include "usb_helpers.h"
#include "usb_helpers_state.h"
#include "controller_nintendo.h"
#include "controller_xbox.h"
#include "controller_ds4.h"
#include "controller_ds3.h"
#include "controller_logitech.h"
#include "controller_mamba.h"

/* ── Logging ──────────────────────────────────────────────────────────── */
#define GC_DIAG_ROOT GC_DATA_ROOT
#define LOG_DIR GC_DIAG_ROOT "/logs"
#define LOG_PATH LOG_DIR "/ghostcontrol.log"
#define RUN_LOG_DIR LOG_DIR "/runs"
#define SETUP_LOG_DIR LOG_DIR "/setup"
#define PID_PATH GC_DATA_ROOT "/runtime/core.pid"
#define XMB_ICON_PATH GC_DATA_ROOT "/i.png"
/* The bytecode HomeUI tile patches a fixed-length icon URI, so it uses the
 * 33-character /user/data/ghostcontrol/icon0.png spelling. Publish the same
 * bytes under both names so source-bundle and bytecode tiles both resolve. */
#define XMB_ICON_ALIAS_PATH GC_DATA_ROOT "/icon0.png"
#define GC_VERSION GC_PRODUCT_VERSION
#define LOG_MAX 1024

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_log_fd = -1;
static int g_run_log_fd = -1;
static int g_setup_log_fd = -1;
static char g_run_log_path[256];
static char g_setup_log_path[256];

static void close_log_fd(int *fd)
{
    if (*fd >= 0) {
        (void)fsync(*fd);
        (void)close(*fd);
        *fd = -1;
    }
}

static void write_log_fd(int fd, const char *data, size_t size)
{
    while (fd >= 0 && size) {
        ssize_t count = write(fd, data, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        data += count;
        size -= (size_t)count;
    }
}

static void write_log_record_locked(const char *message)
{
    struct timespec wall = {0}, mono = {0};
    (void)clock_gettime(CLOCK_REALTIME, &wall);
    (void)clock_gettime(CLOCK_MONOTONIC, &mono);
    char record[LOG_MAX + 128];
    int length = snprintf(record, sizeof(record),
        "[wall=%lld.%03ld mono=%lld.%03ld] %s",
        (long long)wall.tv_sec, wall.tv_nsec / 1000000L,
        (long long)mono.tv_sec, mono.tv_nsec / 1000000L, message);
    if (length < 0)
        return;
    size_t used = (size_t)length < sizeof(record) ? (size_t)length : sizeof(record) - 1;
    write_log_fd(g_log_fd, record, used);
    write_log_fd(g_run_log_fd, record, used);
    write_log_fd(g_setup_log_fd, record, used);
    if (!used || record[used - 1] != '\n') {
        write_log_fd(g_log_fd, "\n", 1);
        write_log_fd(g_run_log_fd, "\n", 1);
        write_log_fd(g_setup_log_fd, "\n", 1);
    }
}

void ghostpad_status_log_reset(void)
{
    pthread_mutex_lock(&g_log_lock);
    close_log_fd(&g_setup_log_fd);
    close_log_fd(&g_run_log_fd);
    close_log_fd(&g_log_fd);
    g_setup_log_path[0] = g_run_log_path[0] = '\0';
    if (gc_ensure_real_directory_tree(RUN_LOG_DIR, 0755) == 0 &&
        gc_ensure_real_directory_tree(SETUP_LOG_DIR, 0755) == 0) {
        int fd = gc_open_private_regular(LOG_PATH, O_WRONLY, 1);
        if (fd >= 0 && ftruncate(fd, 0) == 0)
            g_log_fd = fd;
        else if (fd >= 0)
            (void)close(fd);
        struct timespec now = {0};
        (void)clock_gettime(CLOCK_REALTIME, &now);
        snprintf(g_run_log_path, sizeof(g_run_log_path),
                 RUN_LOG_DIR "/run-%lld-pid%d.log",
                 (long long)now.tv_sec, (int)getpid());
        g_run_log_fd = gc_open_private_regular(g_run_log_path, O_WRONLY | O_APPEND, 1);
    }
    pthread_mutex_unlock(&g_log_lock);
}

static void ghostpad_setup_log_begin(uint16_t vid, uint16_t pid,
                                     const char *profile, int reuse)
{
    pthread_mutex_lock(&g_log_lock);
    if (!reuse || g_setup_log_fd < 0) {
        if (g_setup_log_fd >= 0)
            write_log_record_locked("[GC-SETUP] session-end reason=replaced-by-new-setup\n");
        close_log_fd(&g_setup_log_fd);
        struct timespec now = {0};
        (void)clock_gettime(CLOCK_REALTIME, &now);
        snprintf(g_setup_log_path, sizeof(g_setup_log_path),
                 SETUP_LOG_DIR "/setup-%lld-%04x-%04x-pid%d.log",
                 (long long)now.tv_sec, vid, pid, (int)getpid());
        g_setup_log_fd = gc_open_private_regular(g_setup_log_path,
                                                  O_WRONLY | O_APPEND, 1);
    }
    char event[256];
    snprintf(event, sizeof(event),
             "[GC-SETUP] %s profile=%s VID=%04x PID=%04x log=%s\n",
             reuse ? "rebegin" : "begin", profile ? profile : "", vid, pid,
             g_setup_log_path[0] ? g_setup_log_path : "unavailable");
    write_log_record_locked(event);
    pthread_mutex_unlock(&g_log_lock);
}

static void ghostpad_setup_log_close(const char *reason)
{
    pthread_mutex_lock(&g_log_lock);
    if (g_setup_log_fd >= 0) {
        char event[256];
        snprintf(event, sizeof(event), "[GC-SETUP] session-end reason=%s\n",
                 reason ? reason : "unknown");
        write_log_record_locked(event);
        close_log_fd(&g_setup_log_fd);
    }
    pthread_mutex_unlock(&g_log_lock);
}

void ghostpad_status_log(const char *fmt, ...)
{
    char buf[LOG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    pthread_mutex_lock(&g_log_lock);
    write_log_record_locked(buf);
    pthread_mutex_unlock(&g_log_lock);
}
#define gp_log(...) ghostpad_status_log("[GC] " __VA_ARGS__)

static void ghostpad_setup_log_profile(const GcProfile *profile)
{
    if (!profile)
        return;
    gp_log("setup profile snapshot: id=%s schema=%d VID=%04x PID=%04x protocol=%s learned semantic mappings follow\n",
           profile->id, profile->schema, profile->match_vid,
           profile->match_pid, profile->protocol);
    for (int button = 0; button < GC_BUTTON_INPUT15; ++button) {
        const GcHidButtonBinding *binding = &profile->hid_buttons[button];
        gp_log("setup binding button=%s target_mask=0x%08x pattern_present=%d pattern_mask=0x%08x hid_present=%d page=0x%04x usage=0x%04x offset=%u bits=%u report=%u active=%d\n",
               gc_button_name((GcButton)button), profile->button_targets[button],
               profile->button_patterns[button].present,
               profile->button_patterns[button].source_mask,
               binding->present, binding->source.page, binding->source.usage,
               binding->source.offset, binding->source.bits,
               binding->source.report, binding->active_value);
    }
    for (int axis = 0; axis < GC_AXIS_COUNT; ++axis) {
        const GcHidAxisBinding *hid = &profile->hid_axes[axis];
        const GcAxisBinding *semantic = &profile->axes[axis];
        gp_log("setup binding axis=%s hid_present=%d page=0x%04x usage=0x%04x offset=%u bits=%u report=%u logical=[%d,%d] center=%d polarity=%d semantic_present=%d semantic_source=%s semantic_center=%d semantic_range=[%d,%d] semantic_polarity=%d\n",
               gc_axis_name((GcAxis)axis), hid->present,
               hid->source.page, hid->source.usage, hid->source.offset,
               hid->source.bits, hid->source.report,
               hid->source.minimum, hid->source.maximum, hid->center,
               hid->polarity, semantic->present,
               gc_axis_name(semantic->source), semantic->center,
               semantic->minimum, semantic->maximum, semantic->polarity);
    }
}

/* ── SCE stubs ────────────────────────────────────────────────────────── */
extern int32_t sceUserServiceInitialize(void *params);
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadGetHandle(int32_t userId, int32_t type, int32_t index);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t deviceType);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *padData);

#define VIRTUAL_DEVICE_TYPE_DUALSENSE 3

typedef enum {
    CTRL_NONE = 0,
    CTRL_NINTENDO,
    CTRL_XBOX_GIP,
    CTRL_XUSB,
    CTRL_DS3,
    CTRL_DS4,
    CTRL_LOGITECH,
    CTRL_HID,
    CTRL_GHOSTPAD,
    CTRL_BT,        /* Bluetooth Classic HID pad */
    CTRL_BT_LE,     /* Bluetooth LE HID pad */
} controller_kind_t;

static const char *controller_kind_name(controller_kind_t kind) {
    switch (kind) {
    case CTRL_NINTENDO: return "Nintendo HID";
    case CTRL_XBOX_GIP: return "Xbox GIP";
    case CTRL_XUSB: return "XUSB";
    case CTRL_DS3: return "PS3 HID";
    case CTRL_DS4: return "PS4 HID";
    case CTRL_LOGITECH: return "Logitech HID";
    case CTRL_HID: return "DirectInput / USB HID gamepad";
    case CTRL_GHOSTPAD: return "GhostPad Remote Controller";
    case CTRL_BT: return "Bluetooth HID controller";
    case CTRL_BT_LE: return "Bluetooth LE HID controller";
    default: return "unknown";
    }
}

static const char *controller_kind_key(controller_kind_t kind) {
    switch (kind) {
    case CTRL_NINTENDO: return "nintendo";
    case CTRL_XBOX_GIP: return "xbox_gip";
    case CTRL_XUSB: return "xusb";
    case CTRL_DS3: return "ds3";
    case CTRL_DS4: return "ds4";
    case CTRL_LOGITECH: return "logitech";
    case CTRL_HID: return "dinput";
    case CTRL_GHOSTPAD: return "ghostpad";
    case CTRL_BT: return "bt";
    case CTRL_BT_LE: return "ble";
    default: return "unknown";
    }
}

static controller_kind_t controller_kind_from_key(const char *key) {
    if (strcmp(key, "nintendo") == 0) return CTRL_NINTENDO;
    if (strcmp(key, "xbox_gip") == 0) return CTRL_XBOX_GIP;
    if (strcmp(key, "xusb") == 0) return CTRL_XUSB;
    if (strcmp(key, "ds3") == 0) return CTRL_DS3;
    if (strcmp(key, "ds4") == 0) return CTRL_DS4;
    if (strcmp(key, "logitech") == 0) return CTRL_LOGITECH;
    if (strcmp(key, "hid") == 0 || strcmp(key, "dinput") == 0) return CTRL_HID;
    if (strcmp(key, "bt") == 0) return CTRL_BT;
    if (strcmp(key, "ble") == 0) return CTRL_BT_LE;
    return CTRL_NONE;
}

static const char *controller_display_name(controller_kind_t kind,
                                           uint16_t vid, uint16_t pid)
{
    if (kind == CTRL_NINTENDO && vid == 0x057eu && pid == 0x2009u)
        return "Nintendo Switch Pro / compatible";
    if (kind == CTRL_NINTENDO && vid == 0x2dc8u && pid == 0x310bu)
        return "8BitDo Native";
    if (kind == CTRL_XUSB)
        return mamba_name(vid, pid);
    if (kind == CTRL_XBOX_GIP && vid == 0x3537u && pid == 0x1010u)
        return "GameSir G7 SE (GIP)";
    if (kind == CTRL_XBOX_GIP)
        return "Xbox One/Series GIP controller";
    if (kind == CTRL_DS3 && vid == VID_SONY && pid == PID_DS3)
        return "DualShock 3 / SIXAXIS";
    if (kind == CTRL_DS3)
        return "PS3-protocol third-party";
    if (kind == CTRL_DS4 && vid == VID_SONY)
        return "DualShock 4";
    if (kind == CTRL_DS4 && vid == VID_HORI)
        return "HORIPAD / DS4-compatible (XIM4)";
    if (kind == CTRL_DS4)
        return "DS4-protocol third-party";
    if (kind == CTRL_LOGITECH)
        return "Logitech RumblePad / DInput pad";
    if (kind == CTRL_HID)
        return "DirectInput / USB HID gamepad";
    return "Controller";
}

/* ── Multi-controller slots ───────────────────────────────────────────── */
#define MAX_SLOTS 4
#define GC_RAW_REPORT_MAX 1024
#define GC_USB_RETRY_LIMIT 3

typedef struct {
    int32_t          handle;        /* VDA handle, -1 = slot free */
    int              vdi_ready;
    int              usb_active;    /* USB reader thread is running */
    int              setup_reader_active; /* initialized IN path accepts setup */
    int              confirmed;     /* PS5 reported a manually selected owner */
    int              usb_fd;        /* open ugen fd, -1 if none — for clean teardown */
    char             dev_path[32];  /* ugen path claimed by this slot */
    uint16_t         vid, pid;
    controller_kind_t kind;
    uint32_t         inject_count;
    unsigned         vdi_error_count;
    int              vda_delete_pending;
    unsigned         vda_delete_attempts;
    uint64_t         vda_delete_after_ms;
    ScePadData       last_input;    /* parsed state before profile remapping */
    uint32_t         last_setup_buttons;
    int              last_input_valid;
    uint8_t          last_raw_report[GC_RAW_REPORT_MAX];
    uint16_t         last_raw_length;
    uint16_t         last_raw_copied;
    uint64_t         last_raw_ms;
    uint32_t         raw_report_count;
    uint32_t         parsed_report_count;
    int              last_parse_result;
    uint8_t          input_endpoint, output_endpoint;
    int              interface_index, interface_number;
    GcHidLayout      hid_layout;
    int              hid_layout_valid;
    unsigned         retry_count;
    uint64_t         retry_after_ms;
    int              retry_pending;
    int              retry_ready;
    char             retry_path[32];
    uint16_t         retry_vid, retry_pid;
} ctrl_slot_t;

static ctrl_slot_t     g_slots[MAX_SLOTS];
static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
/* Serializes service start/stop against the manager's final claim + launch.
 * Probing may run without this lock, but a post-probe recheck under the lock
 * guarantees that STOP cannot race a new VDA or worker into existence. */
static pthread_mutex_t g_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static GcConfigStore  *g_config;
static GcState        *g_state;
static GcIpcServer    *g_ipc;

/* The server logs every step it takes (accepted client / read line / wrote
 * response). The callback was left unwired, and that cost a whole debugging
 * round: when a handler wedged the accept loop, the run log said nothing about
 * which client or which request it was. The messages already carry an "ipc: "
 * prefix, so none is added here. */
static void ipc_log_line(void *context, const char *message)
{
    (void)context;
    gp_log("%s\n", message ? message : "");
}
static GcGhostpad     *g_ghostpad;
static GcBt           *g_bt;
/* Bluetooth bridge state. Every gc_bt callback runs on the single gc_bt
 * engine thread, so these are touched by that one thread only and need no
 * lock of their own; the shared g_slots/g_setup they reach into keep theirs. */
static int               g_bt_gcslot[GC_BT_MAX_PADS];
static controller_kind_t g_bt_kind[GC_BT_MAX_PADS];
static uint16_t          g_bt_vid[GC_BT_MAX_PADS], g_bt_pid[GC_BT_MAX_PADS];
static char              g_bt_path[GC_BT_MAX_PADS][32];
static GcBtPad           g_bt_last[GC_BT_MAX_PADS];
static int               g_bt_last_valid[GC_BT_MAX_PADS];
static GcHidLayout       g_bt_layout[GC_BT_MAX_PADS];
static int               g_bt_layout_valid[GC_BT_MAX_PADS];
static GcProfile         g_bt_profile[GC_BT_MAX_PADS];
static uint64_t          g_bt_profile_gen[GC_BT_MAX_PADS];
static int               g_bt_ready_notified[GC_BT_MAX_PADS];
static atomic_int      g_service_enabled;
static atomic_int      g_shutdown;
static int             g_pid_fd = -1;
static volatile sig_atomic_t g_signal_shutdown;
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_setup_lock = PTHREAD_MUTEX_INITIALIZER;
static GcSetup g_setup;

int ghostcontrol_is_owned_vda_handle(int32_t handle)
{
    if (handle <= 0)
        return 0;
    int owned = 0;
    pthread_mutex_lock(&g_slot_lock);
    for (int slot = 0; slot < MAX_SLOTS; ++slot) {
        if (g_slots[slot].handle == handle) {
            owned = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    return owned;
}

int ghostcontrol_setup_busy(void)
{
    pthread_mutex_lock(&g_setup_lock);
    const int busy = g_setup.active;
    pthread_mutex_unlock(&g_setup_lock);
    return busy;
}

unsigned ghostcontrol_copy_owned_vda_handles(int32_t *handles, unsigned capacity)
{
    if (!handles || !capacity)
        return 0;
    unsigned count = 0;
    pthread_mutex_lock(&g_slot_lock);
    for (int slot = 0; slot < MAX_SLOTS && count < capacity; ++slot)
        if (g_slots[slot].handle > 0)
            handles[count++] = g_slots[slot].handle;
    pthread_mutex_unlock(&g_slot_lock);
    return count;
}

static void reset_slot_record(ctrl_slot_t *slot)
{
    memset(slot, 0, sizeof(*slot));
    slot->handle = -1;
    slot->usb_fd = -1;
    slot->interface_index = -1;
    slot->interface_number = -1;
}

static uint64_t setup_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* Caller owns g_slot_lock. Keep a failed launch bound to its physical
 * identity so a different controller cannot inherit this retry budget. */
static int schedule_slot_retry_locked(ctrl_slot_t *slot, int enabled,
                                      unsigned delay_ms, const char *path,
                                      uint16_t vid, uint16_t pid)
{
    slot->retry_pending = 0;
    slot->retry_ready = 0;
    if (!gc_usb_retry_can_schedule(enabled, slot->retry_count,
                                   GC_USB_RETRY_LIMIT))
        return 0;
    ++slot->retry_count;
    slot->retry_after_ms = setup_now_ms() + delay_ms;
    slot->retry_pending = 1;
    snprintf(slot->retry_path, sizeof(slot->retry_path), "%s", path);
    slot->retry_vid = vid;
    slot->retry_pid = pid;
    return 1;
}

static int usb_stop_and_drain(int fd, uint8_t endpoint_index,
                              unsigned timeout_ms)
{
    struct usb_fs_stop stop;
    memset(&stop, 0, sizeof(stop));
    stop.ep_index = endpoint_index;
    if (ioctl(fd, USB_FS_STOP, &stop) != 0)
        return -1;
    const uint64_t deadline = setup_now_ms() + timeout_ms;
    do {
        struct usb_fs_complete complete;
        memset(&complete, 0, sizeof(complete));
        complete.ep_index = endpoint_index;
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
            if (complete.ep_index == endpoint_index) return 0;
            continue; /* Shared completion queue: dispatch only our endpoint. */
        }
        if (errno != EBUSY && errno != EINTR) return -1;
        usleep(2000);
    } while (setup_now_ms() < deadline);
    errno = ETIMEDOUT;
    return -1;
}

static int xbox_service_keep_running(void *context)
{
    (void)context;
    return atomic_load(&g_service_enabled) && !atomic_load(&g_shutdown);
}

static GcUpdateInfo    g_latest_update;
static int             g_latest_update_valid;
/* Assignment serialization: only ONE controller may show its assignment
 * dialog at a time. g_assign_slot = slot awaiting system ownership, or -1.
 * Without this, multiple dialogs stack and all bind to the same user. */
static atomic_int      g_assign_slot = ATOMIC_VAR_INIT(-1);

/* The VDA call creates an MBus device on this firmware while returning an
 * error-valued result. klog is the only proven source for that device's
 * insertion/deletion handle; no account binding is performed here. */
#define GC_VDA_EVENT_CAPACITY 16
typedef struct {
    uint64_t device_id;
    int32_t user_id;
} GcVdaOwner;
static uint64_t g_vda_added[GC_VDA_EVENT_CAPACITY];
static unsigned g_vda_added_read;
static unsigned g_vda_added_write;
static GcVdaOwner g_vda_owners[GC_VDA_EVENT_CAPACITY];
static unsigned g_vda_owner_next;
static pthread_mutex_t g_vda_event_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_vda_klog_online = ATOMIC_VAR_INIT(0);

static void vda_record_event(const GcVdaKlogEvent *event)
{
    pthread_mutex_lock(&g_vda_event_lock);
    if (event->type == GC_VDA_KLOG_ADDED) {
        unsigned next = (g_vda_added_write + 1) % GC_VDA_EVENT_CAPACITY;
        if (next != g_vda_added_read) {
            g_vda_added[g_vda_added_write] = event->device_id;
            g_vda_added_write = next;
        }
    }
    unsigned owner_index = GC_VDA_EVENT_CAPACITY;
    for (unsigned index = 0; index < GC_VDA_EVENT_CAPACITY; ++index)
        if (g_vda_owners[index].device_id == event->device_id ||
            g_vda_owners[index].device_id == 0) {
            owner_index = index;
            break;
        }
    if (owner_index == GC_VDA_EVENT_CAPACITY) {
        owner_index = g_vda_owner_next;
        g_vda_owner_next = (g_vda_owner_next + 1) % GC_VDA_EVENT_CAPACITY;
    }
    g_vda_owners[owner_index].device_id = event->device_id;
    if (event->type == GC_VDA_KLOG_OWNER_CHANGED)
        g_vda_owners[owner_index].user_id = event->user_id;
    else if (event->type == GC_VDA_KLOG_ADDED)
        g_vda_owners[owner_index].user_id = 0;
    pthread_mutex_unlock(&g_vda_event_lock);
}

static void vda_clear_added_events(void)
{
    pthread_mutex_lock(&g_vda_event_lock);
    g_vda_added_read = g_vda_added_write;
    pthread_mutex_unlock(&g_vda_event_lock);
}

static uint64_t vda_wait_added(unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed += 20) {
        pthread_mutex_lock(&g_vda_event_lock);
        if (g_vda_added_read != g_vda_added_write) {
            uint64_t device_id = g_vda_added[g_vda_added_read];
            g_vda_added_read = (g_vda_added_read + 1) % GC_VDA_EVENT_CAPACITY;
            pthread_mutex_unlock(&g_vda_event_lock);
            return device_id;
        }
        pthread_mutex_unlock(&g_vda_event_lock);
        usleep(20000);
    }
    return 0;
}

static int32_t vda_assigned_user(uint64_t device_id)
{
    int32_t user_id = 0;
    pthread_mutex_lock(&g_vda_event_lock);
    for (unsigned index = 0; index < GC_VDA_EVENT_CAPACITY; ++index)
        if (g_vda_owners[index].device_id == device_id) {
            user_id = g_vda_owners[index].user_id;
            break;
        }
    pthread_mutex_unlock(&g_vda_event_lock);
    return user_id;
}

/* /dev/klog is a single-reader device on this console — DevLoop and any other
 * klog tool need it too. It used to be opened at startup and held for the whole
 * process lifetime, which locked every other reader out. Hold it only while a
 * VDA is being created: the DEVICE_ADDED device id is needed for that insert
 * and for nothing else. */
static pthread_mutex_t g_vda_klog_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_vda_klog_tid;
static int g_vda_klog_running;
static int g_vda_klog_users;
static atomic_int g_vda_klog_stop = ATOMIC_VAR_INIT(0);

static void *vda_klog_thread(void *unused)
{
    (void)unused;
    char buffer[512];
    char line[1024];
    size_t line_length = 0;
    while (!atomic_load(&g_vda_klog_stop) && !atomic_load(&g_shutdown)) {
        int fd = open("/dev/klog", O_RDONLY);
        if (fd < 0) {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_in address;
                memset(&address, 0, sizeof(address));
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = inet_addr("127.0.0.1");
                address.sin_port = htons(3232);
                if (connect(fd, (struct sockaddr *)&address,
                            sizeof(address)) != 0) {
                    close(fd);
                    fd = -1;
                }
            }
        }
        if (fd < 0) {
            if (atomic_load(&g_vda_klog_stop) || atomic_load(&g_shutdown))
                break;
            usleep(100000);
            continue;
        }
        atomic_store(&g_vda_klog_online, 1);
        gp_log("VDA klog capture connected\n");
        while (!atomic_load(&g_vda_klog_stop) && !atomic_load(&g_shutdown)) {
            struct pollfd poll_fd = {fd, POLLIN, 0};
            int ready = poll(&poll_fd, 1, 250);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0)
                continue;
            ssize_t length = read(fd, buffer, sizeof(buffer));
            if (length <= 0)
                break;
            for (ssize_t index = 0; index < length; ++index) {
                char byte = buffer[index];
                if (byte == '\n' || line_length == sizeof(line) - 1) {
                    line[line_length] = '\0';
                    GcVdaKlogEvent event;
                    if (gc_vda_klog_parse(line, &event))
                        vda_record_event(&event);
                    line_length = 0;
                } else if (byte != '\r') {
                    line[line_length++] = byte;
                }
            }
        }
        atomic_store(&g_vda_klog_online, 0);
        close(fd);
        line_length = 0;
    }
    return NULL;
}

/* Take klog for one VDA insert. Reference counted: the reader exists only
 * while at least one insert is in flight. */
static void vda_klog_acquire(void)
{
    pthread_mutex_lock(&g_vda_klog_lock);
    ++g_vda_klog_users;
    if (!g_vda_klog_running) {
        atomic_store(&g_vda_klog_stop, 0);
        if (pthread_create(&g_vda_klog_tid, NULL, vda_klog_thread, NULL) == 0) {
            g_vda_klog_running = 1;
            gp_log("VDA klog capture started (on demand)\n");
        } else {
            gp_log("VDA klog capture thread failed to start\n");
        }
    }
    pthread_mutex_unlock(&g_vda_klog_lock);
}

/* Give klog back as soon as the insert is done. */
static void vda_klog_release(void)
{
    pthread_mutex_lock(&g_vda_klog_lock);
    if (g_vda_klog_users > 0)
        --g_vda_klog_users;
    if (g_vda_klog_users == 0 && g_vda_klog_running) {
        atomic_store(&g_vda_klog_stop, 1);
        pthread_join(g_vda_klog_tid, NULL);
        g_vda_klog_running = 0;
        gp_log("VDA klog capture released\n");
    }
    pthread_mutex_unlock(&g_vda_klog_lock);
}

static void vda_klog_shutdown(void)
{
    pthread_mutex_lock(&g_vda_klog_lock);
    if (g_vda_klog_running) {
        atomic_store(&g_vda_klog_stop, 1);
        pthread_join(g_vda_klog_tid, NULL);
        g_vda_klog_running = 0;
    }
    pthread_mutex_unlock(&g_vda_klog_lock);
}

static void publish_slot_state(int slot, const char *name, const char *profile,
                               int streaming, int32_t bound_user)
{
    if (!g_state || slot < 0 || slot >= MAX_SLOTS)
        return;
    GcSlotState value;
    memset(&value, 0, sizeof(value));
    unsigned fields = GC_STATE_SLOT_PATCH_LIVE;
    pthread_mutex_lock(&g_slot_lock);
    value.connected = g_slots[slot].usb_active;
    value.vid = g_slots[slot].vid;
    value.pid = g_slots[slot].pid;
    value.vda_handle = g_slots[slot].handle;
    value.inject_count = g_slots[slot].inject_count;
    snprintf(value.protocol, sizeof(value.protocol), "%s",
             controller_kind_key(g_slots[slot].kind));
    snprintf(value.device_path, sizeof(value.device_path), "%s",
             g_slots[slot].dev_path);
    pthread_mutex_unlock(&g_slot_lock);
    if (name) {
        snprintf(value.name, sizeof(value.name), "%s", name);
        fields |= GC_STATE_SLOT_PATCH_NAME;
    }
    if (profile) {
        snprintf(value.profile, sizeof(value.profile), "%s", profile);
        fields |= GC_STATE_SLOT_PATCH_PROFILE;
    }
    if (streaming >= 0) {
        value.streaming = streaming;
        fields |= GC_STATE_SLOT_PATCH_STREAMING;
    }
    if (bound_user >= -1) {
        value.bound_user = bound_user;
        fields |= GC_STATE_SLOT_PATCH_BOUND_USER;
    }
    gc_state_patch_slot(g_state, (size_t)slot, &value, fields);
}

/* ── Notification ─────────────────────────────────────────────────────── */
static void notify(const char *fmt, ...) {
    static pthread_mutex_t notification_lock = PTHREAD_MUTEX_INITIALIZER;
    static char previous_message[768];
    static uint64_t previous_ms;
    char message[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    message[sizeof(message) - 1] = '\0';
    const uint64_t now = setup_now_ms();
    pthread_mutex_lock(&notification_lock);
    if (!strcmp(previous_message, message) && now - previous_ms < 3000u) {
        pthread_mutex_unlock(&notification_lock);
        return;
    }
    snprintf(previous_message, sizeof(previous_message), "%s", message);
    previous_ms = now;
    pthread_mutex_unlock(&notification_lock);
    gc_state_note(g_state, message);
    if (gc_notify_send(message) != 0)
        gp_log("notification send failed: %s\n", message);
}

/* ── VDI injection ────────────────────────────────────────────────────── */
static int inject_pad(int slot, const ScePadData *pad) {
    pthread_mutex_lock(&g_slot_lock);
    int32_t h = g_slots[slot].handle;
    int ready = g_slots[slot].vdi_ready;
    pthread_mutex_unlock(&g_slot_lock);
    if (h < 0 || !ready) return -1;
    int vr = scePadVirtualDeviceInsertData(h, pad);
    pthread_mutex_lock(&g_slot_lock);
    uint32_t n = g_slots[slot].inject_count;
    unsigned failures;
    if (vr == 0) {
        n = ++g_slots[slot].inject_count;
        g_slots[slot].vdi_error_count = 0;
        failures = 0;
    } else {
        failures = ++g_slots[slot].vdi_error_count;
    }
    pthread_mutex_unlock(&g_slot_lock);
    if (vr == 0 && n != 0 && (n % 600) == 0)
        gp_log("slot[%d] VDI #%u ret=0x%08x\n", slot, n, (uint32_t)vr);
    if (vr != 0 && (failures == 1 || failures == 3))
        gp_log("slot[%d] VDI error 0x%08x consecutive=%u\n",
               slot, (uint32_t)vr, failures);
    /* A stale post-rest VDA handle can otherwise look streaming forever.
     * Three consecutive failures trigger normal teardown + bounded recreate. */
    return (vr == 0 || failures < 3) ? 0 : -1;
}

static int handle_sparse_no_data(int slot, controller_kind_t kind,
                                 int setup_only,
                                 const char *dev_path, int *cache_valid,
                                 uint64_t last_input_ms,
                                 unsigned stale_release_ms,
                                 ScePadData *cached_pad)
{
    if (kind != CTRL_LOGITECH && kind != CTRL_HID && kind != CTRL_XUSB)
        return 0;
    if (*cache_valid && usb_input_cache_should_release(
            setup_now_ms(), last_input_ms, stale_release_ms)) {
        cached_pad->buttons = 0;
        cached_pad->analogButtons.l2 = 0;
        cached_pad->analogButtons.r2 = 0;
    }
    if (!*cache_valid)
        return 0;

    ScePadData repeat = *cached_pad;
    pthread_mutex_lock(&g_setup_lock);
    int learning = g_setup.active && !strcmp(g_setup.path, dev_path) &&
                   (g_setup.target >= 0 || !g_setup.neutral);
    pthread_mutex_unlock(&g_setup_lock);
    if (learning) {
        repeat.buttons = 0;
        repeat.leftStick.x = repeat.leftStick.y = 128;
        repeat.rightStick.x = repeat.rightStick.y = 128;
        repeat.analogButtons.l2 = repeat.analogButtons.r2 = 0;
    }
    return !setup_only && inject_pad(slot, &repeat) != 0 ? -1 : 0;
}

/* ── ugen detection ───────────────────────────────────────────────────── */
#define VID_NATIVE  0x2dc8u
#define PID_NATIVE  0x310bu
#define VID_SWITCH  0x057eu
#define PID_SWITCH  0x2009u
#define VID_XBOX    0x045eu
#define PID_XBOX    0x02eau

/* Xbox One/Series GIP interface signature (USB interface descriptor).
 * Confirmed on PS5 hardware: bInterfaceSubClass=0x47, bInterfaceProtocol=0xD0.
 * This matches EVERY Xbox One/Series controller regardless of PID, unlike the
 * single hardcoded PID_XBOX. See issue #2. */
#define XBOX_GIP_SUBCLASS  0x47u
#define XBOX_GIP_PROTOCOL  0xD0u

/* Path list is now built dynamically per scan from /dev — see manager loop. */

/* DS3 must precede DS4 because Sony and HORI identities overlap. */
static controller_kind_t match_known_vidpid(uint16_t vid, uint16_t pid) {
    if (ds3_is_compatible_vidpid(vid, pid)) return CTRL_DS3;
    if (ds4_is_compatible_vidpid(vid, pid)) return CTRL_DS4;
    if (mamba_is_xinput_vidpid(vid, pid)) return CTRL_XUSB;
    if ((vid == VID_SWITCH && pid == PID_SWITCH) ||
        (vid == VID_NATIVE && pid == PID_NATIVE)) return CTRL_NINTENDO;
    if (logitech_is_compatible_vidpid(vid, pid)) return CTRL_LOGITECH;
    if (vid == VID_XBOX && pid == PID_XBOX) return CTRL_XBOX_GIP;
    return CTRL_NONE;
}

static GcUsbInterfaceKind usb_interface_kind(controller_kind_t kind)
{
    if (kind == CTRL_XBOX_GIP) return GC_USB_INTERFACE_GIP;
    if (kind == CTRL_XUSB) return GC_USB_INTERFACE_XUSB;
    if (kind == CTRL_HID) return GC_USB_INTERFACE_HID;
    if (kind == CTRL_DS3 || kind == CTRL_DS4 || kind == CTRL_NINTENDO ||
        kind == CTRL_LOGITECH)
        return GC_USB_INTERFACE_HID_RAW;
    return GC_USB_INTERFACE_NONE;
}

static int usb_interface_has_endpoint(const GcUsbDevice *device,
                                      uint8_t endpoint, int input)
{
    if (!endpoint) return 1;
    const uint8_t *items = input ? device->in : device->out;
    const unsigned count = input ? device->in_count : device->out_count;
    for (unsigned index = 0; index < count; ++index)
        if (items[index] == endpoint) return 1;
    return 0;
}

/* Probe a /dev/ugen* path to identify controller type.
 * Returns 1 with vid/pid set, 0 if not a known controller.
 *
 * Identification is descriptor-only and non-destructive. No interface is
 * detached until a validated interface or explicit rule identifies it. */
static int probe_one_path(const char *path, uint16_t *out_vid, uint16_t *out_pid,
                          controller_kind_t *out_kind,
                          GcControllerRule *out_rule,
                          GcUsbDevice *out_usb) {
    if (!path || !out_vid || !out_pid || !out_kind || !out_usb)
        return 0;
    memset(out_usb, 0, sizeof(*out_usb));
    out_usb->interface_index = -1;
    out_usb->interface_number = -1;
    out_usb->alternate_setting = -1;
    if (out_rule) memset(out_rule, 0, sizeof(*out_rule));
    int fd = open(path, O_RDWR|O_NONBLOCK);
    if (fd < 0) return 0;

    /* Safe path: read device descriptor. Works on any bus.
     * PS5 ugen driver does NOT implement USB_GET_DEVICEINFO (errno 25),
     * but USB_GET_DEVICE_DESC works — returns the raw USB descriptor with
     * idVendor/idProduct as little-endian uWord arrays.
     *
     * IMPORTANT: if the descriptor ioctl succeeds we MUST NOT fall through
     * to the destructive endpoint probe below, even if the VID/PID isn't a
     * recognized controller. The fallback calls USB_IFACE_DRIVER_DETACH on
     * interfaces 0..2 which yanks whatever driver currently owns the device
     * — e.g. the mass-storage driver for an external HDD. PS5 doesn't
     * auto-reattach. */
    struct usb_device_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    if (ioctl(fd, USB_GET_DEVICE_DESC, &desc) == 0) {
        const uint16_t vid = UGETW(desc.idVendor);
        const uint16_t pid = UGETW(desc.idProduct);
        *out_vid = vid;
        *out_pid = pid;
        out_usb->vid = vid;
        out_usb->pid = pid;
        snprintf(out_usb->path, sizeof(out_usb->path), "%s", path);
        gp_log("scan: %s VID=0x%04x PID=0x%04x\n", path, vid, pid);
        close(fd);

        GcControllerRule rule;
        memset(&rule, 0, sizeof(rule));
        rule.preferred_interface_number = -1;
        if (g_config &&
            gc_config_find_controller(g_config, vid, pid, &rule, NULL) == 0 &&
            rule.ignore) {
            gp_log("scan: %s VID=0x%04x PID=0x%04x ignored by rule=%s\n",
                   path, vid, pid, rule.id);
            return 0;
        }

        GcUsbDevice discovered;
        int inspected;
        if (rule.id[0]) {
            *out_kind = controller_kind_from_key(rule.protocol);
            GcUsbInterfaceKind required = usb_interface_kind(*out_kind);
            inspected = required != GC_USB_INTERFACE_NONE &&
                gc_usb_inspect_matching(path, required,
                    rule.preferred_interface_number, &discovered) == 0 &&
                discovered.vid == vid && discovered.pid == pid;
            if (!inspected ||
                !usb_interface_has_endpoint(&discovered,
                    rule.preferred_in_endpoint, 1) ||
                !usb_interface_has_endpoint(&discovered,
                    rule.preferred_out_endpoint, 0)) {
                gp_log("scan: %s rule=%s does not match one USB interface\n",
                       path, rule.id);
                /* A stale endpoint/interface rule must not make its own repair
                 * impossible. Fall back to safe descriptor-only detection,
                 * clear admission, and launch only a setup reader. */
                memset(&rule, 0, sizeof(rule));
                rule.preferred_interface_number = -1;
                *out_kind = CTRL_NONE;
                inspected = gc_usb_inspect(path, &discovered) == 0 &&
                            discovered.vid == vid && discovered.pid == pid;
            }
        } else {
            inspected = gc_usb_inspect(path, &discovered) == 0 &&
                        discovered.vid == vid && discovered.pid == pid;
        }
        if (inspected)
            *out_usb = discovered;

        /* A user override wins. Otherwise prefer strong interface protocol
         * signatures, then known VID:PID quirks, then descriptor-validated
         * DirectInput/HID. This avoids selecting an auxiliary HID interface
         * on composite XInput controllers. */
        if (rule.id[0]) {
            if (out_rule) *out_rule = rule;
        } else if (inspected && discovered.kind == GC_USB_INTERFACE_GIP) {
            *out_kind = CTRL_XBOX_GIP;
        } else if (inspected && discovered.kind == GC_USB_INTERFACE_XUSB) {
            *out_kind = CTRL_XUSB;
        } else {
            *out_kind = match_known_vidpid(vid, pid);
            if (*out_kind == CTRL_NONE && inspected &&
                discovered.kind == GC_USB_INTERFACE_HID)
                *out_kind = CTRL_HID;
        }

        if (*out_kind == CTRL_NONE)
            return 0;
        gp_log("scan: %s VID=0x%04x PID=0x%04x protocol=%s iface=%d IN=0x%02x OUT=0x%02x%s\n",
               path, vid, pid, controller_kind_name(*out_kind),
               out_usb->interface_index,
               out_usb->in_count ? out_usb->in[0] : 0,
               out_usb->out_count ? out_usb->out[0] : 0,
               rule.id[0] ? " override" : "");
        return 1;
    }
    gp_log("scan: %s USB_GET_DEVICE_DESC errno=%d; skipping safely\n",
           path, errno);
    close(fd);
    return 0;
}

/* ── Create VDA for a slot ────────────────────────────────────────────── */
static int32_t create_vda_for_slot(int slot) {
    struct { int32_t size; int32_t userId; int32_t pad[6]; } vdp;
    const int32_t SEN = (int32_t)0xDEADBEEFu;
    memset(&vdp,0,sizeof(vdp)); vdp.size=sizeof(vdp); vdp.userId=1;
    for(int k=0;k<6;k++) vdp.pad[k]=SEN;

    /* Hold klog only for this insert, then hand it straight back. */
    vda_klog_acquire();
    for (unsigned waited = 0;
         waited < 5000 && !atomic_load(&g_vda_klog_online); waited += 20)
        usleep(20000);
    vda_clear_added_events();

    int ret = scePadVirtualDeviceAddDevice(&vdp, VIRTUAL_DEVICE_TYPE_DUALSENSE);
    gp_log("slot[%d] VDA ret=0x%08x\n", slot, (uint32_t)ret);

    int32_t handle = gc_vda_select_handle(ret, vdp.pad, SEN, 0);
    uint64_t device_id = 0;
    if (handle < 0 && atomic_load(&g_vda_klog_online)) {
        device_id = vda_wait_added(5000);
        handle = gc_vda_select_handle(ret, vdp.pad, SEN, device_id);
    }

    if (handle >= 0) {
        gp_log("slot[%d] VDI handle=0x%x source=%s ready\n", slot,
               (uint32_t)handle, device_id ? "MBus device event" : "VDA result");
    }
    else
        gp_log("slot[%d] ERROR: VDA created without recoverable handle; retry blocked\n",
               slot);
    vda_klog_release();
    return handle;
}

/* Deletion is part of VDA ownership, not best-effort cleanup. If ShellUI
 * rejects deletion after rest mode, retain the handle and quarantine this
 * slot; the manager retries the same handle and must not create a replacement
 * until deletion is confirmed. */
static int release_vda_for_slot(int slot, int32_t handle)
{
    if (handle < 0)
        return 0;
    int result = scePadVirtualDeviceDeleteDevice(handle);
    pthread_mutex_lock(&g_slot_lock);
    if (g_slots[slot].handle == handle) {
        g_slots[slot].vdi_ready = 0;
        if (result == 0) {
            g_slots[slot].handle = -1;
            g_slots[slot].vda_delete_pending = 0;
            g_slots[slot].vda_delete_attempts = 0;
            g_slots[slot].vda_delete_after_ms = 0;
        } else {
            g_slots[slot].vda_delete_pending = 1;
            ++g_slots[slot].vda_delete_attempts;
            g_slots[slot].vda_delete_after_ms = setup_now_ms() + 250;
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    if (result != 0)
        gp_log("slot[%d] VDA delete handle=%d failed 0x%08x; quarantined\n",
               slot, handle, (uint32_t)result);
    return result == 0 ? 0 : -1;
}

static void reap_pending_vdas(void)
{
    const uint64_t now = setup_now_ms();
    for (int slot = 0; slot < MAX_SLOTS; ++slot) {
        int32_t handle = -1;
        pthread_mutex_lock(&g_slot_lock);
        if (!g_slots[slot].usb_active &&
            g_slots[slot].vda_delete_pending &&
            g_slots[slot].handle >= 0 &&
            now >= g_slots[slot].vda_delete_after_ms)
            handle = g_slots[slot].handle;
        pthread_mutex_unlock(&g_slot_lock);
        if (handle < 0)
            continue;

        int result = scePadVirtualDeviceDeleteDevice(handle);
        unsigned attempts = 0;
        int released = 0;
        pthread_mutex_lock(&g_slot_lock);
        if (!g_slots[slot].usb_active &&
            g_slots[slot].vda_delete_pending &&
            g_slots[slot].handle == handle) {
            if (result == 0) {
                g_slots[slot].handle = -1;
                g_slots[slot].vda_delete_pending = 0;
                g_slots[slot].vda_delete_attempts = 0;
                g_slots[slot].vda_delete_after_ms = 0;
                released = 1;
            } else {
                attempts = ++g_slots[slot].vda_delete_attempts;
                g_slots[slot].vda_delete_after_ms = now + 500;
            }
        }
        pthread_mutex_unlock(&g_slot_lock);
        if (released) {
            gp_log("slot[%d] VDA delete retry released handle=%d\n",
                   slot, handle);
            gc_state_clear_slot(g_state, (size_t)slot);
        } else if (attempts == 2 || (attempts % 10) == 0) {
            gp_log("slot[%d] VDA delete still failing handle=%d result=0x%08x attempts=%u\n",
                   slot, handle, (uint32_t)result, attempts);
        }
    }
}

/* GhostPad is a network input source, not a second controller manager. It
 * claims one normal Ghostcontrol slot and therefore shares the proven VDA,
 * assignment, state, teardown, and four-controller limit. */
static int ghostpad_connected(void *context, const char *client)
{
    (void)context;
    int slot = -1;
    pthread_mutex_lock(&g_lifecycle_lock);
    if (!atomic_load(&g_service_enabled) || atomic_load(&g_shutdown) ||
        atomic_load(&g_assign_slot) >= 0)
        goto rejected;

    pthread_mutex_lock(&g_slot_lock);
    for (int index = 0; index < MAX_SLOTS; ++index) {
        if (g_slots[index].handle < 0 && !g_slots[index].usb_active &&
            !g_slots[index].dev_path[0] && !g_slots[index].retry_ready) {
            slot = index;
            reset_slot_record(&g_slots[slot]);
            snprintf(g_slots[slot].dev_path, sizeof(g_slots[slot].dev_path),
                     "ghostpad://lan");
            g_slots[slot].usb_active = 1;
            g_slots[slot].kind = CTRL_GHOSTPAD;
            break;
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    if (slot < 0)
        goto rejected;

    atomic_store(&g_assign_slot, slot);
    publish_slot_state(slot, "GhostPad Remote Controller", "native", 0, -1);
    notify("GhostPad connected from %s — choose a user", client);

    /* VDA creation waits for the klog reader to come online and for the
     * DEVICE_ADDED event. Never hold the lifecycle lock across that: it used to
     * stall every other lifecycle path for up to ten seconds, and a klog the
     * service could not get left the whole service wedged. */
    pthread_mutex_unlock(&g_lifecycle_lock);

    int32_t handle = create_vda_for_slot(slot);

    pthread_mutex_lock(&g_lifecycle_lock);
    if (handle < 0 || atomic_load(&g_shutdown)) {
        int expected = slot;
        (void)atomic_compare_exchange_strong(&g_assign_slot, &expected, -1);
        pthread_mutex_lock(&g_slot_lock);
        reset_slot_record(&g_slots[slot]);
        pthread_mutex_unlock(&g_slot_lock);
        gc_state_clear_slot(g_state, (size_t)slot);
        if (handle < 0)
            notify("GhostPad could not create a virtual controller");
        pthread_mutex_unlock(&g_lifecycle_lock);
        gp_log("GhostPad connection from %s rejected: VDA unavailable\n", client);
        return -1;
    }
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].handle = handle;
    g_slots[slot].vdi_ready = 1;
    pthread_mutex_unlock(&g_slot_lock);
    publish_slot_state(slot, NULL, NULL, 0, -1);
    gp_log("GhostPad client=%s slot[%d] handle=0x%x ready\n",
           client, slot, (uint32_t)handle);
    pthread_mutex_unlock(&g_lifecycle_lock);
    return slot;

rejected:
    pthread_mutex_unlock(&g_lifecycle_lock);
    if (slot < 0)
        gp_log("GhostPad connection from %s rejected: service, assignment, or slot unavailable\n",
               client);
    return -1;
}

static int ghostpad_input(void *context, int session,
                          const GcGhostpadInput *input)
{
    (void)context;
    if (!input || session < 0 || session >= MAX_SLOTS)
        return -1;
    pthread_mutex_lock(&g_slot_lock);
    int valid = g_slots[session].usb_active &&
                g_slots[session].kind == CTRL_GHOSTPAD;
    uint32_t before = g_slots[session].inject_count;
    pthread_mutex_unlock(&g_slot_lock);
    if (!valid)
        return -1;

    ScePadData pad;
    memset(&pad, 0, sizeof(pad));
    pad.buttons = input->buttons;
    pad.leftStick.x = input->lx;
    pad.leftStick.y = input->ly;
    pad.rightStick.x = input->rx;
    pad.rightStick.y = input->ry;
    pad.analogButtons.l2 = input->l2;
    pad.analogButtons.r2 = input->r2;
    pad.connected = 1;
    pad.quat.w = 1.0f;
    pthread_mutex_lock(&g_slot_lock);
    g_slots[session].last_input = pad;
    g_slots[session].last_input_valid = 1;
    pthread_mutex_unlock(&g_slot_lock);
    int result = inject_pad(session, &pad);
    if (result == 0 && before == 0)
        publish_slot_state(session, NULL, NULL, 1, -2);
    return result;
}

static void ghostpad_disconnected(void *context, int session)
{
    (void)context;
    if (session < 0 || session >= MAX_SLOTS)
        return;
    pthread_mutex_lock(&g_slot_lock);
    int valid = g_slots[session].usb_active &&
                g_slots[session].kind == CTRL_GHOSTPAD;
    int32_t handle = g_slots[session].handle;
    pthread_mutex_unlock(&g_slot_lock);
    if (!valid)
        return;

    ScePadData neutral;
    memset(&neutral, 0, sizeof(neutral));
    neutral.leftStick.x = neutral.leftStick.y = 128;
    neutral.rightStick.x = neutral.rightStick.y = 128;
    neutral.connected = 1;
    neutral.quat.w = 1.0f;
    (void)inject_pad(session, &neutral);

    int expected = session;
    (void)atomic_compare_exchange_strong(&g_assign_slot, &expected, -1);
    int released = release_vda_for_slot(session, handle) == 0;
    pthread_mutex_lock(&g_slot_lock);
    g_slots[session].usb_active = 0;
    if (released)
        reset_slot_record(&g_slots[session]);
    pthread_mutex_unlock(&g_slot_lock);
    if (released)
        gc_state_clear_slot(g_state, (size_t)session);
    else
        publish_slot_state(session, NULL, NULL, 0, -2);
    notify("GhostPad remote controller disconnected");
    gp_log("GhostPad slot[%d] disconnected cleanup=%s\n", session,
           released ? "complete" : "retrying");
}

static void fail_claimed_slot_launch(int slot, int32_t handle,
                                     const char *path, uint16_t vid,
                                     uint16_t pid, int retry_enabled,
                                     unsigned retry_delay_ms)
{
    int retry_scheduled;
    unsigned retry_attempt;
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].setup_reader_active = 0;
    g_slots[slot].hid_layout_valid = 0;
    memset(&g_slots[slot].hid_layout, 0,
           sizeof(g_slots[slot].hid_layout));
    retry_scheduled = schedule_slot_retry_locked(
        &g_slots[slot], retry_enabled, retry_delay_ms, path, vid, pid);
    retry_attempt = g_slots[slot].retry_count;
    pthread_mutex_unlock(&g_slot_lock);

    int setup_cancelled = 0;
    pthread_mutex_lock(&g_setup_lock);
    if (g_setup.active && !strcmp(g_setup.path, path)) {
        gp_log("setup reader launch failed: path=%s VID=%04x PID=%04x revision=%llu\n",
               path, vid, pid, (unsigned long long)g_setup.revision);
        gc_setup_cancel(&g_setup,
                        "Controller reader failed to start. Reconnect and begin setup again.");
        setup_cancelled = 1;
    }
    pthread_mutex_unlock(&g_setup_lock);
    if (setup_cancelled)
        ghostpad_setup_log_close("reader-launch-failed");

    int vda_released = release_vda_for_slot(slot, handle) == 0;
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].usb_active = 0;
    pthread_mutex_unlock(&g_slot_lock);
    int expected_slot = slot;
    (void)atomic_compare_exchange_strong(&g_assign_slot, &expected_slot, -1);
    if (vda_released)
        gc_state_clear_slot(g_state, (size_t)slot);
    else
        publish_slot_state(slot, NULL, NULL, 0, -2);
    if (retry_enabled)
        gp_log("manager: slot[%d] launch recovery %s attempt=%u/%u delay=%ums\n",
               slot, retry_scheduled ? "scheduled" : "exhausted",
               retry_attempt, GC_USB_RETRY_LIMIT, retry_delay_ms);
}

/* ── USB HID thread ───────────────────────────────────────────────────── */
/* Single-session: receives slot+path+vid+pid, runs until disconnect, then exits.
 * Manager thread handles re-detection after exit. */

typedef struct {
    int      slot;
    int      setup_only;
    char     dev_path[32];
    uint16_t vid, pid;
    controller_kind_t kind;
    uint8_t in_endpoint, out_endpoint;
    int interface_index, interface_number;
} usb_thread_arg_t;

static void refresh_profile(int slot, uint16_t vid, uint16_t pid,
                            controller_kind_t kind, GcProfile *profile,
                            uint64_t *generation)
{
    if (!g_config)
        return;
    uint64_t current = gc_config_generation(g_config);
    if (current == *generation)
        return;
    GcProfile selected;
    if (gc_config_select_profile(g_config, vid, pid,
                                 controller_kind_key(kind), &selected,
                                 &current) == 0) {
        *profile = selected;
        gp_log("profile: VID=0x%04x PID=0x%04x protocol=%s id=%s generation=%llu\n",
               vid, pid, controller_kind_key(kind), selected.id,
               (unsigned long long)current);
        publish_slot_state(slot, NULL, selected.id, -1, -2);
    } else {
        gc_profile_defaults(profile);
        gp_log("profile: no match for VID=0x%04x PID=0x%04x; identity fallback\n",
               vid, pid);
    }
    *generation = current;
}

static void *usb_hid_thread(void *arg) {
    usb_thread_arg_t *targ = (usb_thread_arg_t *)arg;
    int slot = targ->slot;
    int setup_only = targ->setup_only;
    char dev_path[32]; memcpy(dev_path, targ->dev_path, sizeof(dev_path));
    uint16_t vid = targ->vid, pid = targ->pid;
    controller_kind_t kind = targ->kind;
    uint8_t preferred_in = targ->in_endpoint;
    uint8_t preferred_out = targ->out_endpoint;
    int selected_interface = targ->interface_index;
    int selected_interface_number = targ->interface_number;
    free(targ);
    GcProfile active_profile;
    uint64_t profile_generation = UINT64_MAX;
    gc_profile_defaults(&active_profile);
    refresh_profile(slot, vid, pid, kind, &active_profile, &profile_generation);

    struct usb_fs_endpoint eps[2];
    struct usb_fs_init     init;
    struct usb_fs_open     fs_open;
    struct usb_fs_start    start;
    struct usb_fs_complete complete;
    struct usb_fs_stop     stop;
    struct usb_fs_close    fs_close;
    struct usb_fs_uninit   uninit;
    uint8_t  buf[1024];
    GcUsbDevice generic;
    GcHidState generic_state = {0};
    ScePadData generic_pad, generic_setup_pad;
    XboxControllerState xbox_state = {0};
    memset(&generic_pad, 0, sizeof(generic_pad));
    generic_pad.leftStick.x = generic_pad.leftStick.y = 128;
    generic_pad.rightStick.x = generic_pad.rightStick.y = 128;
    generic_pad.quat.w = 1.0f;
    generic_pad.connected = 1;
    memset(&generic_setup_pad, 0, sizeof(generic_setup_pad));
    generic_setup_pad.leftStick.x = generic_setup_pad.leftStick.y = 128;
    generic_setup_pad.rightStick.x = generic_setup_pad.rightStick.y = 128;
    generic_setup_pad.quat.w = 1.0f;
    generic_setup_pad.connected = 1;
    /* Multi-frame buffers — used by Logitech to queue N reads at once,
     * keeping the kernel in "actively polling" mode so single sparse
     * reports get buffered into available slots. */
    void    *buffers[8]; uint32_t lengths[8];
    int      n_frames = 1;
    int fd = -1, out_opened = 0;
    int xusb_setup_enable_pending = 0;
    int usb_ready_notified = 0;
    uint32_t in_mpkt = 64;  /* actual IN endpoint maxpkt — captured after open;
                              defaulted to 64 for branches that don't set it */

    gp_log("slot[%d] USB thread: %s VID=0x%04x PID=0x%04x protocol=%s iface=%d/%d IN=0x%02x OUT=0x%02x\n",
           slot, dev_path, vid, pid, controller_kind_name(kind),
           selected_interface, selected_interface_number,
           preferred_in, preferred_out);

    /* ── DS3 / SIXAXIS + PS3-protocol third-parties ───────────────────
     * Needs GET_REPORT(0xF2)+GET_REPORT(0xF5) on EP0 -- without these
     * two control transfers the controller never streams input reports.
     * Sony and HORI VIDs match DS4 too, so this branch MUST run before
     * the DS4 open block below. */
    if (kind == CTRL_HID) {
        if (gc_usb_inspect_matching(dev_path, GC_USB_INTERFACE_HID,
                selected_interface_number, &generic) != 0 || !generic.hid ||
            generic.vid != vid || generic.pid != pid) goto exit_slot;
        uint8_t endpoint = preferred_in ? preferred_in : generic.in[0];
        int available = 0;
        for (unsigned i = 0; i < generic.in_count; ++i)
            if (generic.in[i] == endpoint) available = 1;
        if (!available) goto exit_slot;
        fd = open(dev_path, O_RDWR);
        if (fd < 0) goto exit_slot;
        int interface_index = generic.interface_index;
        ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
        memset(eps, 0, sizeof(eps)); memset(&init, 0, sizeof(init));
        init.pEndpoints = eps; init.ep_index_max = 1;
        if (ioctl(fd, USB_FS_INIT, &init) != 0) { close(fd); goto exit_slot; }
        memset(&fs_open, 0, sizeof(fs_open));
        fs_open.ep_no = endpoint; fs_open.max_bufsize = sizeof(buf); fs_open.max_frames = 1;
        if (ioctl(fd, USB_FS_OPEN, &fs_open) != 0) goto uninit_exit;
        in_mpkt = fs_open.max_packet_length;
        if (!in_mpkt || in_mpkt > sizeof(buf)) goto reinit;
        if (generic.interface_number >= 0 && generic.interface_number <= 255) {
            int idle_result = gc_usb_hid_set_idle(
                fd, (uint8_t)generic.interface_number, 2);
            gp_log("slot[%d] DInput HID SET_IDLE iface=%d result=%d\n",
                   slot, generic.interface_number, idle_result);
        }
        buffers[0] = buf; lengths[0] = in_mpkt;
        eps[0].ppBuffer = buffers; eps[0].pLength = lengths; eps[0].nFrames = 1;
        eps[0].timeout = 50;
        eps[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
        goto main_loop;
    }
    if (kind == CTRL_DS3) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] DS3 open fail errno=%d\n", slot, errno); goto exit_slot; }

        if (selected_interface >= 0) {
            int interface_index = selected_interface;
            ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
        } else {
            int ii;
            for (ii = 0; ii < 4; ++ii) {
                int interface_index = ii;
                ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
            }
        }
        usleep(100000);

        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=1;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] DS3 FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        /* Operational handshake on EP0 -- both GET_REPORTs must succeed
         * before opening the interrupt IN, otherwise the device sits idle. */
        uint8_t control_interface = selected_interface_number >= 0
                                        ? (uint8_t)selected_interface_number : 0;
        if (ds3_set_operational_usb(fd, control_interface) != 0) {
            gp_log("slot[%d] DS3 set_operational failed\n", slot);
            goto uninit_exit;
        }

        memset(&fs_open,0,sizeof(fs_open));
        uint8_t in_ep = preferred_in ? preferred_in : DS3_EP_IN;
        fs_open.ep_index=0; fs_open.ep_no=in_ep;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] DS3 IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] DS3 IN ep=0x%02x maxpkt=%u\n",
               slot, in_ep, (unsigned)fs_open.max_packet_length);
        in_mpkt = fs_open.max_packet_length ? fs_open.max_packet_length : 64;

        n_frames = 1;
        buffers[0]=buf; lengths[0]=in_mpkt;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        out_opened = 0;
        goto main_loop;
    }

    /* ── DS4 / HORIPAD / XIM4 / DS4-protocol third-parties / Logitech ──
     * Identical open flow for all of them: detach driver, try IN 0x84 then
     * fall back to 0x81. Logitech and HORI/XIM both land on 0x81 with no
     * OUT endpoint. Dispatch in main_loop picks the right parser. */
    if (kind == CTRL_DS4 || kind == CTRL_LOGITECH) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] DS4 open fail errno=%d\n", slot, errno); goto exit_slot; }

        if (selected_interface >= 0) {
            int interface_index = selected_interface;
            ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
        } else {
            int ii;
            for (ii = 0; ii < 4; ++ii) {
                int interface_index = ii;
                ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
            }
        }
        usleep(100000);

        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] DS4 FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        /* Try DS4 IN ep 0x84 first (real Sony DS4); fall back to 0x81 (HORI). */
        memset(&fs_open,0,sizeof(fs_open));
        uint8_t in_ep = preferred_in ? preferred_in : DS4_EP_IN;
        fs_open.ep_index=0; fs_open.ep_no=in_ep;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        int in_ok = (ioctl(fd,USB_FS_OPEN,&fs_open)==0);
        if (!in_ok && !preferred_in) {
            memset(&fs_open,0,sizeof(fs_open));
            fs_open.ep_index=0; fs_open.ep_no=DS4_EP_IN_ALT;
            fs_open.max_bufsize=64; fs_open.max_frames=1;
            in_ok = (ioctl(fd,USB_FS_OPEN,&fs_open)==0);
            if (in_ok) gp_log("slot[%d] DS4 IN ep=0x81 (HORI/clone)\n", slot);
        } else {
            gp_log("slot[%d] DS4 IN ep=0x84 (Sony)\n", slot);
        }
        if (!in_ok) {
            gp_log("slot[%d] DS4 IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] DS4 IN maxpkt=%u\n", slot, (unsigned)fs_open.max_packet_length);
        in_mpkt = fs_open.max_packet_length ? fs_open.max_packet_length : 64;

        n_frames = 1;
        buffers[0]=buf; lengths[0]=in_mpkt;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        /* OUT endpoint is optional — XIM4 is input-only, no rumble feedback. */
        memset(&fs_open,0,sizeof(fs_open));
        uint8_t out_ep = preferred_out ? preferred_out : DS4_EP_OUT;
        fs_open.ep_index=1; fs_open.ep_no=out_ep;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        if (!out_opened && !preferred_out) {
            memset(&fs_open,0,sizeof(fs_open));
            fs_open.ep_index=1; fs_open.ep_no=DS4_EP_OUT_ALT;
            fs_open.max_bufsize=64; fs_open.max_frames=1;
            out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        }
        gp_log("slot[%d] DS4 OUT opened=%d\n", slot, out_opened);

        /* Generic HID gamepads (Logitech etc.) may need a HID class wake-up
         * before they start streaming reports. DS4 / HORI / XIM4 don't —
         * they stream on enumeration. */
        if (kind == CTRL_LOGITECH) {
            uint8_t control_interface = selected_interface_number >= 0
                                            ? (uint8_t)selected_interface_number : 0;
            if (logitech_wake_up(fd, control_interface) != 0) {
                gp_log("slot[%d] Logitech HID wake-up failed on iface=%u\n",
                       slot, (unsigned)control_interface);
                goto reinit;
            }
        }
        goto main_loop;
    }

    /* ── Xbox 360-style XUSB / Manba / 8BitDo Ultimate 2C ───────────── */
    if (kind == CTRL_XUSB) {
        uint8_t in_ep = MAMBA_XINPUT_EP_IN;
        uint8_t out_eps[2] = {MAMBA_XINPUT_EP_OUT, MAMBA_XINPUT_EP_OUT_ALT};
        int out_count = 2;

        if (vid == GC8BITDO_2C_XINPUT_VID && pid == GC8BITDO_2C_XINPUT_PID) {
            in_ep = GC8BITDO_2C_XINPUT_EP_IN;
            out_eps[0] = GC8BITDO_2C_XINPUT_EP_OUT;
            out_count = 1;
        }
        if (preferred_in) in_ep = preferred_in;
        if (preferred_out) { out_eps[0] = preferred_out; out_count = 1; }

        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] XUSB open fail errno=%d\n", slot, errno); goto exit_slot; }

        if (selected_interface >= 0) {
            int interface_index = selected_interface;
            errno = 0;
            int detach_result = ioctl(fd, USB_IFACE_DRIVER_DETACH,
                                      &interface_index);
            gp_log("slot[%d] XUSB detach iface_index=%d iface_number=%d result=%d errno=%d\n",
                   slot, selected_interface, selected_interface_number,
                   detach_result, detach_result == 0 ? 0 : errno);
        } else {
            int ii;
            for (ii = 0; ii < 4; ++ii) {
                int interface_index = ii;
                ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
            }
        }
        usleep(100000);
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] XUSB FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=in_ep;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] XUSB IN ep=0x%02x fail errno=%d\n",slot,in_ep,errno);
            goto uninit_exit;
        }
        in_mpkt = fs_open.max_packet_length ? fs_open.max_packet_length : 64;
        gp_log("slot[%d] XUSB IN ep=0x%02x maxpkt=%u\n",
               slot, in_ep, (unsigned)in_mpkt);

        n_frames = 1;
        buffers[0]=buf; lengths[0]=in_mpkt;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50;
        eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        for (int i=0; i<out_count && !out_opened; i++) {
                memset(&fs_open,0,sizeof(fs_open));
                fs_open.ep_index=1; fs_open.ep_no=out_eps[i];
                fs_open.max_bufsize=64; fs_open.max_frames=1;
                if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) {
                    out_opened=1;
                    gp_log("slot[%d] XUSB OUT ep=0x%02x\n",slot,out_eps[i]);
                }
            }
        if (out_opened) {
            if (gc_usb_xusb_output_allowed(setup_only)) {
                int led_result = mamba_xinput_send_enable(fd, eps);
                if (led_result != 0)
                    gp_log("slot[%d] XUSB enable failed result=%d; continuing input\n",
                           slot, led_result);
                if (!gc_usb_out_transfer_released(eps[1].ppBuffer,
                                                  eps[1].pLength))
                    goto reinit;
            } else {
                xusb_setup_enable_pending = 1;
                gp_log("slot[%d] XUSB setup: standard enable deferred until SETUP_BEGIN\n", slot);
            }
        }
        goto main_loop;
    }

    /* ── Xbox One: single-pass ─────────────────────────────────────────── */
    if (kind == CTRL_XBOX_GIP) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] Xbox open fail errno=%d\n", slot, errno); goto exit_slot; }

        if (selected_interface >= 0) {
            int interface_index = selected_interface;
            ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
        } else {
            int ii;
            for (ii = 0; ii < 4; ++ii) {
                int interface_index = ii;
                ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
            }
        }
        usleep(120000); /* hub settle: give USB hub time to propagate detach */
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] Xbox FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        memset(&fs_open,0,sizeof(fs_open));
        uint8_t in_ep = preferred_in ? preferred_in : XBOX_EP_IN;
        uint8_t out_ep = preferred_out ? preferred_out : XBOX_EP_OUT;
        fs_open.ep_index=0; fs_open.ep_no=in_ep;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] Xbox IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] Xbox IN ep=0x%02x ok maxpkt=%u\n",
               slot, in_ep, (unsigned)fs_open.max_packet_length);

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=out_ep;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        gp_log("slot[%d] Xbox OUT ep=0x%02x opened=%d\n", slot, out_ep, out_opened);

        xbox_state.out_available = (uint8_t)out_opened;
        if (out_opened) {
            int handshake = xbox_gip_handshake(
                fd, eps, vid, pid, &xbox_state,
                xbox_service_keep_running, NULL);
            gp_log("slot[%d] Xbox handshake result=%d\n", slot, handshake);
            if (handshake < 0) goto reinit;
        } else {
            gp_log("slot[%d] Xbox handshake skipped: OUT endpoint unavailable\n",
                   slot);
        }
        goto main_loop;
    }

    /* ── Nintendo: two-pass ────────────────────────────────────────────── */
    if (kind != CTRL_NINTENDO) {
        gp_log("slot[%d] unsupported protocol=%s\n", slot, controller_kind_name(kind));
        goto exit_slot;
    }
    fd = open(dev_path, O_RDWR);
    if (fd < 0) { gp_log("slot[%d] open fail errno=%d\n", slot, errno); goto exit_slot; }

    memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
    init.pEndpoints=eps; init.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&init)!=0){
        gp_log("slot[%d] Nintendo FS_INIT p1 fail\n",slot); close(fd); goto exit_slot;
    }
    if (selected_interface >= 0) {
        int interface_index = selected_interface;
        ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
    } else {
        int i0 = 0, i1 = 1;
        ioctl(fd, USB_IFACE_DRIVER_DETACH, &i0);
        ioctl(fd, USB_IFACE_DRIVER_DETACH, &i1);
    }
    memset(&fs_open,0,sizeof(fs_open));
    uint8_t nintendo_in = preferred_in ? preferred_in : 0x81;
    fs_open.ep_index=0; fs_open.ep_no=nintendo_in; fs_open.max_bufsize=64; fs_open.max_frames=1;
    if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
        gp_log("slot[%d] Nintendo IN p1 fail errno=%d\n",slot,errno); goto uninit_exit;
    }
    gp_log("slot[%d] Nintendo p1 IN ok maxpkt=%u\n",slot,(unsigned)fs_open.max_packet_length);
    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd); fd=-1;

    /* Pass 2: retry DETACH+OPEN up to 5 times to beat usb_hid0 re-attach
     * (real Switch Pro is claimed by PS5 native HID driver after probe releases it) */
    { int detach_try;
      for (detach_try = 0; detach_try < 5; detach_try++) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] reopen fail attempt %d\n",slot,detach_try); goto exit_slot; }
        if (selected_interface >= 0) {
            int interface_index = selected_interface;
            ioctl(fd, USB_IFACE_DRIVER_DETACH, &interface_index);
        } else {
            int i0 = 0, i1 = 1, i2 = 2;
            ioctl(fd, USB_IFACE_DRIVER_DETACH, &i0);
            ioctl(fd, USB_IFACE_DRIVER_DETACH, &i1);
            ioctl(fd, USB_IFACE_DRIVER_DETACH, &i2);
        }
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            close(fd); fd=-1; usleep(50000); continue;
        }
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=nintendo_in; fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) break; /* claimed it */
        memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
        close(fd); fd=-1;
        gp_log("slot[%d] Nintendo p2 OPEN retry %d errno=%d\n",slot,detach_try,errno);
        usleep(50000);
      }
      if (fd < 0) { gp_log("slot[%d] Nintendo p2 give up\n",slot); goto exit_slot; }
    }
    gp_log("slot[%d] Nintendo p2 IN ok maxpkt=%u\n",slot,(unsigned)fs_open.max_packet_length);
    if (fs_open.max_packet_length != 64){ gp_log("slot[%d] wrong maxpkt, reinit\n",slot); goto reinit; }

    buffers[0]=buf; lengths[0]=64;
    eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
    eps[0].timeout=200; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

    /* 8BitDo uses ep=0x02; real Nintendo Switch Pro Controller uses ep=0x01 */
    memset(&fs_open,0,sizeof(fs_open));
    uint8_t nintendo_out = preferred_out ? preferred_out : 0x02;
    fs_open.ep_index=1; fs_open.ep_no=nintendo_out; fs_open.max_bufsize=64; fs_open.max_frames=1;
    out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
    if (!out_opened && !preferred_out) {
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=0x01; fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        if (out_opened) gp_log("slot[%d] Nintendo OUT ep=0x01 (real Switch Pro)\n", slot);
    }
    gp_log("slot[%d] Nintendo OUT opened=%d\n", slot, out_opened);
    if (out_opened) {
        if (usb_send_cmd(fd,&eps[1],0x80,0x02) != 0) goto reinit;
        usleep(30000);
        if (usb_send_cmd(fd,&eps[1],0x80,0x04) != 0) goto reinit;
        usleep(50000);
        gp_log("slot[%d] Nintendo [80 02]+[80 04] sent\n", slot);
    }

main_loop: ;
    /* DS3 first: Sony and HORI VIDs match both DS3 and DS4 lists.
     * Without this ordering DS3 packets would be parsed by the DS4 layout
     * and produce garbage. */
    int is_ds3      = (kind == CTRL_DS3);
    int is_ds4      = (kind == CTRL_DS4);
    int is_logitech = (kind == CTRL_LOGITECH);
    int is_xbox     = (kind == CTRL_XBOX_GIP);
    int is_xusb     = (kind == CTRL_XUSB);
    int hs_state    = (kind == CTRL_NINTENDO) ? HS_WAIT_81_01 : HS_STREAMING;
    uint8_t nintendo_seq = 1;
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].usb_fd = fd;  /* register fd for clean teardown on SIGTERM */
    g_slots[slot].setup_reader_active = 1;
    if (kind == CTRL_HID) {
        g_slots[slot].hid_layout = generic.layout;
        g_slots[slot].hid_layout_valid = 1;
    } else {
        memset(&g_slots[slot].hid_layout, 0,
               sizeof(g_slots[slot].hid_layout));
        g_slots[slot].hid_layout_valid = 0;
    }
    pthread_mutex_unlock(&g_slot_lock);

    /* Cached pad state for sparse-report controllers (Logitech).
     * PS5 seems to need continuous state updates to "lock in" a button
     * state — a single inject_pad call gets dismissed. We re-inject the
     * cached state on either userspace or USB transfer timeouts to keep PS5
     * actively seeing the input. The decay is short enough that fast successive
     * presses aren't blocked by stale "pressed" state. */
    ScePadData cached_pad; memset(&cached_pad, 0, sizeof(cached_pad));
    cached_pad.quat.w = 1.0f;
    cached_pad.leftStick.x  = 128; cached_pad.leftStick.y  = 128;
    cached_pad.rightStick.x = 128; cached_pad.rightStick.y = 128;
    int cache_valid = 0;
    uint64_t last_cached_input_ms = 0;
    ScePadData setup_log_pad;
    memset(&setup_log_pad, 0, sizeof(setup_log_pad));
    int setup_log_pad_valid = 0;
    uint64_t setup_log_last_ms = 0;
    uint8_t setup_raw_log[GC_RAW_REPORT_MAX];
    memset(setup_raw_log, 0, sizeof(setup_raw_log));
    size_t setup_raw_log_len = 0;
    int setup_raw_log_valid = 0;
    uint64_t setup_raw_seq = 0;

    /* A USB_FS transfer can remain pending forever across a suspend/resume
     * boundary even though the ugen node still exists. Use the configured
     * health cadence for cheap descriptor liveness checks and recycle only a
     * genuinely stuck endpoint (no completion at all for several intervals).
     * Sparse/event-only HID devices complete with USB_ERR_TIMEOUT; those
     * completions prove the kernel path is alive and are not disconnects. */
    unsigned health_interval_ms = 1000;
    unsigned input_stale_release_ms = 400;
    GcRuntimeConfig health_runtime;
    if (g_config &&
        gc_config_snapshot(g_config, &health_runtime, NULL) == 0) {
        health_interval_ms = health_runtime.health_interval_ms;
        input_stale_release_ms = health_runtime.input_stale_release_ms;
    }
    uint64_t idle_recycle_ms = (uint64_t)health_interval_ms * 5u;
    if (idle_recycle_ms < 5000u) idle_recycle_ms = 5000u;
    uint64_t last_health_check_ms = setup_now_ms();
    uint64_t last_endpoint_progress_ms = last_health_check_ms;
    uint64_t last_protocol_packet_ms = last_health_check_ms;
    uint64_t protocol_idle_ms = (uint64_t)health_interval_ms * 10u;
    if (protocol_idle_ms < 15000u) protocol_idle_ms = 15000u;
    if (protocol_idle_ms > 60000u) protocol_idle_ms = 60000u;
    /* XUSB interrupt reports may be change-only. Keep the pending transfer
     * alive while an idle controller is waiting for Guided Setup input. */
    const int requires_periodic_input = gc_usb_idle_packet_watchdog(
        kind == CTRL_XBOX_GIP, kind == CTRL_NINTENDO);
    int in_pending = 0;

    while (atomic_load(&g_service_enabled) && !atomic_load(&g_shutdown)) {
        int no_data_handled = 0;
        if (is_xusb && setup_only && xusb_setup_enable_pending &&
            out_opened && !in_pending) {
            int setup_started = 0;
            pthread_mutex_lock(&g_setup_lock);
            setup_started = g_setup.active && !strcmp(g_setup.path, dev_path);
            pthread_mutex_unlock(&g_setup_lock);
            if (setup_started) {
                const int enable_result = mamba_xinput_send_enable(fd, eps);
                xusb_setup_enable_pending = 0; /* one bounded attempt */
                gp_log("slot[%d] XUSB setup standard enable result=%d\n",
                       slot, enable_result);
                if (!gc_usb_out_transfer_released(eps[1].ppBuffer,
                                                  eps[1].pLength))
                    goto reinit;
            }
        }
        if (!in_mpkt || in_mpkt > sizeof(buf) / (unsigned)n_frames) goto reinit;
        if (!in_pending) {
        memset(buf,0,sizeof(buf));
        for (int fi = 0; fi < n_frames; fi++) {
            buffers[fi] = buf + fi * in_mpkt;
            lengths[fi] = in_mpkt;
        }
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=n_frames;
        eps[0].aFrames=0; eps[0].status=0;

        /* Canonical FreeBSD ugen read pattern (see lib/libusb/libusb20_ugen20.c
         * ugen20_tr_submit + libusb20_dev_wait_process + ugen20_process):
         *   1. USB_FS_START
         *   2. poll(fd, ...) — wait for completion event
         *   3. USB_FS_COMPLETE — pick up the result
         * No USB_FS_STOP needed between successful transfers; STOP is only
         * for cancellation. Our old busy-poll + STOP approach was dropping
         * sparse events because of gaps between cycles. */
        memset(&start,0,sizeof(start)); start.ep_index=0;
        if (ioctl(fd,USB_FS_START,&start)!=0) {
            int e = errno;
            if (e == EBUSY) {
                /* Transfer still pending from prior iter — go straight to
                 * poll+complete and pick it up. */
            } else if (e == ENXIO || e == ENOTTY) {
                gp_log("slot[%d] START errno=%d — device gone\n", slot, e);
                goto reinit;
            } else {
                gp_log("slot[%d] START fatal errno=%d\n", slot, e);
                goto reinit;
            }
        }
        in_pending = 1;

        }

        {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, (int)eps[0].timeout);
            if (pr < 0) {
                if (errno == EINTR) continue;
                gp_log("slot[%d] poll fatal errno=%d\n", slot, errno);
                goto reinit;
            }
            if (pr == 0) {
                const uint64_t now_ms = setup_now_ms();
                if (now_ms - last_health_check_ms >= health_interval_ms) {
                    struct usb_device_descriptor live_desc;
                    memset(&live_desc, 0, sizeof(live_desc));
                    errno = 0;
                    if (ioctl(fd, USB_GET_DEVICE_DESC, &live_desc) != 0 ||
                        UGETW(live_desc.idVendor) != vid ||
                        UGETW(live_desc.idProduct) != pid) {
                        gp_log("slot[%d] USB health check failed errno=%d\n",
                               slot, errno);
                        goto reinit;
                    }
                    last_health_check_ms = now_ms;
                    /* Runtime changes take effect without reconnecting. */
                    if (g_config &&
                        gc_config_snapshot(g_config, &health_runtime, NULL) == 0) {
                        health_interval_ms = health_runtime.health_interval_ms;
                        input_stale_release_ms =
                            health_runtime.input_stale_release_ms;
                        idle_recycle_ms = (uint64_t)health_interval_ms * 5u;
                        if (idle_recycle_ms < 5000u) idle_recycle_ms = 5000u;
                        protocol_idle_ms =
                            (uint64_t)health_interval_ms * 10u;
                        if (protocol_idle_ms < 15000u)
                            protocol_idle_ms = 15000u;
                        if (protocol_idle_ms > 60000u)
                            protocol_idle_ms = 60000u;
                    }
                }
                /* No event in the timeout window. Re-inject cached state
                 * for Logitech to keep PS5 actively seeing the input
                 * (single-shot injects get dismissed by PS5).
                 *
                 * Decay strategy: clear only BUTTONS (so missed release
                 * events auto-recover) but HOLD the cached stick positions
                 * (otherwise sticks snap to center mid-motion = stuttering).
                 * Stick positions are corrected as soon as the next real
                 * report arrives, which is always-immediately because each
                 * physical stick movement generates a report. */
                if (handle_sparse_no_data(slot, kind, setup_only, dev_path,
                                          &cache_valid, last_cached_input_ms,
                                          input_stale_release_ms,
                                          &cached_pad) != 0) {
                    gp_log("slot[%d] cached VDI stream became stale\n", slot);
                    goto reinit;
                }
                no_data_handled = 1;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                gp_log("slot[%d] poll terminal revents=0x%x\n", slot,
                       (unsigned)pfd.revents);
                goto reinit;
            }
        }

        memset(&complete,0,sizeof(complete)); complete.ep_index=0;
        if (ioctl(fd,USB_FS_COMPLETE,&complete) != 0) {
            int e = errno;
            if (e == EINTR) continue;
            if (e == EBUSY) {
                const uint64_t now_ms = setup_now_ms();
                if (requires_periodic_input &&
                    now_ms - last_protocol_packet_ms >= protocol_idle_ms) {
                    gp_log("slot[%d] protocol silent for %llums; reinitializing\n",
                           slot, (unsigned long long)
                           (now_ms - last_protocol_packet_ms));
                    goto reinit;
                }
                if (now_ms - last_endpoint_progress_ms >= idle_recycle_ms) {
                    gp_log("slot[%d] IN stalled for %llums; recycling endpoint\n",
                           slot, (unsigned long long)
                           (now_ms - last_endpoint_progress_ms));
                    if (usb_stop_and_drain(fd, 0, 500) != 0) {
                        gp_log("slot[%d] IN recycle failed errno=%d\n", slot,
                               errno);
                        goto reinit;
                    }
                    in_pending = 0;
                    last_endpoint_progress_ms = setup_now_ms();
                }
                continue;
            }
            gp_log("slot[%d] COMPLETE fatal errno=%d\n", slot, e);
            goto reinit;
        }

        /* USB_FS_COMPLETE returns the oldest device-wide completion and
         * overwrites ep_index. Never interpret OUT metadata as an IN report. */
        if (complete.ep_index != 0) {
            gp_log("slot[%d] ignored completion for ep_index=%u\n", slot,
                   (unsigned)complete.ep_index);
            continue;
        }
        in_pending = 0;
        if (eps[0].status == USB_ERR_TIMEOUT) {
            /* Normal for sparse/event-only DirectInput controllers. */
            last_endpoint_progress_ms = setup_now_ms();
            if (!no_data_handled &&
                handle_sparse_no_data(slot, kind, setup_only, dev_path,
                                      &cache_valid, last_cached_input_ms,
                                      input_stale_release_ms,
                                      &cached_pad) != 0) {
                gp_log("slot[%d] cached VDI stream became stale\n", slot);
                goto reinit;
            }
            if (requires_periodic_input &&
                last_endpoint_progress_ms - last_protocol_packet_ms >=
                    protocol_idle_ms) {
                gp_log("slot[%d] protocol timed out for %llums; reinitializing\n",
                       slot, (unsigned long long)
                       (last_endpoint_progress_ms - last_protocol_packet_ms));
                goto reinit;
            }
            continue;
        }
        if (eps[0].status != USB_ERR_NORMAL_COMPLETION) {
            gp_log("slot[%d] IN completion status=%d\n", slot,
                   eps[0].status);
            goto reinit;
        }
        if (eps[0].aFrames != 1 || lengths[0] < 1 ||
            lengths[0] > in_mpkt || lengths[0] > sizeof(buf)) {
            gp_log("slot[%d] IN invalid completion frames=%u length=%u maxpkt=%u\n",
                   slot, (unsigned)eps[0].aFrames,
                   (unsigned)lengths[0], (unsigned)in_mpkt);
            goto reinit;
        }
        last_endpoint_progress_ms = setup_now_ms();
        last_protocol_packet_ms = last_endpoint_progress_ms;

        const uint8_t *parse_buf = (const uint8_t *)buffers[0];
        uint32_t len = lengths[0];
        uint8_t parse_copy[sizeof(buf)];

        /* XUSB pads can change state while profile/setup/VDI work runs. Keep
         * the interrupt pipe armed so the release following a short D-pad tap
         * cannot fall into that processing gap. The completed report must be
         * copied first because the next transfer reuses buf. */
        if (is_xusb) {
            memcpy(parse_copy, parse_buf, len);
            parse_buf = parse_copy;
            memset(buf, 0, sizeof(buf));
            for (int fi = 0; fi < n_frames; ++fi) {
                buffers[fi] = buf + fi * in_mpkt;
                lengths[fi] = in_mpkt;
            }
            eps[0].ppBuffer = buffers;
            eps[0].pLength = lengths;
            eps[0].nFrames = n_frames;
            eps[0].aFrames = 0;
            eps[0].status = 0;
            memset(&start, 0, sizeof(start));
            start.ep_index = 0;
            if (ioctl(fd, USB_FS_START, &start) != 0) {
                gp_log("slot[%d] XUSB pipeline START errno=%d\n", slot,
                       errno);
                goto reinit;
            }
            in_pending = 1;
        }

        ScePadData pad; memset(&pad,0,sizeof(pad)); pad.quat.w=1.0f;
        ScePadData setup_pad = pad;
        int injected = 0;

        if (kind == CTRL_HID) {
            int setup_parsed = gc_hid_input(&generic.layout, parse_buf, len,
                                            &generic_setup_pad);
            injected = gc_hid_input_profile(&generic.layout, parse_buf, len,
                                            &generic_state, &active_profile,
                                            &generic_pad);
            if (setup_parsed != injected)
                gp_log("slot[%d] HID setup/runtime parse mismatch raw=%d runtime=%d\n",
                       slot, setup_parsed, injected);
            pad = generic_pad;
            setup_pad = generic_setup_pad;
        } else if (is_ds3) {
            injected = ds3_handle_packet(fd, eps, parse_buf, len, &pad);
        } else if (is_ds4) {
            injected = ds4_handle_packet(fd, eps, parse_buf, len, &pad);
        } else if (is_logitech) {
            injected = logitech_handle_packet(fd, eps, parse_buf, len, &pad);
        } else if (is_xbox) {
            injected = xbox_handle_packet(fd, eps, parse_buf, len, &pad,
                                          &xbox_state);
        } else if (is_xusb) {
            injected = mamba_xinput_handle_packet(fd, eps, parse_buf, len, &pad);
        } else {
            injected = nintendo_handle_packet(fd, eps, parse_buf, len, &hs_state, &nintendo_seq, &pad);
        }
        if (kind != CTRL_HID)
            setup_pad = pad;

        if (injected < 0) {
            gp_log("slot[%d] protocol handler requested USB reinitialization\n",
                   slot);
            goto reinit;
        }

        const uint64_t packet_ms = setup_now_ms();
        const size_t raw_copy = len < GC_RAW_REPORT_MAX ? len : GC_RAW_REPORT_MAX;

        /* Unknown/spoofed XUSB devices can use the standard 00/14 framing
         * while placing controls in non-standard bytes. During active setup,
         * capture every DISTINCT raw report even when our semantic XInput
         * decoder sees no button/axis change. This makes Guided Setup logs a
         * complete hardware trace instead of filtering out the evidence needed
         * to teach a new packet layout. */
        if (kind == CTRL_XUSB) {
            int raw_setup_active = 0;
            int raw_target = -1, raw_step = 0, raw_total = 0;
            uint64_t raw_revision = 0;
            char raw_stage[24] = "review";
            pthread_mutex_lock(&g_setup_lock);
            raw_setup_active = g_setup.active && !strcmp(g_setup.path, dev_path);
            if (raw_setup_active) {
                const char *stage = "review";
                raw_target = g_setup.target;
                raw_revision = g_setup.revision;
                gc_setup_progress(&g_setup, &raw_step, &raw_total, &stage);
                snprintf(raw_stage, sizeof(raw_stage), "%s",
                         stage ? stage : "review");
            }
            pthread_mutex_unlock(&g_setup_lock);
            if (!raw_setup_active) {
                setup_raw_log_valid = 0;
                setup_raw_log_len = 0;
                setup_raw_seq = 0;
            } else if (!setup_raw_log_valid ||
                       gc_raw_report_changed(setup_raw_log, setup_raw_log_len,
                                             parse_buf, raw_copy)) {
                char raw_hex[193] = "";
                char raw_delta[512] = "";
                const size_t raw_diag_size = raw_copy < 96u ? raw_copy : 96u;
                (void)gc_ipc_hex_encode(parse_buf, raw_diag_size,
                                        raw_hex, sizeof(raw_hex));
                if (gc_raw_delta_format(setup_raw_log_valid ? setup_raw_log : NULL,
                                        setup_raw_log_valid ? setup_raw_log_len : 0,
                                        parse_buf, raw_copy,
                                        raw_delta, sizeof(raw_delta)) != 0)
                    snprintf(raw_delta, sizeof(raw_delta), "delta-too-large");
                ++setup_raw_seq;
                gp_log("setup raw: seq=%llu path=%s transport=xusb target=%d(%s) stage=%s step=%d/%d revision=%llu parse=%d decoded_buttons=0x%08x decoded_axes=[%u,%u,%u,%u,%u,%u] raw_len=%zu raw=%s delta=%s\n",
                       (unsigned long long)setup_raw_seq, dev_path,
                       raw_target, gc_setup_target_name(raw_target), raw_stage,
                       raw_step, raw_total, (unsigned long long)raw_revision,
                       injected, gc_setup_learning_buttons(&setup_pad),
                       setup_pad.leftStick.x, setup_pad.leftStick.y,
                       setup_pad.rightStick.x, setup_pad.rightStick.y,
                       setup_pad.analogButtons.l2, setup_pad.analogButtons.r2,
                       raw_copy, raw_hex, raw_delta);
                memcpy(setup_raw_log, parse_buf, raw_copy);
                setup_raw_log_len = raw_copy;
                setup_raw_log_valid = 1;
            }
        }

        uint32_t setup_buttons = injected > 0
            ? gc_setup_learning_buttons(&setup_pad) : 0;
        pthread_mutex_lock(&g_slot_lock);
        memcpy(g_slots[slot].last_raw_report, parse_buf, raw_copy);
        g_slots[slot].last_raw_length = (uint16_t)len;
        g_slots[slot].last_raw_copied = (uint16_t)raw_copy;
        g_slots[slot].last_raw_ms = packet_ms;
        ++g_slots[slot].raw_report_count;
        g_slots[slot].last_parse_result = injected;
        if (injected > 0) {
            g_slots[slot].last_input = setup_pad;
            g_slots[slot].last_setup_buttons = setup_buttons;
            g_slots[slot].last_input_valid = 1;
            ++g_slots[slot].parsed_report_count;
        }
        pthread_mutex_unlock(&g_slot_lock);

        if (injected > 0) {
            int setup_completed = 0;
            int log_setup_input = 0;
            int next_target = -1;
            int setup_step = 0, setup_total = 0;
            char setup_stage[24] = "review";
            uint64_t next_revision = 0;
            int setup_axes_changed = !setup_log_pad_valid ||
                abs((int)setup_pad.leftStick.x - (int)setup_log_pad.leftStick.x) >= 4 ||
                abs((int)setup_pad.leftStick.y - (int)setup_log_pad.leftStick.y) >= 4 ||
                abs((int)setup_pad.rightStick.x - (int)setup_log_pad.rightStick.x) >= 4 ||
                abs((int)setup_pad.rightStick.y - (int)setup_log_pad.rightStick.y) >= 4 ||
                abs((int)setup_pad.analogButtons.l2 - (int)setup_log_pad.analogButtons.l2) >= 4 ||
                abs((int)setup_pad.analogButtons.r2 - (int)setup_log_pad.analogButtons.r2) >= 4;
            pthread_mutex_lock(&g_setup_lock);
            uint64_t previous_revision = g_setup.revision;
            uint32_t previous_buttons = g_setup.observed_buttons;
            int previous_target = g_setup.target;
            int matching_setup = g_setup.active && !strcmp(g_setup.path, dev_path);
            if (matching_setup) {
                g_setup.input = setup_pad;
                g_setup.input_ms = packet_ms;
            }
            int suppress = kind == CTRL_HID
                ? gc_setup_feed_hid(&g_setup, dev_path, &generic.layout,
                                    parse_buf, len, &setup_pad, packet_ms)
                : (kind == CTRL_XUSB || kind == CTRL_LOGITECH)
                    ? gc_setup_feed_raw(&g_setup, dev_path, parse_buf, len,
                                        &setup_pad, packet_ms)
                    : gc_setup_feed_input(&g_setup, dev_path, &setup_pad,
                                          packet_ms);
            setup_completed = matching_setup && g_setup.completed &&
                g_setup.revision != previous_revision;
            log_setup_input = matching_setup &&
                (setup_buttons != previous_buttons ||
                 g_setup.revision != previous_revision ||
                 (setup_axes_changed && packet_ms - setup_log_last_ms >= 100u));
            next_target = g_setup.target;
            next_revision = g_setup.revision;
            const char *stage = "review";
            gc_setup_progress(&g_setup, &setup_step, &setup_total, &stage);
            snprintf(setup_stage, sizeof(setup_stage), "%s", stage ? stage : "review");
            pthread_mutex_unlock(&g_setup_lock);
            if (log_setup_input) {
                char raw_hex[193] = "";
                size_t raw_diag_size = len < 96u ? len : 96u;
                (void)gc_ipc_hex_encode(parse_buf, raw_diag_size,
                                        raw_hex, sizeof(raw_hex));
                gp_log("setup input: path=%s transport=%s target=%d(%s)->%d(%s) stage=%s step=%d/%d buttons=0x%08x previous=0x%08x rising=0x%08x axes=[%u,%u,%u,%u,%u,%u] raw_len=%zu raw_prefix=%s revision=%llu->%llu\n",
                       dev_path, controller_kind_key(kind),
                       previous_target, gc_setup_target_name(previous_target),
                       next_target, gc_setup_target_name(next_target),
                       setup_stage, setup_step, setup_total,
                       setup_buttons, previous_buttons,
                       setup_buttons & ~previous_buttons,
                       setup_pad.leftStick.x, setup_pad.leftStick.y,
                       setup_pad.rightStick.x, setup_pad.rightStick.y,
                       setup_pad.analogButtons.l2, setup_pad.analogButtons.r2,
                       len, raw_hex,
                       (unsigned long long)previous_revision,
                       (unsigned long long)next_revision);
                setup_log_pad = setup_pad;
                setup_log_pad_valid = 1;
                setup_log_last_ms = packet_ms;
            }
            if (setup_completed)
                notify("Guided setup complete. Review and save.");
            refresh_profile(slot, vid, pid, kind, &active_profile,
                            &profile_generation);
            gc_profile_apply(&active_profile, &pad);
            gc_rawmap_apply(&active_profile, parse_buf, len, &pad);
            /* HID-only source bits are never native PlayStation output buttons. */
            if (kind == CTRL_HID) pad.buttons &= ~0x002e0000u;
            if (suppress) {
                pad.buttons = 0;
                pad.leftStick.x = pad.leftStick.y = 128;
                pad.rightStick.x = pad.rightStick.y = 128;
                pad.analogButtons.l2 = pad.analogButtons.r2 = 0;
            }
            if (!usb_ready_notified) {
                if (!setup_only)
                    notify("Slot %d input ready — choose a user", slot + 1);
                usb_ready_notified = 1;
                publish_slot_state(slot, NULL,
                    setup_only ? "setup-only" : active_profile.id,
                    1, setup_only ? 0 : -2);
            }
            /* Update the cache for sparse-report controllers and reset
             * the decay counter — we just got fresh data. */
            if (is_logitech || kind == CTRL_HID || kind == CTRL_XUSB) {
                memcpy(&cached_pad, &pad, sizeof(cached_pad));
                cache_valid = 1;
                last_cached_input_ms = packet_ms;
            }
            if (!setup_only && inject_pad(slot, &pad) != 0) {
                gp_log("slot[%d] VDI stream became stale\n", slot);
                goto reinit;
            }
        }

        /* Explicit stop after each successful read for Logitech — keeps
         * the kernel in a clean state for the next USB_FS_START. Without
         * this, the next start returns EBUSY and we lose ~5ms to a stop+
         * sleep recovery, dropping reports (notably button releases). */
        if (is_logitech) {
            memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
        }
    }

reinit:
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].usb_fd = -1;  /* unregister before teardown */
    pthread_mutex_unlock(&g_slot_lock);
    if (usb_ready_notified) { notify("Slot %d controller disconnected", slot + 1); usb_ready_notified=0; }
    memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
    if (out_opened) {
        memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=1; ioctl(fd,USB_FS_CLOSE,&fs_close);
        out_opened=0;
    }
    memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=0; ioctl(fd,USB_FS_CLOSE,&fs_close);
uninit_exit:
    memset(&uninit,0,sizeof(uninit));
    int uninit_result = ioctl(fd,USB_FS_UNINIT,&uninit);
    if (uninit_result == 0 &&
        (kind == CTRL_NINTENDO || kind == CTRL_XUSB))
        usb_out_release_after_uninit(&eps[1]);
    close(fd); fd=-1;

exit_slot:
    gp_log("slot[%d] USB thread exiting — freeing slot\n", slot);
    pthread_mutex_lock(&g_slot_lock);
    int32_t handle = g_slots[slot].handle;
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].setup_reader_active = 0;
    g_slots[slot].hid_layout_valid = 0;
    memset(&g_slots[slot].hid_layout, 0,
           sizeof(g_slots[slot].hid_layout));
    pthread_mutex_unlock(&g_slot_lock);
    /* SETUP_BEGIN holds setup_lock while checking setup_reader_active under
     * slot_lock. It therefore either sees a live reader, after which this
     * cancellation wins, or observes inactive and rejects the stale slot. */
    int disconnected_setup = 0;
    pthread_mutex_lock(&g_setup_lock);
    if (g_setup.active && !strcmp(g_setup.path, dev_path)) {
        gp_log("setup disconnect: path=%s VID=%04x PID=%04x target=%d(%s) revision=%llu\n",
               dev_path, vid, pid, g_setup.target,
               gc_setup_target_name(g_setup.target),
               (unsigned long long)g_setup.revision);
        gc_setup_cancel(&g_setup,
                        "Controller disconnected. Reconnect and begin setup again.");
        disconnected_setup = 1;
    }
    pthread_mutex_unlock(&g_setup_lock);
    if (disconnected_setup)
        ghostpad_setup_log_close("controller-disconnected");
    int retry_enabled = 0;
    unsigned retry_delay_ms = 3000;
    GcRuntimeConfig retry_runtime;
    if (atomic_load(&g_service_enabled) &&
        !atomic_load(&g_shutdown) && g_config &&
        gc_config_snapshot(g_config, &retry_runtime, NULL) == 0) {
        retry_enabled = retry_runtime.auto_restart;
        retry_delay_ms = retry_runtime.disconnect_restart_ms;
    }
    int retry_scheduled = 0;
    unsigned retry_attempt = 0;
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].usb_fd    = -1;
    (void)schedule_slot_retry_locked(&g_slots[slot], retry_enabled,
                                     retry_delay_ms, dev_path, vid, pid);
    retry_scheduled = g_slots[slot].retry_pending;
    retry_attempt = g_slots[slot].retry_count;
    /* Every recognized worker gets bounded same-path recovery, including an
     * initial OPEN/handshake failure. Physical unplug clears the budget. */
    pthread_mutex_unlock(&g_slot_lock);
    int vda_released = release_vda_for_slot(slot, handle) == 0;
    if (retry_enabled)
        gp_log("slot[%d] recovery %s attempt=%u/%u delay=%ums\n", slot,
               retry_scheduled ? "scheduled" : "exhausted",
               retry_attempt, GC_USB_RETRY_LIMIT,
               retry_delay_ms);
    /* usb_active is the worker-lifetime barrier used by service_stop().
     * Keep it asserted until every resource and published-state cleanup is
     * complete, then make this worker invisible as the final operation. */
    int expected_slot = slot;
    (void)atomic_compare_exchange_strong(&g_assign_slot, &expected_slot, -1);
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].usb_active = 0;
    pthread_mutex_unlock(&g_slot_lock);
    if (vda_released) {
        gc_state_clear_slot(g_state, (size_t)slot);
    } else {
        publish_slot_state(slot, NULL, NULL, 0, -2);
        gc_state_note(g_state,
            "Virtual controller cleanup is retrying before reconnect");
    }
    return NULL;
}

/* ── Controller manager thread ────────────────────────────────────────── */
static void *controller_manager_thread(void *arg) {
    (void)arg;
    int scan = 0;
    gp_log("Manager thread started (MAX_SLOTS=%d)\n", MAX_SLOTS);

    while (!atomic_load(&g_shutdown)) {
        reap_pending_vdas();
        if (!atomic_load(&g_service_enabled)) {
            usleep(100000);
            continue;
        }

        int awaiting_slot = atomic_load(&g_assign_slot);
        if (awaiting_slot >= 0 && awaiting_slot < MAX_SLOTS) {
            pthread_mutex_lock(&g_slot_lock);
            int32_t awaiting_handle = g_slots[awaiting_slot].handle;
            pthread_mutex_unlock(&g_slot_lock);
            int32_t assigned_user = vda_assigned_user((uint32_t)awaiting_handle);
            if (awaiting_handle > 0 && assigned_user > 0) {
                pthread_mutex_lock(&g_slot_lock);
                g_slots[awaiting_slot].confirmed = 1;
                pthread_mutex_unlock(&g_slot_lock);
                int expected_slot = awaiting_slot;
                if (atomic_compare_exchange_strong(&g_assign_slot,
                                                   &expected_slot, -1)) {
                    publish_slot_state(awaiting_slot, NULL, NULL, -1,
                                       assigned_user);
                    gp_log("slot[%d] manual assignment confirmed user=0x%08x\n",
                           awaiting_slot, (uint32_t)assigned_user);
                    notify("Slot %d assigned — controller active",
                           awaiting_slot + 1);
                }
            }
        }

        /* Setup deadlines must advance even when the selected controller is
         * completely idle and ShellUI is not polling SETUP_INPUT. */
        pthread_mutex_lock(&g_setup_lock);
        if (g_setup.active && g_setup.target >= 0) {
            const int timer_target = g_setup.target;
            const int timer_guided = g_setup.guided;
            const uint64_t timer_revision = g_setup.revision;
            const uint64_t timer_now_ms = setup_now_ms();
            gc_setup_tick(&g_setup, timer_now_ms);
            if (g_setup.revision != timer_revision &&
                g_setup.target != timer_target)
                gp_log("setup timer: target=%d(%s)->%d(%s) guided=%d revision=%llu->%llu message=%s\n",
                       timer_target, gc_setup_target_name(timer_target),
                       g_setup.target, gc_setup_target_name(g_setup.target),
                       timer_guided,
                       (unsigned long long)timer_revision,
                       (unsigned long long)g_setup.revision, g_setup.message);
        }
        pthread_mutex_unlock(&g_setup_lock);

        /* Snapshot every /dev/ugen*.* this scan pass — beats a hardcoded
         * list when the XIM/controller lands on an unexpected path
         * (ugen2.10+, ugen3.*, etc). Skip root hub (.1) entries. */
        char ugen_paths[32][32];
        int n_paths = 0;
        DIR *dp = opendir("/dev");
        if (dp) {
            struct dirent *ent;
            while (n_paths < 32 && (ent = readdir(dp)) != NULL) {
                if (strncmp(ent->d_name, "ugen", 4) != 0) continue;
                const char *dot = strchr(ent->d_name, '.');
                if (!dot || dot[1] == '\0') continue;          /* not "ugenX.Y" */
                if (strcmp(dot, ".1") == 0) continue;          /* root hub */
                snprintf(ugen_paths[n_paths], sizeof(ugen_paths[0]),
                         "/dev/%s", ent->d_name);
                n_paths++;
            }
            closedir(dp);
        }
        if ((scan % 5) == 0) gp_log("manager: scan #%d — %d ugen paths\n", scan, n_paths);

        /* Initial failures stay quarantined to prevent VDA storms. A reader
         * that had streamed may release the same path after the configured
         * delay, with a hard retry cap. Physical unplug resets the cap. */
        int cleared_slots[MAX_SLOTS] = {0};
        const uint64_t manager_now_ms = setup_now_ms();
        pthread_mutex_lock(&g_slot_lock);
        for (int s = 0; s < MAX_SLOTS; ++s) {
            if (!g_slots[s].usb_active && g_slots[s].handle < 0) {
                const char *identity = g_slots[s].dev_path[0] ?
                    g_slots[s].dev_path : g_slots[s].retry_path;
                if (identity[0] &&
                    !gc_usb_path_present(ugen_paths, n_paths, identity)) {
                    reset_slot_record(&g_slots[s]);
                    cleared_slots[s] = 1;
                } else if (g_slots[s].dev_path[0] &&
                           gc_usb_retry_due(g_slots[s].retry_pending,
                                            manager_now_ms,
                                            g_slots[s].retry_after_ms)) {
                    gp_log("manager: releasing slot[%d] for bounded recovery attempt %u/%u\n",
                           s, g_slots[s].retry_count, GC_USB_RETRY_LIMIT);
                    g_slots[s].dev_path[0] = '\0';
                    g_slots[s].retry_pending = 0;
                    g_slots[s].retry_ready = 1;
                }
            }
        }
        pthread_mutex_unlock(&g_slot_lock);
        for (int s = 0; s < MAX_SLOTS; ++s)
            if (cleared_slots[s]) gc_state_clear_slot(g_state, (size_t)s);

        for (int i = 0; i < n_paths; i++) {
            const char *path = ugen_paths[i];

            if (!atomic_load(&g_service_enabled) || atomic_load(&g_shutdown))
                break;

            /* Serialize assignment: if a controller is still awaiting the user's
             * button press to confirm its dialog, do not start another one. */
            if (atomic_load(&g_assign_slot) >= 0) break;

            /* Skip if already claimed by an active slot */
            int busy = 0;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].dev_path[0] && strcmp(g_slots[s].dev_path, path) == 0) {
                    busy = 1; break;
                }
            }
            pthread_mutex_unlock(&g_slot_lock);
            if (busy) continue;

            /* Try to identify controller */
            uint16_t vid=0, pid=0;
            controller_kind_t kind=CTRL_NONE;
            GcControllerRule rule;
            GcUsbDevice usb;
            memset(&rule, 0, sizeof(rule));
            if (!probe_one_path(path, &vid, &pid, &kind, &rule, &usb)) continue;

            /* Find free slot */
            int slot = -1;
            int retry_claim = 0;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].handle < 0 && !g_slots[s].usb_active &&
                    g_slots[s].retry_ready &&
                    !strcmp(g_slots[s].retry_path, path)) {
                    if (g_slots[s].retry_vid == vid &&
                        g_slots[s].retry_pid == pid) {
                        slot = s;
                        retry_claim = 1;
                        break;
                    }
                    /* The path was reused by a different physical device
                     * before an absent scan was observed. Do not transfer the
                     * old controller's retry budget to it. */
                    reset_slot_record(&g_slots[s]);
                }
            }
            if (slot < 0) for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].handle < 0 && !g_slots[s].usb_active &&
                    !g_slots[s].dev_path[0] &&
                    !g_slots[s].retry_ready) { slot=s; break; }
            }
            pthread_mutex_unlock(&g_slot_lock);

            if (slot < 0) {
                if ((scan % 5) == 0) gp_log("manager: all %d slots full\n", MAX_SLOTS);
                continue;
            }

            GcProfile admission_profile;
            int has_exact_profile = g_config &&
                gc_config_find_exact_profile(g_config, vid, pid,
                    controller_kind_key(kind), &admission_profile, NULL) == 0;
            /* A community profile may have been copied in after startup.
             * Reload once on first admission miss so reconnecting the pad is
             * enough to install it; no rebuilt payload or controller rule is
             * required. */
            if (!has_exact_profile && g_config && !rule.id[0]) {
                char reload_error[256] = "";
                if (gc_config_reload(g_config, reload_error,
                                     sizeof(reload_error)) == 0) {
                    uint64_t generation = gc_config_generation(g_config);
                    gc_state_set_generation(g_state, generation);
                    has_exact_profile = gc_config_find_exact_profile(
                        g_config, vid, pid, controller_kind_key(kind),
                        &admission_profile, NULL) == 0;
                    gp_log("manager: reloaded profile files for VID=0x%04x PID=0x%04x generation=%llu match=%s\n",
                           vid, pid, (unsigned long long)generation,
                           has_exact_profile ? admission_profile.id : "none");
                } else {
                    gp_log("manager: profile reload failed for VID=0x%04x PID=0x%04x: %s\n",
                           vid, pid, *reload_error ? reload_error : "unknown error");
                }
            }
            /* An exact-match profile is portable controller configuration;
             * a rule only overrides USB interface details when required. */
            /* A catch-all rule (the seeded default) is a template, not an
             * admission: treating it as configuration mirrored every unknown
             * pad into a virtual pad, which raised the console's
             * user-assignment dialog and took the reports away from the setup
             * learner. Unknown pads stay setup-only until a profile is saved. */
            const int catch_all_rule = rule.match_any_vid && rule.match_any_pid;
            const int configured =
                (!catch_all_rule && rule.id[0] != '\0') || has_exact_profile;
            const char *name = rule.id[0] ? rule.name :
                has_exact_profile ? admission_profile.name :
                controller_display_name(kind, vid, pid);
            /* A source that is being configured must not be mirrored: the
             * virtual pad would raise the console's user-assignment dialog and
             * consume the very reports the learner needs. Keep it setup-only
             * until a profile for it has actually been saved. */
            const int under_setup = g_setup.active && !strcmp(g_setup.path, path);
            const int setup_only = gc_usb_setup_only(configured) || under_setup;
            const uint8_t selected_in = rule.preferred_in_endpoint ?
                rule.preferred_in_endpoint : (usb.in_count ? usb.in[0] : 0);
            const uint8_t selected_out = rule.preferred_out_endpoint ?
                rule.preferred_out_endpoint : (usb.out_count ? usb.out[0] : 0);

            /* Linearization point for a controller launch. service_stop()
             * holds this same lock while disabling the service and waiting,
             * so a probe that raced STOP can never claim a slot, create a
             * VDA, or launch a worker after the stop transition. */
            pthread_mutex_lock(&g_lifecycle_lock);
            if (!atomic_load(&g_service_enabled) ||
                atomic_load(&g_shutdown) ||
                atomic_load(&g_assign_slot) >= 0) {
                pthread_mutex_unlock(&g_lifecycle_lock);
                break;
            }

            /* Revalidate both the selected slot and path under the slot lock;
             * either may have changed while USB descriptors/config were read. */
            int claim_available = 1;
            pthread_mutex_lock(&g_slot_lock);
            if (g_slots[slot].handle >= 0 || g_slots[slot].usb_active ||
                g_slots[slot].dev_path[0] ||
                (retry_claim &&
                 (!g_slots[slot].retry_ready ||
                  strcmp(g_slots[slot].retry_path, path) ||
                  g_slots[slot].retry_vid != vid ||
                  g_slots[slot].retry_pid != pid)) ||
                (!retry_claim && g_slots[slot].retry_ready)) {
                claim_available = 0;
            }
            for (int s = 0; claim_available && s < MAX_SLOTS; ++s) {
                if (g_slots[s].dev_path[0] &&
                    strcmp(g_slots[s].dev_path, path) == 0)
                    claim_available = 0;
            }
            if (claim_available) {
                if (!retry_claim) {
                    g_slots[slot].retry_count = 0;
                    g_slots[slot].retry_path[0] = '\0';
                    g_slots[slot].retry_vid = 0;
                    g_slots[slot].retry_pid = 0;
                }
                g_slots[slot].retry_pending = 0;
                g_slots[slot].retry_ready = 0;
                snprintf(g_slots[slot].dev_path,
                         sizeof(g_slots[slot].dev_path), "%s", path);
                g_slots[slot].usb_active = 1; /* worker-lifetime claim */
                g_slots[slot].setup_reader_active = 0;
                g_slots[slot].hid_layout_valid = 0;
                memset(&g_slots[slot].hid_layout, 0,
                       sizeof(g_slots[slot].hid_layout));
                g_slots[slot].confirmed  = setup_only;
                g_slots[slot].vid = vid;
                g_slots[slot].pid = pid;
                g_slots[slot].kind = kind;
                g_slots[slot].last_input_valid = 0;
                g_slots[slot].last_setup_buttons = 0;
                g_slots[slot].last_raw_length = 0;
                g_slots[slot].last_raw_copied = 0;
                g_slots[slot].last_raw_ms = 0;
                g_slots[slot].raw_report_count = 0;
                g_slots[slot].parsed_report_count = 0;
                g_slots[slot].last_parse_result = 0;
                g_slots[slot].input_endpoint = selected_in;
                g_slots[slot].output_endpoint = selected_out;
                g_slots[slot].interface_index = usb.interface_index;
                g_slots[slot].interface_number = usb.interface_number;
            }
            pthread_mutex_unlock(&g_slot_lock);
            if (!claim_available) {
                pthread_mutex_unlock(&g_lifecycle_lock);
                continue;
            }
            atomic_store(&g_assign_slot, setup_only ? -1 : slot);

            gp_log("manager: %s VID=0x%04x PID=0x%04x protocol=%s at %s → slot[%d]\n",
                   name, vid, pid, controller_kind_name(kind), path, slot);
            if (setup_only)
                notify("%s [%04x:%04x] detected — open Custom Controller Setup",
                       name, vid, pid);
            else
                notify("%s [%04x:%04x] detected — assign a user on screen",
                       name, vid, pid);

            publish_slot_state(slot, name, "", 0, -1);

            int launch_retry_enabled = 0;
            unsigned launch_retry_delay_ms = 3000;
            GcRuntimeConfig launch_runtime;
            if (g_config &&
                gc_config_snapshot(g_config, &launch_runtime, NULL) == 0) {
                launch_retry_enabled = launch_runtime.auto_restart;
                launch_retry_delay_ms = launch_runtime.disconnect_restart_ms;
            }

            /* Create VDA (shows the PS5 assignment dialog). */
            int32_t handle = setup_only ? -1 : create_vda_for_slot(slot);
            if (setup_only)
                gp_log("manager: slot[%d] unconfigured controller setup-only; VDA deferred\n", slot);
            if (handle < 0 && !setup_only) {
                gp_log("manager: slot[%d] VDA creation failed\n", slot);
                fail_claimed_slot_launch(slot, -1, path, vid, pid,
                                         launch_retry_enabled &&
                                         (uint32_t)handle != 0x803b0006u,
                                         launch_retry_delay_ms);
                pthread_mutex_unlock(&g_lifecycle_lock);
                continue;
            }

            pthread_mutex_lock(&g_slot_lock);
            g_slots[slot].handle      = handle;
            g_slots[slot].vdi_ready   = !setup_only;
            g_slots[slot].inject_count = 0;
            g_slots[slot].vdi_error_count = 0;
            g_slots[slot].vda_delete_pending = 0;
            g_slots[slot].vda_delete_attempts = 0;
            g_slots[slot].vda_delete_after_ms = 0;
            pthread_mutex_unlock(&g_slot_lock);
            publish_slot_state(slot, NULL, NULL, 0, -1);

            /* Launch USB reader thread */
            usb_thread_arg_t *targ = malloc(sizeof(*targ));
            if (!targ) {
                gp_log("manager: malloc fail for slot[%d]\n", slot);
                fail_claimed_slot_launch(slot, handle, path, vid, pid,
                                         launch_retry_enabled,
                                         launch_retry_delay_ms);
                pthread_mutex_unlock(&g_lifecycle_lock);
                continue;
            }
            memset(targ, 0, sizeof(*targ));
            targ->slot = slot;
            targ->setup_only = setup_only;
            snprintf(targ->dev_path, sizeof(targ->dev_path), "%s", path);
            targ->vid=vid; targ->pid=pid; targ->kind=kind;
            targ->in_endpoint=selected_in;
            targ->out_endpoint=selected_out;
            targ->interface_index=usb.interface_index;
            targ->interface_number=usb.interface_number;

            pthread_t tid;
            if (pthread_create(&tid, NULL, usb_hid_thread, targ) != 0) {
                gp_log("manager: pthread_create fail slot[%d]\n", slot);
                free(targ);
                fail_claimed_slot_launch(slot, handle, path, vid, pid,
                                         launch_retry_enabled,
                                         launch_retry_delay_ms);
            } else {
                pthread_detach(tid);
                gp_log("manager: slot[%d] USB thread started handle=0x%x\n",
                       slot, (uint32_t)handle);
            }
            pthread_mutex_unlock(&g_lifecycle_lock);
            /* One controller per scan pass. The PS5 owner-change event releases
             * the gate only after the user selects an account on screen. */
            break;
        }

        scan++;
        if ((scan % 5) == 0) {
            /* Log active slots every 10s */
            int active = 0;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++)
                if (g_slots[s].usb_active) active++;
            pthread_mutex_unlock(&g_slot_lock);
            if (active == 0 && (scan % 10) == 0)
                gp_log("manager: scan #%d — no controllers\n", scan);
        }
        usleep(2000000);
    }
    return NULL;
}

/* ── Credential elevation ─────────────────────────────────────────────── */
static void elevate_credentials(void) {
    pid_t p = getpid();
    uint8_t caps[16]; memset(caps,0xff,sizeof(caps));
    kernel_set_ucred_authid(p, 0x3800000000010003l);
    kernel_set_ucred_caps(p, caps);
}

/* ── Service lifecycle + local IPC ───────────────────────────────────── */
static int active_slot_count(void)
{
    int active = 0;
    pthread_mutex_lock(&g_slot_lock);
    for (int slot = 0; slot < MAX_SLOTS; ++slot)
        active += g_slots[slot].usb_active != 0 ||
                  g_slots[slot].vda_delete_pending != 0;
    pthread_mutex_unlock(&g_slot_lock);
    return active;
}

static void clear_inactive_slot_claims(void)
{
    int cleared[MAX_SLOTS] = {0};
    pthread_mutex_lock(&g_slot_lock);
    for (int slot = 0; slot < MAX_SLOTS; ++slot) {
        if (!g_slots[slot].usb_active && g_slots[slot].handle < 0) {
            reset_slot_record(&g_slots[slot]);
            cleared[slot] = 1;
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    for (int slot = 0; slot < MAX_SLOTS; ++slot)
        if (cleared[slot]) gc_state_clear_slot(g_state, (size_t)slot);
}

static int service_start(void)
{
    pthread_mutex_lock(&g_lifecycle_lock);
    if (atomic_load(&g_service_enabled)) {
        pthread_mutex_unlock(&g_lifecycle_lock);
        return 0;
    }
    if (active_slot_count() != 0) {
        gc_state_set_service(g_state, GC_SERVICE_ERROR,
                             "previous controller workers are still stopping");
        gp_log("service: start refused while controller workers are stopping\n");
        pthread_mutex_unlock(&g_lifecycle_lock);
        return -1;
    }
    /* A VDA delete may have completed asynchronously after STOP timed out.
     * Drop its now-inactive path claim before enabling scans, otherwise a
     * still-plugged controller can remain falsely busy until physical unplug. */
    clear_inactive_slot_claims();
    atomic_store(&g_service_enabled, 1);
    gc_state_set_service(g_state, GC_SERVICE_STARTING, NULL);
    if (gc_ds4_bridge_start(g_config, g_state))
        gc_state_note(g_state, "Wireless DS4 worker could not start");
    GcRuntimeConfig runtime;
    if (gc_config_snapshot(g_config, &runtime, NULL) == 0) {
        gc_ghostpad_configure(g_ghostpad, runtime.ghostpad_port,
                              runtime.ghostpad_stale_ms);
        gc_ghostpad_set_enabled(g_ghostpad, runtime.ghostpad_enabled);
    }
    atomic_store(&g_assign_slot, -1);
    gc_state_set_service(g_state, GC_SERVICE_ACTIVE, NULL);
    gp_log("service: started\n");
    pthread_mutex_unlock(&g_lifecycle_lock);
    return 0;
}

static int service_stop(void)
{
    pthread_mutex_lock(&g_lifecycle_lock);
    int was_enabled = atomic_exchange(&g_service_enabled, 0);
    gc_ghostpad_set_enabled(g_ghostpad, 0);
    if (!was_enabled && active_slot_count() == 0) {
        clear_inactive_slot_claims();
        gc_state_set_service(g_state, GC_SERVICE_INACTIVE, NULL);
        pthread_mutex_unlock(&g_lifecycle_lock);
        return 0;
    }
    gc_state_set_service(g_state, GC_SERVICE_STOPPING, NULL);
    if (was_enabled && gc_ds4_bridge_stop())
        gc_state_note(g_state, "Wireless DS4 stopped with target cleanup pending");
    atomic_store(&g_assign_slot, -1);
    for (int wait = 0; wait < 50 && active_slot_count() != 0; ++wait)
        usleep(100000);
    if (active_slot_count() != 0) {
        gc_state_set_service(g_state, GC_SERVICE_ERROR,
                             "controller workers did not stop in 5 seconds");
        gp_log("service: stop timeout\n");
        pthread_mutex_unlock(&g_lifecycle_lock);
        return -1;
    }
    clear_inactive_slot_claims();
    gc_state_set_service(g_state, GC_SERVICE_INACTIVE, NULL);
    gp_log("service: stopped\n");
    pthread_mutex_unlock(&g_lifecycle_lock);
    return 0;
}

static int service_restart(void)
{
    int stopped = service_stop();
    return stopped == 0 ? service_start() : stopped;
}

static int ipc_status(void *context, char *response, size_t response_size)
{
    return gc_state_json((GcState *)context, response, response_size);
}

static int ipc_log_tail(const char *argument, char *response,
                        size_t response_size)
{
    int lines = 0;
    if (gc_log_tail_parse_lines(argument, &lines) != 0) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"invalid_log_tail_argument\"}\n");
        return -1;
    }
    /* Read only the tail window the formatter can return, so a huge log
     * never forces a huge allocation: the hex reply stays inside the IPC
     * response buffer (see gc_log_tail.h for the documented ceilings). */
    unsigned char buffer[GC_LOG_TAIL_MAX_BYTES];
    int fd = gc_open_private_regular(LOG_PATH, O_RDONLY, 0);
    if (fd < 0)
        return snprintf(response, response_size,
                        "{\"ok\":false,\"error\":\"log_unavailable\"}\n") < 0
                   ? -1 : 0;
    off_t end = lseek(fd, 0, SEEK_END);
    off_t start = end > (off_t)sizeof(buffer) ? end - (off_t)sizeof(buffer) : 0;
    if (end < 0 || lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    ssize_t length = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (length < 0)
        return -1;
    return gc_log_tail_format((const char *)buffer, (size_t)length, lines,
                              response, response_size);
}

static int ipc_profiles(char *response, size_t response_size)
{
    GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles)
        return -1;
    uint64_t generation = 0;
    size_t count = gc_config_profiles(g_config, profiles,
                                      GC_CONFIG_MAX_PROFILES, &generation);
    size_t used = (size_t)snprintf(response, response_size,
                                   "{\"ok\":true,\"generation\":%llu,\"profiles\":[",
                                   (unsigned long long)generation);
    if (used >= response_size) {
        free(profiles);
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        int written = snprintf(response + used, response_size - used,
                               "%s\"%s\"", i ? "," : "", profiles[i].id);
        if (written < 0 || (size_t)written >= response_size - used) {
            free(profiles);
            return -1;
        }
        used += (size_t)written;
    }
    free(profiles);
    return snprintf(response + used, response_size - used, "]}\n") < 0 ? -1 : 0;
}

static int parse_ipc_bool(const char *value, int *output);

static int ipc_controllers(char *response, size_t response_size)
{
    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS];
    uint64_t generation = 0;
    size_t count = gc_config_controllers(g_config, rules,
                                         GC_CONFIG_MAX_CONTROLLERS,
                                         &generation);
    size_t used = (size_t)snprintf(
        response, response_size,
        "{\"ok\":true,\"generation\":%llu,\"controllers\":[",
        (unsigned long long)generation);
    if (used >= response_size)
        return -1;
    for (size_t index = 0; index < count; ++index) {
        char name[GC_PROFILE_NAME_MAX * 2 + 1];
        if (gc_ipc_hex_encode(rules[index].name, strlen(rules[index].name),
                              name, sizeof(name)) != 0)
            return -1;
        int written = snprintf(
            response + used, response_size - used,
            "%s{\"id\":\"%s\",\"name_encoding\":\"hex\",\"name\":\"%s\","
            "\"enabled\":%s,\"ignore\":%s,\"vid\":%u,\"pid\":%u,"
            "\"protocol\":\"%s\",\"interface_number\":%d,"
            "\"in_endpoint\":%u,\"out_endpoint\":%u}",
            index ? "," : "", rules[index].id, name,
            rules[index].enabled ? "true" : "false",
            rules[index].ignore ? "true" : "false",
            (unsigned)rules[index].vid, (unsigned)rules[index].pid,
            rules[index].protocol,
            rules[index].preferred_interface_number,
            (unsigned)rules[index].preferred_in_endpoint,
            (unsigned)rules[index].preferred_out_endpoint);
        if (written < 0 || (size_t)written >= response_size - used)
            return -1;
        used += (size_t)written;
    }
    return snprintf(response + used, response_size - used, "]}\n") < 0
               ? -1 : 0;
}

static int parse_hex_u16(const char *value, uint16_t *output)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 16);
    if (errno || !*value || *end || parsed > 0xffffu)
        return -1;
    *output = (uint16_t)parsed;
    return 0;
}

static int parse_hex_u8(const char *value, uint8_t *output)
{
    if (strcmp(value, "auto") == 0) {
        *output = 0;
        return 0;
    }
    uint16_t parsed;
    if (parse_hex_u16(value, &parsed) != 0 || parsed > 0xffu)
        return -1;
    *output = (uint8_t)parsed;
    return 0;
}

static int parse_ipc_interface(const char *value, int *output)
{
    if (!strcasecmp(value, "auto")) {
        *output = -1;
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 0);
    if (errno || !*value || *end || parsed > 255u)
        return -1;
    *output = (int)parsed;
    return 0;
}

static int ipc_controller_save(const char *hex, char *response,
                               size_t response_size)
{
    char compact[512];
    size_t length = 0;
    if (gc_ipc_hex_decode(hex, compact, sizeof(compact) - 1, &length) != 0) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"invalid_controller_encoding\"}\n");
        return -1;
    }
    compact[length] = '\0';
    char *fields[10];
    char *cursor = compact;
    size_t field_count = 0;
    for (;;) {
        if (field_count == sizeof(fields) / sizeof(fields[0])) goto rejected;
        fields[field_count++] = cursor;
        char *separator = strchr(cursor, '|');
        if (!separator) break;
        *separator = '\0';
        cursor = separator + 1;
    }
    if (field_count != 9 && field_count != 10) goto rejected;
    GcControllerRule rule;
    memset(&rule, 0, sizeof(rule));
    rule.preferred_interface_number = -1;
    if (strlen(fields[0]) >= sizeof(rule.id) ||
        strlen(fields[1]) >= sizeof(rule.name) ||
        strlen(fields[4]) >= sizeof(rule.protocol) ||
        parse_hex_u16(fields[2], &rule.vid) != 0 ||
        parse_hex_u16(fields[3], &rule.pid) != 0 ||
        parse_hex_u8(fields[5], &rule.preferred_in_endpoint) != 0 ||
        parse_hex_u8(fields[6], &rule.preferred_out_endpoint) != 0 ||
        parse_ipc_bool(fields[7], &rule.enabled) != 0 ||
        parse_ipc_bool(fields[8], &rule.ignore) != 0 ||
        (field_count == 10 &&
         parse_ipc_interface(fields[9],
                             &rule.preferred_interface_number) != 0))
        goto rejected;
    snprintf(rule.id, sizeof(rule.id), "%s", fields[0]);
    snprintf(rule.name, sizeof(rule.name), "%s", fields[1]);
    snprintf(rule.protocol, sizeof(rule.protocol), "%s", fields[4]);
    char error[256] = "";
    if (gc_config_save_controller(g_config, &rule, error, sizeof(error)) != 0) {
        char detail[sizeof(error) * 2 + 1];
        (void)gc_ipc_hex_encode(error, strlen(error), detail, sizeof(detail));
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"controller_rejected\","
                 "\"detail_encoding\":\"hex\",\"detail\":\"%s\"}\n",
                 detail);
        return -1;
    }
    gc_state_set_generation(g_state, gc_config_generation(g_config));
    snprintf(response, response_size,
             "{\"ok\":true,\"id\":\"%s\",\"generation\":%llu}\n",
             rule.id, (unsigned long long)gc_config_generation(g_config));
    return 0;

rejected:
    snprintf(response, response_size,
             "{\"ok\":false,\"error\":\"invalid_controller_rule\"}\n");
    return -1;
}

static int ipc_profile_get(const char *id, char *response, size_t response_size)
{
    GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles)
        return -1;
    size_t count = gc_config_profiles(g_config, profiles,
                                      GC_CONFIG_MAX_PROFILES, NULL);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(profiles[i].id, id) != 0)
            continue;
        char ini[GC_PROFILE_TEXT_MAX];
        int length = gc_profile_serialize(&profiles[i], ini, sizeof(ini));
        if (length < 0) {
            free(profiles);
            return -1;
        }
        char hex[GC_PROFILE_TEXT_MAX * 2 + 1];
        if (gc_ipc_hex_encode(ini, (size_t)length, hex, sizeof(hex)) != 0) {
            free(profiles);
            return -1;
        }
        int result = snprintf(response, response_size,
                              "{\"ok\":true,\"id\":\"%s\",\"encoding\":\"hex\",\"data\":\"%s\"}\n",
                              id, hex) < 0 ? -1 : 0;
        free(profiles);
        return result;
    }
    free(profiles);
    snprintf(response, response_size,
             "{\"ok\":false,\"error\":\"profile_not_found\"}\n");
    return -1;
}

static int ipc_profile_save(const char *hex, char *response,
                            size_t response_size)
{
    char ini[GC_PROFILE_TEXT_MAX];
    size_t length = 0;
    char error[256] = "";
    GcProfile profile;
    if (gc_ipc_hex_decode(hex, ini, sizeof(ini) - 1, &length) != 0) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"invalid_profile_encoding\"}\n");
        return -1;
    }
    ini[length] = '\0';
    if (gc_profile_parse(ini, length, &profile, error, sizeof(error)) != 0 ||
        gc_config_save_profile(g_config, &profile, error, sizeof(error)) != 0) {
        char error_hex[sizeof(error) * 2 + 1];
        (void)gc_ipc_hex_encode(error, strlen(error), error_hex,
                                sizeof(error_hex));
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"profile_rejected\",\"detail_encoding\":\"hex\",\"detail\":\"%s\"}\n",
                 error_hex);
        return -1;
    }
    gc_state_set_generation(g_state, gc_config_generation(g_config));
    snprintf(response, response_size,
             "{\"ok\":true,\"id\":\"%s\",\"generation\":%llu}\n",
             profile.id, (unsigned long long)gc_config_generation(g_config));
    return 0;
}

static int ipc_profile_rename(const char *hex, char *response,
                              size_t response_size)
{
    char names[GC_PROFILE_ID_MAX * 2 + 2];
    size_t length = 0;
    char error[256] = "";
    if (gc_ipc_hex_decode(hex, names, sizeof(names) - 1, &length) != 0) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"invalid_profile_encoding\"}\n");
        return -1;
    }
    names[length] = '\0';
    char *new_id = strchr(names, '|');
    if (!new_id || new_id == names || !new_id[1] || strchr(new_id + 1, '|')) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"invalid_profile_rename\"}\n");
        return -1;
    }
    *new_id++ = '\0';
    if (gc_config_rename_profile(g_config, names, new_id,
                                 error, sizeof(error)) != 0) {
        char detail[sizeof(error) * 2 + 1];
        (void)gc_ipc_hex_encode(error, strlen(error), detail, sizeof(detail));
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"profile_rename_failed\","
                 "\"detail_encoding\":\"hex\",\"detail\":\"%s\"}\n",
                 detail);
        return -1;
    }
    gc_state_set_generation(g_state, gc_config_generation(g_config));
    snprintf(response, response_size,
             "{\"ok\":true,\"id\":\"%s\"}\n", new_id);
    return 0;
}

static int ipc_profile_operation(const char *operation, const char *id,
                                 char *response, size_t response_size)
{
    char error[256] = "";
    int result = -1;
    if (strcmp(operation, "PROFILE_CREATE") == 0)
        result = gc_config_create_profile(g_config, id, error, sizeof(error));
    else if (strcmp(operation, "PROFILE_IMPORT") == 0)
        result = gc_config_import_profile(g_config, id, error, sizeof(error));
    else if (strcmp(operation, "PROFILE_EXPORT") == 0)
        result = gc_config_export_profile(g_config, id, error, sizeof(error));
    if (result == 0) {
        gc_state_set_generation(g_state, gc_config_generation(g_config));
        snprintf(response, response_size,
                 "{\"ok\":true,\"command\":\"%s\",\"id\":\"%s\"}\n",
                 operation, id);
        return 0;
    }
    char detail[sizeof(error) * 2 + 1];
    (void)gc_ipc_hex_encode(error, strlen(error), detail, sizeof(detail));
    snprintf(response, response_size,
             "{\"ok\":false,\"error\":\"profile_operation_failed\","
             "\"detail_encoding\":\"hex\",\"detail\":\"%s\"}\n",
             detail);
    return -1;
}

/* PROFILE_EDIT id|key|value: read-modify-write one field of a saved profile.
 * The split validates the parts (1.x field rules) before the store is touched;
 * the store then applies the value through the loader's own key parser, so an
 * edit that saves is guaranteed to reload. */
static int ipc_profile_edit(const char *argument, char *response,
                            size_t response_size)
{
    char id[GC_PROFILE_EDIT_ID_MAX];
    char key[GC_PROFILE_EDIT_KEY_MAX + 1];
    char value[GC_PROFILE_EDIT_VALUE_MAX + 1];
    char error[256] = "";
    int result = gc_profile_edit_split(argument, id, sizeof(id), key,
                                       sizeof(key), value, sizeof(value),
                                       error, sizeof(error));
    if (result == 0)
        result = gc_config_edit_profile(g_config, id, key, value, error,
                                        sizeof(error));
    if (result != 0) {
        char detail[sizeof(error) * 2 + 1];
        (void)gc_ipc_hex_encode(error, strlen(error), detail, sizeof(detail));
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"profile_edit_failed\","
                 "\"detail_encoding\":\"hex\",\"detail\":\"%s\"}\n",
                 detail);
        return -1;
    }
    gc_state_set_generation(g_state, gc_config_generation(g_config));
    snprintf(response, response_size,
             "{\"ok\":true,\"id\":\"%s\",\"key\":\"%s\",\"generation\":%llu}\n",
             id, key, (unsigned long long)gc_config_generation(g_config));
    return 0;
}

static int parse_ipc_bool(const char *value, int *output)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *output = 1;
        return 0;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *output = 0;
        return 0;
    }
    return -1;
}

#include "gc_runtime_commands.h"

static int ipc_runtime_get(char *response, size_t response_size)
{
    GcGhostpadStatus ghostpad = {0};
    gc_ghostpad_snapshot(g_ghostpad, &ghostpad);
    return gc_runtime_command_get(g_config, &ghostpad, response, response_size);
}

static int ipc_runtime_set(const char *argument, char *response, size_t response_size)
{
    int result = gc_runtime_command_set(g_config, argument, response, response_size);
    if (result == 0) {
        /* Invalidate the update cache only. Saving a preference is not proof
         * of an applied runtime change; no legacy bridge is invoked here. */
        if (!strncmp(argument, "include_prereleases=", 20) ||
            !strncmp(argument, "update_enabled=", 15)) {
            pthread_mutex_lock(&g_update_lock);
            g_latest_update_valid = 0;
            pthread_mutex_unlock(&g_update_lock);
        }
        gc_state_set_generation(g_state, gc_config_generation(g_config));
    }
    return result;
}

static int ipc_update_error(const char *error, char *response,
                            size_t response_size)
{
    char detail[513];
    if (gc_ipc_hex_encode(error ? error : "update failed",
                          strlen(error ? error : "update failed"),
                          detail, sizeof(detail)) != 0)
        snprintf(detail, sizeof(detail), "757064617465206661696c6564");
    snprintf(response, response_size,
             "{\"ok\":false,\"error\":\"update_failed\",\"detail_encoding\":\"hex\",\"detail\":\"%s\"}\n",
             detail);
    return -1;
}

static int ipc_update_check(char *response, size_t response_size)
{
    GcRuntimeConfig runtime;
    char error[256] = "";
    if (!g_config || gc_config_snapshot(g_config, &runtime, NULL) != 0)
        return ipc_update_error("configuration unavailable", response,
                                response_size);
    if (!runtime.update_enabled)
        return ipc_update_error("updates are disabled", response,
                                response_size);
    GcUpdateInfo info;
    if (gc_update_check(GC_VERSION, runtime.include_prereleases, &info,
                        error, sizeof(error)) != 0)
        return ipc_update_error(error, response, response_size);
    pthread_mutex_lock(&g_update_lock);
    g_latest_update = info;
    g_latest_update_valid = 1;
    pthread_mutex_unlock(&g_update_lock);
    char tag[GC_UPDATE_TAG_MAX * 2 + 1];
    (void)gc_ipc_hex_encode(info.tag, strlen(info.tag), tag, sizeof(tag));
    snprintf(response, response_size,
             "{\"ok\":true,\"available\":%s,\"tag_encoding\":\"hex\","
             "\"tag\":\"%s\",\"size\":%llu,\"sha256\":\"%s\"}\n",
             info.available ? "true" : "false", tag,
             (unsigned long long)info.size, info.sha256);
    if (info.available)
        gc_state_note(g_state, "A verified Ghostcontrol update is available");
    return 0;
}

static int ipc_update_download(char *response, size_t response_size)
{
    GcUpdateInfo info;
    char error[256] = "";
    GcRuntimeConfig runtime;
    if (!g_config || gc_config_snapshot(g_config, &runtime, NULL) != 0 ||
        !runtime.update_enabled)
        return ipc_update_error("updates are disabled", response,
                                response_size);
    pthread_mutex_lock(&g_update_lock);
    int valid = g_latest_update_valid;
    info = g_latest_update;
    pthread_mutex_unlock(&g_update_lock);
    if (!valid) {
        if (ipc_update_check(response, response_size) != 0)
            return -1;
        pthread_mutex_lock(&g_update_lock);
        info = g_latest_update;
        pthread_mutex_unlock(&g_update_lock);
    }
    if (!info.available)
        return snprintf(response, response_size,
                        "{\"ok\":true,\"downloaded\":false,\"reason\":\"up_to_date\"}\n") < 0
                   ? -1 : 0;
    if (gc_update_download(GC_DATA_ROOT, &info, error, sizeof(error)) != 0)
        return ipc_update_error(error, response, response_size);
    char tag[GC_UPDATE_TAG_MAX * 2 + 1];
    (void)gc_ipc_hex_encode(info.tag, strlen(info.tag), tag, sizeof(tag));
    snprintf(response, response_size,
             "{\"ok\":true,\"downloaded\":true,\"tag_encoding\":\"hex\",\"tag\":\"%s\",\"path\":\"/data/ghostcontrol/updates/Ghostcontrol-Toolbox-app.zip\"}\n",
             tag);
    gc_state_note(g_state, "Verified Ghostcontrol update downloaded and staged");
    return 0;
}

static int ipc_update_rollback(char *response, size_t response_size)
{
    char error[256] = "";
    if (gc_update_rollback(GC_DATA_ROOT, error, sizeof(error)) != 0)
        return ipc_update_error(error, response, response_size);
    gc_state_note(g_state, "Ghostcontrol staged update rolled back");
    return snprintf(response, response_size,
                    "{\"ok\":true,\"rolled_back\":true}\n") < 0 ? -1 : 0;
}

static int setup_json_response(struct json_object *json, char *response, size_t size)
{
    const char *text = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);
    int written = snprintf(response, size, "%s\n", text);
    json_object_put(json);
    return written < 0 || (size_t)written >= size ? -1 : 0;
}

/* ── Bluetooth input source bridge ────────────────────────────────────────
 * The gc_bt engine owns the userland Bluetooth host. When a pad becomes
 * ready it claims one ordinary Ghostcontrol slot and travels the same
 * VDA / assignment / profile / guided-setup / VDI path as USB and GhostPad.
 * Raw HID reports are fed through gc_hid / gc_profile / gc_rawmap / gc_setup
 * exactly like a USB controller, so custom configuration and the Custom
 * Controller Setup wizard work identically over Bluetooth. */

static void bt_format_addr(char out[18], const unsigned char addr[6])
{
    /* Host stores the address least-significant byte first; show it the way
     * the pad is labelled and the Toolbox sends it back. */
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static int bt_find_gcslot(int bt_slot)
{
    if (bt_slot < 0 || bt_slot >= GC_BT_MAX_PADS) return -1;
    return g_bt_gcslot[bt_slot];
}

static int bt_ready(void *ud, int bt_slot, const GcBtOrigin *origin, const GcBtController *c)
{
    (void)ud;
    (void)origin;
    if (bt_slot < 0 || bt_slot >= GC_BT_MAX_PADS) return -1;

    controller_kind_t kind = c->le ? CTRL_BT_LE : CTRL_BT;
    char addr[18];
    bt_format_addr(addr, c->addr);
    char path[32];
    snprintf(path, sizeof(path), "bt://%s", addr);

    GcHidLayout layout;
    memset(&layout, 0, sizeof(layout));
    int layout_valid = c->desc_len > 0 &&
        gc_hid_parse_descriptor(c->desc, (size_t)c->desc_len, &layout) == 0;

    int slot = -1;
    pthread_mutex_lock(&g_lifecycle_lock);
    if (!atomic_load(&g_service_enabled) || atomic_load(&g_shutdown) ||
        atomic_load(&g_assign_slot) >= 0)
        goto rejected;

    pthread_mutex_lock(&g_slot_lock);
    for (int index = 0; index < MAX_SLOTS; ++index) {
        if (g_slots[index].handle < 0 && !g_slots[index].usb_active &&
            !g_slots[index].dev_path[0] && !g_slots[index].retry_ready) {
            slot = index;
            reset_slot_record(&g_slots[slot]);
            snprintf(g_slots[slot].dev_path, sizeof(g_slots[slot].dev_path),
                     "%s", path);
            g_slots[slot].usb_active = 1;
            g_slots[slot].setup_reader_active = 1;
            g_slots[slot].kind = kind;
            g_slots[slot].vid = c->vid;
            g_slots[slot].pid = c->pid;
            if (layout_valid) {
                g_slots[slot].hid_layout = layout;
                g_slots[slot].hid_layout_valid = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    if (slot < 0)
        goto rejected;

    atomic_store(&g_assign_slot, slot);
    {
        char name[96];
        if (c->profile[0])
            snprintf(name, sizeof(name), "Bluetooth: %s", c->profile);
        else
            snprintf(name, sizeof(name), "Bluetooth controller");
        publish_slot_state(slot, name, "automatic", 0, -1);
        notify("%s connected — choose a user, then tap PS", name);
    }

    int32_t handle = create_vda_for_slot(slot);
    if (handle < 0) {
        int expected = slot;
        (void)atomic_compare_exchange_strong(&g_assign_slot, &expected, -1);
        pthread_mutex_lock(&g_slot_lock);
        reset_slot_record(&g_slots[slot]);
        pthread_mutex_unlock(&g_slot_lock);
        gc_state_clear_slot(g_state, (size_t)slot);
        notify("Bluetooth controller could not create a virtual controller");
        goto rejected;
    }
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].handle = handle;
    g_slots[slot].vdi_ready = 1;
    pthread_mutex_unlock(&g_slot_lock);
    publish_slot_state(slot, NULL, NULL, 0, -1);
    pthread_mutex_unlock(&g_lifecycle_lock);

    g_bt_gcslot[bt_slot] = slot;
    g_bt_kind[bt_slot] = kind;
    g_bt_vid[bt_slot] = c->vid;
    g_bt_pid[bt_slot] = c->pid;
    snprintf(g_bt_path[bt_slot], sizeof(g_bt_path[bt_slot]), "%s", path);
    g_bt_layout[bt_slot] = layout;
    g_bt_layout_valid[bt_slot] = layout_valid;
    gc_profile_defaults(&g_bt_profile[bt_slot]);
    g_bt_profile_gen[bt_slot] = ~0ull;      /* force first refresh */
    g_bt_last_valid[bt_slot] = 0;
    g_bt_ready_notified[bt_slot] = 0;
    gc_bt_set_virtual(g_bt, bt_slot, origin, 1);
    gp_log("bt: slot[%d] %s %04x:%04x profile=%s handle=0x%x layout=%d ready\n",
           slot, path, c->vid, c->pid, c->profile[0] ? c->profile : "generic",
           (uint32_t)handle, layout_valid);
    return 0;

rejected:
    pthread_mutex_unlock(&g_lifecycle_lock);
    gp_log("bt: pad %s rejected: service, assignment, or slot unavailable\n", addr);
    return -1;
}

static void bt_state(void *ud, int bt_slot, const GcBtOrigin *origin, const GcBtPad *pad)
{
    (void)ud;
    (void)origin;
    if (bt_slot < 0 || bt_slot >= GC_BT_MAX_PADS || !pad) return;
    g_bt_last[bt_slot] = *pad;
    g_bt_last_valid[bt_slot] = 1;
}

static void bt_report(void *ud, int bt_slot, const GcBtOrigin *origin, const uint8_t *rep, int len)
{
    (void)ud;
    (void)origin;
    int slot = bt_find_gcslot(bt_slot);
    if (slot < 0 || len < 1) return;
    const char *dev_path = g_bt_path[bt_slot];
    controller_kind_t kind = g_bt_kind[bt_slot];
    uint16_t vid = g_bt_vid[bt_slot], pid = g_bt_pid[bt_slot];
    int layout_valid = g_bt_layout_valid[bt_slot];

    /* Base from the host's own normalized parse (gives known pads correct
     * output instantly, plus sticks/triggers/motion). */
    ScePadData pad;
    memset(&pad, 0, sizeof(pad));
    pad.leftStick.x = pad.leftStick.y = 128;
    pad.rightStick.x = pad.rightStick.y = 128;
    if (g_bt_last_valid[bt_slot]) {
        const GcBtPad *b = &g_bt_last[bt_slot];
        pad.buttons = b->buttons;
        pad.leftStick.x = b->lx; pad.leftStick.y = b->ly;
        pad.rightStick.x = b->rx; pad.rightStick.y = b->ry;
        pad.analogButtons.l2 = b->l2; pad.analogButtons.r2 = b->r2;
    }
    pad.connected = 1;
    pad.quat.w = 1.0f;

    /* Pre-remap state for the wizard: a generic HID parse when we have the
     * descriptor, else the host's normalized state. */
    ScePadData setup_pad = pad;
    if (layout_valid)
        (void)gc_hid_input(&g_bt_layout[bt_slot], rep, (size_t)len, &setup_pad);

    const uint64_t now = setup_now_ms();
    const size_t raw_copy = (size_t)len < GC_RAW_REPORT_MAX ? (size_t)len
                                                            : GC_RAW_REPORT_MAX;
    uint32_t setup_buttons = gc_setup_learning_buttons(&setup_pad);
    pthread_mutex_lock(&g_slot_lock);
    memcpy(g_slots[slot].last_raw_report, rep, raw_copy);
    g_slots[slot].last_raw_length = (uint16_t)len;
    g_slots[slot].last_raw_copied = (uint16_t)raw_copy;
    g_slots[slot].last_raw_ms = now;
    ++g_slots[slot].raw_report_count;
    g_slots[slot].last_parse_result = 1;
    g_slots[slot].last_input = setup_pad;
    g_slots[slot].last_setup_buttons = setup_buttons;
    g_slots[slot].last_input_valid = 1;
    ++g_slots[slot].parsed_report_count;
    pthread_mutex_unlock(&g_slot_lock);

    /* Guided / custom setup: identical contract to USB. */
    int suppress = 0, setup_completed = 0;
    pthread_mutex_lock(&g_setup_lock);
    uint64_t previous_revision = g_setup.revision;
    int matching_setup = g_setup.active && !strcmp(g_setup.path, dev_path);
    if (matching_setup) {
        g_setup.input = setup_pad;
        g_setup.input_ms = now;
    }
    suppress = layout_valid
        ? gc_setup_feed_hid(&g_setup, dev_path, &g_bt_layout[bt_slot],
                            rep, (size_t)len, &setup_pad, now)
        : gc_setup_feed_input(&g_setup, dev_path, &setup_pad, now);
    setup_completed = matching_setup && g_setup.completed &&
                      g_setup.revision != previous_revision;
    pthread_mutex_unlock(&g_setup_lock);
    if (setup_completed)
        notify("Guided setup complete. Review and save.");

    refresh_profile(slot, vid, pid, kind, &g_bt_profile[bt_slot],
                    &g_bt_profile_gen[bt_slot]);
    gc_profile_apply(&g_bt_profile[bt_slot], &pad);
    gc_rawmap_apply(&g_bt_profile[bt_slot], rep, (size_t)len, &pad);

    if (suppress) {
        pad.buttons = 0;
        pad.leftStick.x = pad.leftStick.y = 128;
        pad.rightStick.x = pad.rightStick.y = 128;
        pad.analogButtons.l2 = pad.analogButtons.r2 = 0;
    }

    if (!g_bt_ready_notified[bt_slot]) {
        notify("Slot %d input ready — choose a user", slot + 1);
        g_bt_ready_notified[bt_slot] = 1;
        publish_slot_state(slot, NULL, g_bt_profile[bt_slot].id, 1, -2);
    }

    (void)inject_pad(slot, &pad);
}

static void bt_lost(void *ud, int bt_slot, const GcBtOrigin *origin)
{
    (void)ud;
    int slot = bt_find_gcslot(bt_slot);
    if (slot < 0) return;
    g_bt_gcslot[bt_slot] = -1;
    gc_bt_set_virtual(g_bt, bt_slot, origin, 0);

    pthread_mutex_lock(&g_slot_lock);
    int32_t handle = g_slots[slot].handle;
    pthread_mutex_unlock(&g_slot_lock);

    ScePadData neutral;
    memset(&neutral, 0, sizeof(neutral));
    neutral.leftStick.x = neutral.leftStick.y = 128;
    neutral.rightStick.x = neutral.rightStick.y = 128;
    neutral.connected = 1;
    neutral.quat.w = 1.0f;
    (void)inject_pad(slot, &neutral);

    int expected = slot;
    (void)atomic_compare_exchange_strong(&g_assign_slot, &expected, -1);

    /* Cancel an in-flight setup bound to this controller. */
    pthread_mutex_lock(&g_setup_lock);
    if (g_setup.active && !strcmp(g_setup.path, g_bt_path[bt_slot]))
        gc_setup_cancel(&g_setup, "Bluetooth controller disconnected.");
    pthread_mutex_unlock(&g_setup_lock);

    int released = release_vda_for_slot(slot, handle) == 0;
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].usb_active = 0;
    g_slots[slot].setup_reader_active = 0;
    if (released)
        reset_slot_record(&g_slots[slot]);
    pthread_mutex_unlock(&g_slot_lock);
    if (released)
        gc_state_clear_slot(g_state, (size_t)slot);
    else
        publish_slot_state(slot, NULL, NULL, 0, -2);
    notify("Bluetooth controller disconnected");
    gp_log("bt: slot[%d] disconnected cleanup=%s\n", slot,
           released ? "complete" : "retrying");
}

/* ── Bluetooth IPC (in-process, served from the live gc_bt engine) ─────── */
static int ipc_bluetooth_status(char *response, size_t response_size)
{
    GcBtStatus status;
    gc_bt_snapshot(g_bt, &status);
    return gc_bluetooth_format_status(&status, response, response_size);
}

static int ipc_bluetooth_command(const char *command, const char *argument,
                                 char *response, size_t response_size)
{
    int ok = -1;
    if (!strcmp(command, "BLUETOOTH_PAIR"))
        ok = gc_bt_pair(g_bt, 60);
    else if (!strcmp(command, "BLUETOOTH_RETRY"))
        ok = gc_bt_retry(g_bt);
    else if (!strcmp(command, "BLUETOOTH_FORGET")) {
        char canonical[18];
        if (gc_bluetooth_canonical_address(argument, canonical) != 0) {
            snprintf(response, response_size,
                     "{\"ok\":false,\"error\":\"invalid_bluetooth_address\"}\n");
            return -1;
        }
        ok = gc_bt_forget(g_bt, canonical);
    } else {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"unknown_bluetooth_command\"}\n");
        return -1;
    }
    if (ok != 0) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"bluetooth_control_unavailable\"}\n");
        return -1;
    }
    int written = snprintf(response, response_size,
                           "{\"ok\":true,\"command\":\"%s\"}\n", command);
    return written < 0 || (size_t)written >= response_size ? -1 : 0;
}

static int ipc_devices(char *response, size_t size)
{
    GcUsbDevice *devices = calloc(32, sizeof(*devices));
    if (!devices) return -1;
    size_t count = gc_usb_devices(devices, 32);
    struct json_object *root = json_object_new_object(), *list = json_object_new_array();
    json_object_object_add(root, "ok", json_object_new_boolean(1));
    json_object_object_add(root, "devices", list);
    for (size_t i = 0; i < count; ++i) {
        GcUsbDevice *d = &devices[i];
        controller_kind_t kind = CTRL_NONE;
        if (d->kind == GC_USB_INTERFACE_GIP) kind = CTRL_XBOX_GIP;
        else if (d->kind == GC_USB_INTERFACE_XUSB) kind = CTRL_XUSB;
        else kind = match_known_vidpid(d->vid, d->pid);
        if (kind == CTRL_NONE && d->kind == GC_USB_INTERFACE_HID) kind = CTRL_HID;
        GcControllerRule rule;
        memset(&rule, 0, sizeof(rule));
        const int rule_found =
            gc_config_find_controller(g_config, d->vid, d->pid,
                                      &rule, NULL) == 0;
        const int ignored = rule_found && rule.ignore;
        int rule_matches = 0;
        if (rule_found && !ignored) {
            controller_kind_t ruled_kind =
                controller_kind_from_key(rule.protocol);
            GcUsbDevice ruled_device;
            GcUsbInterfaceKind required = usb_interface_kind(ruled_kind);
            if (required != GC_USB_INTERFACE_NONE &&
                gc_usb_inspect_matching(d->path, required,
                    rule.preferred_interface_number, &ruled_device) == 0 &&
                usb_interface_has_endpoint(&ruled_device,
                    rule.preferred_in_endpoint, 1) &&
                usb_interface_has_endpoint(&ruled_device,
                    rule.preferred_out_endpoint, 0)) {
                *d = ruled_device;
                kind = ruled_kind;
                rule_matches = 1;
            }
        }
        GcProfile exact_profile;
        const int profile_found = gc_config_find_exact_profile(
            g_config, d->vid, d->pid, controller_kind_key(kind),
            &exact_profile, NULL) == 0;
        const int configured = rule_matches || profile_found;
        const char *device_name = rule_matches && rule.name[0] ? rule.name :
            profile_found && exact_profile.name[0] ? exact_profile.name :
            controller_display_name(kind, d->vid, d->pid);
        struct json_object *item = json_object_new_object();
        json_object_object_add(item, "path", json_object_new_string(d->path));
        json_object_object_add(item, "vid", json_object_new_int(d->vid));
        json_object_object_add(item, "pid", json_object_new_int(d->pid));
        json_object_object_add(item, "protocol", json_object_new_string(controller_kind_key(kind)));
        json_object_object_add(item, "name", json_object_new_string(device_name));
        json_object_object_add(item, "configured", json_object_new_boolean(configured));
        const int setup_supported = kind != CTRL_NONE;
        json_object_object_add(item, "supported",
                               json_object_new_boolean(setup_supported));
        json_object_object_add(item, "setup_required",
                               json_object_new_boolean(!configured &&
                                                       !ignored &&
                                                       setup_supported));
        json_object_object_add(item, "ignored", json_object_new_boolean(ignored));
        json_object_object_add(item, "rule_mismatch",
                               json_object_new_boolean(rule_found &&
                                                       !ignored &&
                                                       !rule_matches));
        json_object_object_add(item, "interface_index", json_object_new_int(d->interface_index));
        json_object_object_add(item, "interface_number", json_object_new_int(d->interface_number));
        int streaming = 0;
        GcStateSnapshot state;
        if (gc_state_snapshot(g_state, &state) == 0)
            for (int j = 0; j < MAX_SLOTS; ++j)
                if (state.slots[j].connected && state.slots[j].streaming && !strcmp(state.slots[j].device_path, d->path))
                    streaming = 1;
        json_object_object_add(item, "streaming", json_object_new_boolean(streaming));
        struct json_object *inputs = json_object_new_array(), *outputs = json_object_new_array();
        for (unsigned j = 0; j < d->in_count; ++j) json_object_array_add(inputs, json_object_new_int(d->in[j]));
        for (unsigned j = 0; j < d->out_count; ++j) json_object_array_add(outputs, json_object_new_int(d->out[j]));
        json_object_object_add(item, "inputs", inputs);
        json_object_object_add(item, "outputs", outputs);
        json_object_array_add(list, item);
    }
    free(devices);

    /* Bluetooth controllers appear in the same device list so the Custom
     * Controller Setup wizard can target them exactly like USB. */
    if (g_bt) {
        GcBtStatus bts;
        gc_bt_snapshot(g_bt, &bts);
        GcStateSnapshot bt_state_snap;
        int have_state = gc_state_snapshot(g_state, &bt_state_snap) == 0;
        for (int i = 0; i < bts.controller_count; ++i) {
            char addr[18], path[32];
            bt_format_addr(addr, bts.controllers[i].addr);
            snprintf(path, sizeof(path), "bt://%s", addr);
            controller_kind_t kind = bts.controllers[i].le ? CTRL_BT_LE : CTRL_BT;
            const char *proto = controller_kind_key(kind);
            GcProfile exact;
            const int configured = g_config &&
                gc_config_find_exact_profile(g_config, bts.controllers[i].vid,
                    bts.controllers[i].pid, proto, &exact, NULL) == 0;
            const char *device_name = bts.controllers[i].profile[0]
                ? bts.controllers[i].profile
                : controller_display_name(kind, bts.controllers[i].vid,
                                          bts.controllers[i].pid);
            struct json_object *item = json_object_new_object();
            json_object_object_add(item, "path", json_object_new_string(path));
            json_object_object_add(item, "vid",
                json_object_new_int(bts.controllers[i].vid));
            json_object_object_add(item, "pid",
                json_object_new_int(bts.controllers[i].pid));
            json_object_object_add(item, "protocol",
                json_object_new_string(proto));
            json_object_object_add(item, "name",
                json_object_new_string(device_name));
            json_object_object_add(item, "configured",
                json_object_new_boolean(configured));
            json_object_object_add(item, "supported",
                json_object_new_boolean(1));
            json_object_object_add(item, "setup_required",
                json_object_new_boolean(!configured));
            json_object_object_add(item, "ignored", json_object_new_boolean(0));
            json_object_object_add(item, "rule_mismatch",
                json_object_new_boolean(0));
            json_object_object_add(item, "interface_index",
                json_object_new_int(-1));
            json_object_object_add(item, "interface_number",
                json_object_new_int(-1));
            int streaming = 0;
            if (have_state)
                for (int j = 0; j < MAX_SLOTS; ++j)
                    if (bt_state_snap.slots[j].connected &&
                        bt_state_snap.slots[j].streaming &&
                        !strcmp(bt_state_snap.slots[j].device_path, path))
                        streaming = 1;
            json_object_object_add(item, "streaming",
                json_object_new_boolean(streaming));
            json_object_object_add(item, "transport",
                json_object_new_string("bluetooth"));
            json_object_object_add(item, "inputs", json_object_new_array());
            json_object_object_add(item, "outputs", json_object_new_array());
            json_object_array_add(list, item);
        }
    }

    return setup_json_response(root, response, size);
}

static int ipc_setup(const char *command, const char *argument, char *response, size_t size)
{
    char error[256] = "";
    char warning[256] = "";
    int restart_after_save = 0;
    pthread_mutex_lock(&g_setup_lock);
    int tick_target = g_setup.target;
    int tick_guided = g_setup.guided;
    uint64_t tick_revision = g_setup.revision;
    uint64_t tick_now_ms = setup_now_ms();
    gc_setup_tick(&g_setup, tick_now_ms);
    if (tick_target >= 0 && g_setup.revision != tick_revision &&
        g_setup.target != tick_target) {
        gp_log("setup timer IPC: target=%d(%s)->%d(%s) guided=%d now=%llu revision=%llu->%llu message=%s\n",
               tick_target, gc_setup_target_name(tick_target),
               g_setup.target, gc_setup_target_name(g_setup.target), tick_guided,
               (unsigned long long)tick_now_ms,
               (unsigned long long)tick_revision,
               (unsigned long long)g_setup.revision, g_setup.message);
    }
    if (!strcmp(command, "SETUP_INPUT")) {
        ScePadData preview = g_setup.input;
        uint8_t raw_report[GC_RAW_REPORT_MAX];
        uint16_t raw_length = 0, raw_copied = 0;
        uint64_t raw_ms = 0;
        uint32_t raw_count = 0, parsed_count = 0, source_buttons = 0;
        int parser_result = 0, input_endpoint = 0;
        int interface_index = -1;
        const char *transport = "unknown";
        pthread_mutex_lock(&g_slot_lock);
        for (int slot = 0; slot < MAX_SLOTS; ++slot) {
            if (!g_slots[slot].usb_active ||
                !g_slots[slot].setup_reader_active ||
                !g_slots[slot].dev_path[0] ||
                strcmp(g_slots[slot].dev_path, g_setup.path) != 0)
                continue;
            raw_length = g_slots[slot].last_raw_length;
            raw_copied = g_slots[slot].last_raw_copied;
            raw_ms = g_slots[slot].last_raw_ms;
            raw_count = g_slots[slot].raw_report_count;
            parsed_count = g_slots[slot].parsed_report_count;
            parser_result = g_slots[slot].last_parse_result;
            source_buttons = g_slots[slot].last_setup_buttons;
            input_endpoint = g_slots[slot].input_endpoint;
            interface_index = g_slots[slot].interface_index;
            transport = controller_kind_key(g_slots[slot].kind);
            if (raw_copied > GC_RAW_REPORT_MAX)
                raw_copied = GC_RAW_REPORT_MAX;
            memcpy(raw_report, g_slots[slot].last_raw_report, raw_copied);
            break;
        }
        pthread_mutex_unlock(&g_slot_lock);
        char raw_hex[GC_RAW_REPORT_MAX * 2 + 1];
        for (uint16_t i = 0; i < raw_copied; ++i)
            (void)snprintf(raw_hex + (size_t)i * 2,
                           sizeof(raw_hex) - (size_t)i * 2, "%02x",
                           raw_report[i]);
        raw_hex[(size_t)raw_copied * 2] = '\0';
        if (g_setup.active && g_setup.input_ms) {
            if (g_setup.hid)
                gc_hid_apply_profile_values(&g_setup.hid_layout,
                                            g_setup.hid_values,
                                            g_setup.hid_count,
                                            &g_setup.profile, &preview);
            gc_profile_apply(&g_setup.profile, &preview);
            gc_rawmap_apply(&g_setup.profile, raw_report, raw_copied, &preview);
        }
        else {
            memset(&preview, 0, sizeof(preview));
            preview.leftStick.x = preview.leftStick.y = preview.rightStick.x = preview.rightStick.y = 128;
        }
        struct json_object *input = json_object_new_object();
        json_object_object_add(input, "ok", json_object_new_boolean(1));
        json_object_object_add(input, "buttons", json_object_new_int64(preview.buttons & ~0x002e0000u));
        json_object_object_add(input, "source_buttons", json_object_new_int64(source_buttons));
        json_object_object_add(input, "observed_buttons", json_object_new_int64(g_setup.observed_buttons));
        json_object_object_add(input, "learned", json_object_new_int64(g_setup.learned));
        json_object_object_add(input, "neutral", json_object_new_boolean(g_setup.neutral));
        json_object_object_add(input, "completed", json_object_new_boolean(g_setup.completed));
        json_object_object_add(input, "confirming", json_object_new_boolean(
            g_setup.pending_pattern &&
            g_setup.pending_pattern_target == g_setup.target));
        json_object_object_add(input, "revision", json_object_new_int64((int64_t)g_setup.revision));
        const uint64_t now_ms = setup_now_ms();
        json_object_object_add(input, "deadline_remaining_ms", json_object_new_int64(
            g_setup.target >= 0 && g_setup.deadline_ms > now_ms
                ? (int64_t)(g_setup.deadline_ms - now_ms) : 0));
        json_object_object_add(input, "target", json_object_new_string(
            gc_setup_target_name(g_setup.target)));
        int setup_step = 0, setup_total = 0;
        const char *setup_stage = "review";
        gc_setup_progress(&g_setup, &setup_step, &setup_total, &setup_stage);
        json_object_object_add(input, "stage",
                               json_object_new_string(setup_stage));
        json_object_object_add(input, "step",
                               json_object_new_int(setup_step));
        json_object_object_add(input, "total",
                               json_object_new_int(setup_total));
        json_object_object_add(input, "message", json_object_new_string(g_setup.message));
        json_object_object_add(input, "transport", json_object_new_string(transport));
        json_object_object_add(input, "raw_report", json_object_new_string(raw_hex));
        json_object_object_add(input, "raw_length", json_object_new_int(raw_length));
        json_object_object_add(input, "raw_truncated", json_object_new_boolean(raw_length > raw_copied));
        json_object_object_add(input, "raw_report_count", json_object_new_int64(raw_count));
        json_object_object_add(input, "parsed_report_count", json_object_new_int64(parsed_count));
        json_object_object_add(input, "parser_result", json_object_new_int(parser_result));
        json_object_object_add(input, "input_endpoint", json_object_new_int(input_endpoint));
        json_object_object_add(input, "interface_index", json_object_new_int(interface_index));
        json_object_object_add(input, "last_report_ms", json_object_new_int64((int64_t)raw_ms));
        json_object_object_add(input, "setup_input_ms", json_object_new_int64((int64_t)g_setup.input_ms));
        struct json_object *axes = json_object_new_array();
        const uint8_t values[] = {preview.leftStick.x, preview.leftStick.y, preview.rightStick.x,
            preview.rightStick.y, preview.analogButtons.l2, preview.analogButtons.r2};
        for (size_t i = 0; i < sizeof(values); ++i) json_object_array_add(axes, json_object_new_int(values[i]));
        json_object_object_add(input, "axes", axes);
        pthread_mutex_unlock(&g_setup_lock);
        return setup_json_response(input, response, size);
    }
    if (!strcmp(command, "SETUP_BEGIN")) {
        char text[256]; size_t length = 0;
        if (gc_ipc_hex_decode(argument, text, sizeof(text) - 1, &length) != 0 ||
            memchr(text, 0, length)) {
            snprintf(error, sizeof(error), "Invalid setup selection");
        } else {
            text[length] = 0;
            char *parts[5], *cursor = text;
            int valid = 1;
            for (int i = 0; i < 5; ++i) {
                parts[i] = cursor;
                char *separator = strchr(cursor, '|');
                if (i < 4) { if (!separator) { valid = 0; break; } *separator = 0; cursor = separator + 1; }
                else if (separator) valid = 0;
            }
            uint16_t vid = 0, pid = 0;
            if (!valid || parse_hex_u16(parts[1], &vid) || parse_hex_u16(parts[2], &pid) ||
                !*parts[3] || strlen(parts[3]) >= GC_PROFILE_ID_MAX ||
                strspn(parts[3], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != strlen(parts[3]) ||
                !strcmp(parts[3], ".") || !strcmp(parts[3], "..") || !strcasecmp(parts[3], "auto")) {
                snprintf(error, sizeof(error), "Enter a new profile ID using letters, numbers, dots, hyphens or underscores");
            } else {
                int slot = -1;
                controller_kind_t live_kind = CTRL_NONE;
                ScePadData live_input;
                uint32_t live_setup_buttons = 0;
                uint64_t live_input_ms = 0;
                int live_input_valid = 0;
                GcHidLayout live_hid_layout;
                uint8_t live_raw[GC_RAW_REPORT_MAX];
                size_t live_raw_size = 0;
                int live_hid_layout_valid = 0;
                memset(&live_input, 0, sizeof(live_input));
                memset(&live_hid_layout, 0, sizeof(live_hid_layout));
                /* Resolve the actual initialized reader, not the eventually
                 * consistent public state snapshot. Worker exit first clears
                 * setup_reader_active under this lock and then cancels under
                 * setup_lock, closing the disconnect/BEGIN race. */
                pthread_mutex_lock(&g_slot_lock);
                for (int i = 0; i < MAX_SLOTS; ++i) {
                    if (!g_slots[i].usb_active ||
                        !g_slots[i].setup_reader_active ||
                        g_slots[i].vid != vid || g_slots[i].pid != pid ||
                        strcmp(g_slots[i].dev_path, parts[0]) != 0)
                        continue;
                    slot = i;
                    live_kind = g_slots[i].kind;
                    live_input = g_slots[i].last_input;
                    live_setup_buttons = g_slots[i].last_setup_buttons;
                    live_input_ms = g_slots[i].last_raw_ms;
                    live_input_valid = g_slots[i].last_input_valid;
                    if (g_slots[i].hid_layout_valid &&
                        g_slots[i].last_raw_length ==
                            g_slots[i].last_raw_copied) {
                        live_hid_layout = g_slots[i].hid_layout;
                        live_raw_size = g_slots[i].last_raw_copied;
                        memcpy(live_raw, g_slots[i].last_raw_report,
                               live_raw_size);
                        live_hid_layout_valid = 1;
                    }
                    break;
                }
                pthread_mutex_unlock(&g_slot_lock);
                if (slot < 0 && !*error)
                    snprintf(error, sizeof(error),
                             "Selected controller reader is not ready. Start the service, reconnect it and refresh devices");
                const char *live_protocol = controller_kind_key(live_kind);
                GcProfile profile;
                GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES,
                                             sizeof(*profiles));
                gc_profile_defaults(&profile);
                size_t count = profiles ? gc_config_profiles(
                    g_config, profiles, GC_CONFIG_MAX_PROFILES, NULL) : 0;
                if (!profiles && !*error)
                    snprintf(error, sizeof(error),
                             "Not enough memory to inspect controller profiles");
                int base_found = !strcmp(parts[4], "identity");
                int id_found = 0;
                GcProfile exact_match;
                int exact_found = 0;
                for (size_t i = 0; i < count; ++i) {
                    if (gc_profile_matches_exact_identity(
                            &profiles[i], vid, pid, live_protocol) &&
                        (!exact_found ||
                         strcmp(profiles[i].id, exact_match.id) < 0)) {
                        exact_match = profiles[i];
                        exact_found = 1;
                    }
                    if (!strcmp(profiles[i].id, parts[3])) {
                        id_found = 1;
                        if (!gc_profile_matches_exact_identity(
                                &profiles[i], vid, pid, live_protocol)) {
                            snprintf(error, sizeof(error),
                                     "That profile ID belongs to another match. Choose a new ID");
                        } else {
                            /* Disabled exact profiles are valid editing
                             * targets and will be re-enabled on save. */
                            profile = profiles[i];
                        }
                    }
                    if (!id_found && !strcmp(profiles[i].id, parts[4]) &&
                        strcmp(parts[4], "identity")) {
                        profile = profiles[i];
                        base_found = 1;
                    }
                }
                free(profiles);
                if (!*error && exact_found &&
                    strcmp(exact_match.id, parts[3]))
                    snprintf(error, sizeof(error),
                             "This controller already uses profile %s; use that ID to reconfigure it",
                             exact_match.id);
                if (!id_found && count >= GC_CONFIG_MAX_PROFILES)
                    snprintf(error, sizeof(error), "Profile limit reached");
                if (!id_found && !base_found)
                    snprintf(error, sizeof(error),
                             "Base profile is unavailable. Refresh and select again");
                if (!*error) {
                    snprintf(profile.id, sizeof(profile.id), "%s", parts[3]);
                    if (!id_found || !profile.name[0])
                        snprintf(profile.name, sizeof(profile.name), "%s",
                                 parts[3]);
                    profile.enabled = 1;
                    profile.match_any_vid = profile.match_any_pid = 0;
                    profile.match_vid = vid; profile.match_pid = pid;
                    snprintf(profile.protocol, sizeof(profile.protocol), "%s",
                             live_protocol);
                    int reused_setup = gc_setup_rebegin(&g_setup, &profile, parts[0]);
                    ghostpad_setup_log_begin(vid, pid, profile.id, reused_setup);
                    gp_log("setup begin: path=%s slot=%d VID=%04x PID=%04x protocol=%s profile=%s reused=%d live_input=%d hid_layout=%d raw_bytes=%zu input_age_ms=%llu\n",
                           parts[0], slot, vid, pid, live_protocol, profile.id,
                           reused_setup, live_input_valid, live_hid_layout_valid,
                           live_raw_size,
                           live_input_ms && setup_now_ms() >= live_input_ms
                               ? (unsigned long long)(setup_now_ms() - live_input_ms)
                               : 0ull);
                    /* Preserve the reader's latest pre-remap state. A known
                     * neutral snapshot lets event-only XInput/DInput pads
                     * capture the very first press after arming; a held or
                     * unknown state still requires a release. */
                    if (live_input_valid) {
                        g_setup.input = live_input;
                        g_setup.input_ms = live_input_ms;
                        gc_setup_seed_buttons(&g_setup, live_setup_buttons);
                        if (live_hid_layout_valid)
                            (void)gc_setup_seed_hid(&g_setup,
                                &live_hid_layout, live_raw, live_raw_size);
                    }
                }
            }
        }
        if (*error && g_setup.active) {
            gp_log("setup begin failed: error=%s profile=%s path=%s revision=%llu\n",
                   error, g_setup.profile.id, g_setup.path,
                   (unsigned long long)g_setup.revision);
            gc_setup_cancel(&g_setup, error);
            ghostpad_setup_log_close("begin-failed");
        }
    } else if (!strcmp(command, "SETUP_ARM")) {
        int target = -1;
        int guided = !strcmp(argument, "guided");
        if (guided) target = GC_BUTTON_CROSS;
        int arm_failed = (!guided && gc_setup_target_from_name(argument, &target)) ||
            gc_setup_arm(&g_setup, target, guided, setup_now_ms());
        if (arm_failed) {
            snprintf(error, sizeof(error), "Begin setup with a connected controller first");
            gp_log("setup arm failed: requested=%s active=%d profile=%s revision=%llu\n",
                   argument, g_setup.active, g_setup.profile.id,
                   (unsigned long long)g_setup.revision);
        } else {
            int step = 0, total = 0;
            const char *stage = "review";
            gc_setup_progress(&g_setup, &step, &total, &stage);
            gp_log("setup arm: requested=%s guided=%d target=%d(%s) stage=%s step=%d/%d neutral=%d observed=0x%08x revision=%llu\n",
                   argument, guided, g_setup.target,
                   gc_setup_target_name(g_setup.target), stage ? stage : "review",
                   step, total, g_setup.neutral, g_setup.observed_buttons,
                   (unsigned long long)g_setup.revision);
        }
    } else if (!strcmp(command, "SETUP_SKIP")) {
        int skipped_target = g_setup.target;
        uint64_t before_revision = g_setup.revision;
        gc_setup_skip(&g_setup, setup_now_ms());
        gp_log("setup skip: target=%d(%s) next=%d(%s) revision=%llu->%llu message=%s\n",
               skipped_target, gc_setup_target_name(skipped_target),
               g_setup.target, gc_setup_target_name(g_setup.target),
               (unsigned long long)before_revision,
               (unsigned long long)g_setup.revision, g_setup.message);
    } else if (!strcmp(command, "SETUP_CANCEL")) {
        gp_log("setup cancel: profile=%s path=%s target=%d(%s) learned=0x%08x revision=%llu\n",
               g_setup.profile.id, g_setup.path, g_setup.target,
               gc_setup_target_name(g_setup.target), g_setup.learned,
               (unsigned long long)g_setup.revision);
        gc_setup_cancel(&g_setup, "Setup cancelled. No profile was saved.");
        ghostpad_setup_log_close("cancelled");
    } else if (!strcmp(command, "SETUP_SAVE")) {
        if (!g_setup.active) snprintf(error, sizeof(error), "Begin setup first");
        else if (g_setup.target >= 0)
            snprintf(error, sizeof(error),
                     "Finish, skip or cancel the current control capture before saving");
        else {
            GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES,
                                         sizeof(*profiles));
            size_t count = profiles ? gc_config_profiles(
                g_config, profiles, GC_CONFIG_MAX_PROFILES, NULL) : 0;
            if (!profiles)
                snprintf(error, sizeof(error),
                         "Not enough memory to inspect controller profiles");
            int replacing_profile = 0;
            for (size_t i = 0; i < count; ++i)
                if (!strcmp(profiles[i].id, g_setup.profile.id))
                    replacing_profile = 1;
            free(profiles);
            if (!replacing_profile && count >= GC_CONFIG_MAX_PROFILES)
                snprintf(error, sizeof(error), "Profile limit reached");
            GcProfile existing_match;
            if (!*error && gc_config_find_exact_profile(g_config,
                    g_setup.profile.match_vid, g_setup.profile.match_pid,
                    g_setup.profile.protocol, &existing_match, NULL) == 0 &&
                strcmp(existing_match.id, g_setup.profile.id) != 0)
                snprintf(error, sizeof(error),
                         "This controller already uses profile %s; re-run setup using that ID",
                         existing_match.id);
            /* Resolve and validate the live setup reader before touching disk.
             * A path may be reused after disconnect, so VID:PID must match too. */
            controller_kind_t setup_kind = CTRL_NONE;
            uint8_t setup_in = 0, setup_out = 0;
            int setup_interface_number = -1;
            if (!*error) {
                pthread_mutex_lock(&g_slot_lock);
                for (int slot = 0; slot < MAX_SLOTS; ++slot) {
                    if (!g_slots[slot].usb_active ||
                        !g_slots[slot].setup_reader_active ||
                        strcmp(g_slots[slot].dev_path, g_setup.path) != 0 ||
                        g_slots[slot].vid != g_setup.profile.match_vid ||
                        g_slots[slot].pid != g_setup.profile.match_pid)
                        continue;
                    setup_kind = g_slots[slot].kind;
                    setup_in = g_slots[slot].input_endpoint;
                    /* Persist only endpoints consumed by that protocol. DS3,
                     * Logitech and generic DInput use no interrupt OUT path. */
                    if (setup_kind == CTRL_NINTENDO ||
                        setup_kind == CTRL_XBOX_GIP ||
                        setup_kind == CTRL_XUSB || setup_kind == CTRL_DS4)
                        setup_out = g_slots[slot].output_endpoint;
                    setup_interface_number =
                        g_slots[slot].interface_number;
                    break;
                }
                pthread_mutex_unlock(&g_slot_lock);
                if (setup_kind == CTRL_NONE)
                    snprintf(error, sizeof(error),
                             "Controller disconnected before save. Reconnect it and keep setup open");
            }

            if (!*error) {
                const char *setup_protocol = controller_kind_key(setup_kind);
                if (strcmp(g_setup.profile.protocol, setup_protocol) != 0) {
                    snprintf(error, sizeof(error),
                             "Controller transport changed during setup; reconnect and begin again");
                } else if (setup_kind == CTRL_BT || setup_kind == CTRL_BT_LE) {
                    /* Bluetooth admission is owned by the in-core Ghosttooth
                     * engine, so there is no USB controller rule to persist.
                     * The learned profile is still an exact transport match
                     * (bt/ble + VID:PID) and is selected on reconnect. */
                    if (gc_config_save_profile(g_config, &g_setup.profile,
                            error, sizeof(error)) == 0) {
                        uint64_t saved_generation = gc_config_generation(g_config);
                        gp_log("setup save: bluetooth profile=%s path=%s VID=%04x PID=%04x protocol=%s learned_buttons=0x%08x learned_axes=0x%08x generation=%llu\n",
                               g_setup.profile.id, g_setup.path,
                               g_setup.profile.match_vid, g_setup.profile.match_pid,
                               setup_protocol, g_setup.learned,
                               g_setup.learned_axes,
                               (unsigned long long)saved_generation);
                        ghostpad_setup_log_profile(&g_setup.profile);
                        gc_state_set_generation(g_state, saved_generation);
                        gc_setup_cancel(&g_setup,
                            "Bluetooth profile saved. Reloading with Automatic matching.");
                        ghostpad_setup_log_close("saved");
                        restart_after_save = 1;
                    }
                } else {
                    GcControllerRule learned_rule;
                    if (gc_config_find_controller_including_disabled(g_config,
                            g_setup.profile.match_vid,
                            g_setup.profile.match_pid,
                            &learned_rule, NULL) != 0) {
                        memset(&learned_rule, 0, sizeof(learned_rule));
                        GcControllerRule all_rules[GC_CONFIG_MAX_CONTROLLERS];
                        size_t rule_count = gc_config_controllers(g_config,
                            all_rules, GC_CONFIG_MAX_CONTROLLERS, NULL);
                        for (unsigned suffix = 0;; ++suffix) {
                            if (suffix == 0)
                                snprintf(learned_rule.id,
                                    sizeof(learned_rule.id),
                                    "auto-%04x-%04x-%s",
                                    g_setup.profile.match_vid,
                                    g_setup.profile.match_pid,
                                    setup_protocol);
                            else
                                snprintf(learned_rule.id,
                                    sizeof(learned_rule.id),
                                    "auto-%04x-%04x-%s-%u",
                                    g_setup.profile.match_vid,
                                    g_setup.profile.match_pid,
                                    setup_protocol, suffix);
                            int collision = 0;
                            for (size_t index = 0; index < rule_count; ++index)
                                if (!strcmp(all_rules[index].id,
                                            learned_rule.id)) {
                                    collision = 1;
                                    break;
                                }
                            if (!collision || suffix > rule_count)
                                break;
                        }
                    }
                    snprintf(learned_rule.name, sizeof(learned_rule.name),
                             "%s", g_setup.profile.name);
                    learned_rule.enabled = 1;
                    learned_rule.ignore = 0;
                    learned_rule.vid = g_setup.profile.match_vid;
                    learned_rule.pid = g_setup.profile.match_pid;
                    snprintf(learned_rule.protocol,
                             sizeof(learned_rule.protocol), "%s",
                             setup_protocol);
                    learned_rule.preferred_in_endpoint = setup_in;
                    learned_rule.preferred_out_endpoint = setup_out;
                    learned_rule.preferred_interface_number =
                        setup_interface_number;

                    if (gc_config_save_setup(g_config, &g_setup.profile,
                            &learned_rule, error, sizeof(error)) == 0) {
                        uint64_t saved_generation = gc_config_generation(g_config);
                        gp_log("setup save: profile=%s rule=%s path=%s VID=%04x PID=%04x protocol=%s iface=%d IN=0x%02x OUT=0x%02x learned_buttons=0x%08x learned_axes=0x%08x generation=%llu\n",
                               g_setup.profile.id, learned_rule.id, g_setup.path,
                               g_setup.profile.match_vid, g_setup.profile.match_pid,
                               setup_protocol, setup_interface_number, setup_in,
                               setup_out, g_setup.learned, g_setup.learned_axes,
                               (unsigned long long)saved_generation);
                        ghostpad_setup_log_profile(&g_setup.profile);
                        gc_state_set_generation(g_state, saved_generation);
                        gc_setup_cancel(&g_setup,
                            "Profile and controller rule saved. Reloading with Automatic matching.");
                        ghostpad_setup_log_close("saved");
                        restart_after_save = 1;
                    }
                }
            }
        }
    } else if (strcmp(command, "SETUP_STATUS")) snprintf(error, sizeof(error), "Unknown setup action");
    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "ok", json_object_new_boolean(!*error));
    if (*error) json_object_object_add(root, "error", json_object_new_string(error));
    if (*warning) json_object_object_add(root, "warning", json_object_new_string(warning));
    json_object_object_add(root, "active", json_object_new_boolean(g_setup.active));
    json_object_object_add(root, "message", json_object_new_string(g_setup.message));
    json_object_object_add(root, "profile", json_object_new_string(g_setup.profile.id));
    json_object_object_add(root, "path", json_object_new_string(g_setup.path));
    json_object_object_add(root, "target", json_object_new_int(g_setup.target));
    json_object_object_add(root, "revision", json_object_new_int64((int64_t)g_setup.revision));
    struct json_object *mappings = json_object_new_array();
    if (g_setup.profile.id[0]) for (int i = 0; i < GC_BUTTON_COUNT; ++i) {
        uint32_t mask = g_setup.profile.button_targets[i];
        struct json_object *mapping = json_object_new_object();
        json_object_object_add(mapping, "source", json_object_new_string(gc_button_name((GcButton)i)));
        char targets[256] = "";
        for (int j = 0; j < GC_BUTTON_COUNT; ++j) if (mask & gc_button_mask((GcButton)j)) {
            if (*targets) strncat(targets, " + ", sizeof(targets) - strlen(targets) - 1);
            strncat(targets, gc_button_name((GcButton)j), sizeof(targets) - strlen(targets) - 1);
        }
        json_object_object_add(mapping, "target", json_object_new_string(*targets ? targets : "Disabled"));
        json_object_array_add(mappings, mapping);
    }
    if (g_setup.profile.id[0]) for (int target = 0; target < GC_BUTTON_INPUT15; ++target) {
        const GcButtonPatternBinding *pattern = &g_setup.profile.button_patterns[target];
        if (!pattern->present)
            continue;
        char sources[256] = "Pattern: ";
        for (int source = 0; source < GC_BUTTON_COUNT; ++source) {
            if (!(pattern->source_mask & gc_button_mask((GcButton)source)))
                continue;
            if (strlen(sources) > strlen("Pattern: "))
                strncat(sources, " + ", sizeof(sources) - strlen(sources) - 1);
            strncat(sources, gc_button_name((GcButton)source),
                    sizeof(sources) - strlen(sources) - 1);
        }
        struct json_object *mapping = json_object_new_object();
        json_object_object_add(mapping, "source", json_object_new_string(sources));
        json_object_object_add(mapping, "target",
                               json_object_new_string(gc_button_name((GcButton)target)));
        json_object_array_add(mappings, mapping);
    }
    json_object_object_add(root, "mappings", mappings);
    pthread_mutex_unlock(&g_setup_lock);
    if (restart_after_save) {
        int restart_result = service_restart();
        json_object_object_add(root, "service_restarted",
                               json_object_new_boolean(restart_result == 0));
        if (restart_result != 0)
            json_object_object_add(root, "restart_warning", json_object_new_string(
                "Profile was saved, but the controller service did not restart. Restart it manually."));
    }
    int result = setup_json_response(root, response, size);
    return *error ? -1 : result;
}

static int ipc_dualsense_fx_status(char *response, size_t response_size)
{
    /* Report saved preferences separately from a live, normal-API route.
     * Until the app-pad integration is available no output is claimed. */
    GcDualSenseFxStatus status = {0};
    GcRuntimeConfig runtime;
    if (gc_config_snapshot(g_config, &runtime, NULL) != 0) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"runtime_supported\":false,\"error\":\"configuration_unavailable\"}\n");
        return -1;
    }
    {
        status.enabled = runtime.dualsense_fx_enabled;
        status.effect = runtime.dualsense_fx_effect;
        status.period_ms = runtime.dualsense_fx_period_ms;
        status.step_ms = runtime.dualsense_fx_step_ms;
        status.r = runtime.dualsense_fx_r;
        status.g = runtime.dualsense_fx_g;
        status.b = runtime.dualsense_fx_b;
    }
    return gc_runtime_command_fx_status(&status, response, response_size);
}

static int ipc_console_id(char *response, size_t response_size)
{
    GcConsoleId id;
    struct json_object *root = json_object_new_object();
    const int available = gc_console_id_read(&id) == 0;
    json_object_object_add(root, "ok", json_object_new_boolean(available));
    json_object_object_add(root, "serial", json_object_new_string(
        available && (id.flags & GC_CONSOLE_ID_HAS_SERIAL) ? id.serial : "Unavailable"));
    json_object_object_add(root, "model", json_object_new_string(
        available && (id.flags & GC_CONSOLE_ID_HAS_MODEL) ? id.model : "Unavailable"));
    char openpsid[33] = "", system[33] = "";
    if (available && (id.flags & GC_CONSOLE_ID_HAS_OPENPSID))
        gc_console_id_hex(id.openpsid, openpsid);
    if (available && (id.flags & GC_CONSOLE_ID_HAS_OPENPSID_SYS))
        gc_console_id_hex(id.openpsid_sys, system);
    json_object_object_add(root, "openpsid", json_object_new_string(
        openpsid[0] ? openpsid : "Unavailable"));
    json_object_object_add(root, "openpsid_system", json_object_new_string(
        system[0] ? system : "Unavailable"));
    return setup_json_response(root, response, response_size);
}

static int ipc_dualsense_rumble(const char *argument, char *response, size_t response_size)
{
    /* Parent will supply only a frozen normal-API app-pad sink. Never route
     * an application request through the legacy remote-pad worker. */
    return gc_runtime_command_rumble(argument, NULL, response, response_size);
}

static int ipc_command_impl(void *context, const char *command, const char *argument,
                       char *response, size_t response_size)
{
    (void)context;
    (void)argument;
    int result = 0;
    char error[256] = "";
    if (!strcmp(command, "DEVICES")) return ipc_devices(response, response_size);
    if (!strcmp(command, "BLUETOOTH_STATUS"))
        return ipc_bluetooth_status(response, response_size);
    if (!strncmp(command, "BLUETOOTH_", 10))
        return ipc_bluetooth_command(command, argument, response, response_size);
    if (!strcmp(command, "DUALSENSE_FX_STATUS")) return ipc_dualsense_fx_status(response, response_size);
    if (!strcmp(command, "CONSOLE_ID")) return ipc_console_id(response, response_size);
    if (!strcmp(command, "DUALSENSE_RUMBLE"))
        return ipc_dualsense_rumble(argument, response, response_size);
    if (!strcmp(command, "DUALSENSE_RUMBLE_STOP")) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"runtime_supported\":false,\"applied\":false,\"error\":\"effect_route_unavailable\"}\n");
        return -1;
    }
    if (!strncmp(command, "SETUP_", 6)) return ipc_setup(command, argument, response, response_size);
    if (strcmp(command, "PING") == 0) {
        result = 0;
    } else if (strcmp(command, "START") == 0) {
        result = service_start();
    } else if (strcmp(command, "STOP") == 0) {
        result = service_stop();
    } else if (strcmp(command, "RESTART") == 0 ||
               strcmp(command, "REBIND") == 0 ||
               strcmp(command, "FORCE_VDA") == 0) {
        result = service_restart();
    } else if (strcmp(command, "RELOAD") == 0 && g_config) {
        result = gc_config_reload(g_config, error, sizeof(error));
        if (result == 0)
            gc_state_set_generation(g_state, gc_config_generation(g_config));
        else
            gc_state_set_service(g_state, GC_SERVICE_ERROR, error);
    } else if (strcmp(command, "LOG_TAIL") == 0) {
        return ipc_log_tail(argument, response, response_size);
    } else if (strcmp(command, "TOAST") == 0) {
        char message[768];
        size_t length = 0;
        if (gc_ipc_hex_decode(argument, message, sizeof(message) - 1,
                              &length) != 0) {
            snprintf(response, response_size,
                     "{\"ok\":false,\"error\":\"invalid_toast\"}\n");
            return -1;
        }
        message[length] = '\0';
        notify("%s", message);
        snprintf(response, response_size,
                 "{\"ok\":true,\"command\":\"TOAST\"}\n");
        return 0;
    } else if (strcmp(command, "PROFILES") == 0 && g_config) {
        return ipc_profiles(response, response_size);
    } else if (strcmp(command, "CONTROLLERS") == 0 && g_config) {
        return ipc_controllers(response, response_size);
    } else if (strcmp(command, "CONTROLLER_SAVE") == 0 && g_config) {
        return ipc_controller_save(argument, response, response_size);
    } else if (strcmp(command, "PROFILE_GET") == 0 && g_config) {
        return ipc_profile_get(argument, response, response_size);
    } else if (strcmp(command, "PROFILE_SAVE") == 0 && g_config) {
        return ipc_profile_save(argument, response, response_size);
    } else if (strcmp(command, "PROFILE_RENAME") == 0 && g_config) {
        return ipc_profile_rename(argument, response, response_size);
    } else if ((strcmp(command, "PROFILE_CREATE") == 0 ||
                strcmp(command, "PROFILE_IMPORT") == 0 ||
                strcmp(command, "PROFILE_EXPORT") == 0) && g_config) {
        return ipc_profile_operation(command, argument, response,
                                     response_size);
    } else if (strcmp(command, "PROFILE_EDIT") == 0 && g_config) {
        return ipc_profile_edit(argument, response, response_size);
    } else if (strcmp(command, "PROFILE_FILES") == 0 && g_config) {
        return gc_config_profile_files_json(g_config, response,
                                            response_size);
    } else if (strcmp(command, "RUNTIME_GET") == 0 && g_config) {
        return ipc_runtime_get(response, response_size);
    } else if (strcmp(command, "RUNTIME_SET") == 0 && g_config) {
        return ipc_runtime_set(argument, response, response_size);
    } else if (strcmp(command, "UPDATE_CHECK") == 0) {
        return ipc_update_check(response, response_size);
    } else if (strcmp(command, "UPDATE_DOWNLOAD") == 0) {
        return ipc_update_download(response, response_size);
    } else if (strcmp(command, "UPDATE_ROLLBACK") == 0) {
        return ipc_update_rollback(response, response_size);
    } else {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"unknown_command\"}\n");
        return -1;
    }
    if (result == 0) {
        snprintf(response, response_size,
                 "{\"ok\":true,\"command\":\"%s\"}\n", command);
    } else {
        snprintf(response, response_size,
                 "{\"ok\":false,\"error\":\"%s\"}\n",
                 error[0] ? error : "operation_failed");
    }
    return result;
}

static int ipc_command(void *context, const char *command, const char *argument,
                       char *response, size_t response_size)
{
    int result = ipc_command_impl(context, command, argument, response, response_size);
    /* Diagnostic: the app's read-poll commands are logged too (thinned 1-in-8
     * so the log stays readable), because a run that never reaches the
     * service must be provable from this log alone. Writes log every time. */
    static unsigned poll_ticks = 0;
    const int is_poll = !strcmp(command, "RUNTIME_GET") || !strcmp(command, "PROFILE_GET") ||
                        !strcmp(command, "PROFILES") || !strcmp(command, "CONTROLLERS") ||
                        !strcmp(command, "LOG_TAIL") || !strcmp(command, "PING") ||
                        !strcmp(command, "DEVICES") || !strcmp(command, "DUALSENSE_FX_STATUS") ||
                        !strcmp(command, "BLUETOOTH_STATUS") || !strcmp(command, "CONSOLE_ID") ||
                        !strcmp(command, "SETUP_STATUS") || !strcmp(command, "SETUP_INPUT") ||
                        !strcmp(command, "STATUS");
    if (is_poll)
    {
        if ((++poll_ticks % 8) == 0)
            gp_log("settings: %s %s (poll)\n", command, result == 0 ? "ok" : "failed");
    }
    else if (argument && argument[0])
    {
        gp_log("settings: %s %s arg=%.120s\n", command,
               result == 0 ? "ok" : "failed", argument);
    }
    else
    {
        gp_log("settings: %s %s\n", command, result == 0 ? "ok" : "failed");
    }
    return result;
}

/* A signal handler only requests shutdown. USB/VDA teardown stays in worker
 * context, where locks, ioctls and heap operations are safe. */
static void request_shutdown(int signal_number)
{
    (void)signal_number;
    g_signal_shutdown = 1;
}

/* Watches the shutdown: if this process is still running 20 s after a signal,
 * the clean path is wedged in an unkillable kernel call. Free the singleton
 * locks so a redeploy can proceed even though this process cannot exit. */
static void *shutdown_watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        if (!g_signal_shutdown)
            continue;
        usleep(20 * 1000 * 1000);
        if (g_pid_fd >= 0)
            flock(g_pid_fd, LOCK_UN);
        gc_singleton_release_secondary();
        _exit(0);
    }
    return NULL;
}

extern const unsigned char gc_icon_start[];
extern const unsigned char gc_icon_end[];

static int publish_xmb_icon_to(const char *icon_path)
{
    const size_t icon_size = (size_t)(gc_icon_end - gc_icon_start);
    struct stat info;
    if (lstat(icon_path, &info) == 0) {
        if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {
            errno = EINVAL;
            return -1;
        }
        if ((size_t)info.st_size == icon_size) {
            int existing = open(icon_path, O_RDONLY | O_NOFOLLOW);
            if (existing >= 0) {
                unsigned char buffer[4096];
                size_t offset = 0;
                while (offset < icon_size) {
                    size_t wanted = icon_size - offset;
                    if (wanted > sizeof(buffer))
                        wanted = sizeof(buffer);
                    ssize_t count = read(existing, buffer, wanted);
                    if (count <= 0 ||
                        memcmp(buffer, gc_icon_start + offset,
                               (size_t)count) != 0)
                        break;
                    offset += (size_t)count;
                }
                close(existing);
                if (offset == icon_size)
                    return 0;
            }
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    char temporary[256];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp.%d",
                          icon_path, (int)getpid());
    if (length <= 0 || (size_t)length >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    size_t offset = 0;
    while (offset < icon_size) {
        ssize_t count = write(fd, gc_icon_start + offset, icon_size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += (size_t)count;
    }
    int result = offset == icon_size && fsync(fd) == 0 ? 0 : -1;
    if (close(fd) != 0)
        result = -1;
    fd = -1;
    if (result == 0 && rename(temporary, icon_path) != 0)
        result = -1;
    if (result != 0) {
        int saved = errno;
        unlink(temporary);
        errno = saved;
    }
    return result;
}

/* Publish the icon under both names the Home tile may reference. The primary
 * name decides success; the alias is best effort. */
static int publish_xmb_icon(void)
{
    int primary = publish_xmb_icon_to(XMB_ICON_PATH);
    int saved = errno;
    if (publish_xmb_icon_to(XMB_ICON_ALIAS_PATH) != 0)
        gp_log("XMB icon alias publish failed: %s\n", strerror(errno));
    if (primary != 0)
        errno = saved;
    return primary;
}

/* ── main ─────────────────────────────────────────────────────────────── */

/* Routes takeover log lines through the payload's logger so the singleton
 * module itself stays free of PS5 headers. */
static void gc_singleton_log(void *context, const char *message)
{
    (void)context;
    gp_log("%s\n", message);
}

int main(void) {
    int ret;
    GcRuntimeConfig runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.autostart = 1;
    runtime.ipc_port = GC_IPC_PORT;

    elevate_credentials();

    /* Open this instance's run log before the takeover so a failed eviction
     * attempt is never silent: gc_singleton_log routes through gp_log. */
    ghostpad_status_log_reset();
    gp_log("startup: pid=%d attempting singleton takeover\n", (int)getpid());

    /* Acquire ownership before touching shared config, logs, state, or icon.
     * A redeploy loads a second ELF while the first still owns the
     * controllers; evict that instance (SIGTERM, then SIGKILL) and take the
     * singleton instead of silently doing nothing, so redeploying never needs
     * a console reboot. If the lock truly cannot be taken we must not run a
     * second instance, so leave without touching shared state. */
    if (gc_ensure_real_directory_tree(GC_DATA_ROOT "/runtime", 0755) != 0)
        return 1;
    g_pid_fd = gc_open_private_regular(PID_PATH, O_RDWR, 1);
    if (g_pid_fd < 0)
        return 1;
    if (gc_singleton_takeover(g_pid_fd, PID_PATH, GC_SINGLETON_SETTLE_US,
                              gc_singleton_log, NULL) != 0) {
        close(g_pid_fd);
        g_pid_fd = -1;
        return 0;
    }

    char config_error[256] = "";
    g_config = gc_config_open(NULL, config_error, sizeof(config_error));
    gp_log("Ghost-Control v5 starting — %d slots\n", MAX_SLOTS);
    if (g_config) {
        uint64_t generation;
        (void)gc_config_snapshot(g_config, &runtime, &generation);
        gp_log("config ready root=%s generation=%llu ipc_port=%u\n",
               gc_config_root(g_config), (unsigned long long)generation,
               (unsigned)runtime.ipc_port);
    } else {
        gp_log("config ERROR: %s\n", config_error);
        flock(g_pid_fd, LOCK_UN);
        close(g_pid_fd);
        g_pid_fd = -1;
        return 1;
    }
    if (publish_xmb_icon() != 0)
        gp_log("XMB icon publish ERROR: %s\n", strerror(errno));

    /* The pid file was written by gc_singleton_takeover when ownership was
     * claimed, before any shared state was touched. */

    char state_error[256] = "";
    g_state = gc_state_open(GC_DATA_ROOT, GC_VERSION,
                            state_error, sizeof(state_error));
    if (g_state && g_config)
        gc_state_set_generation(g_state, gc_config_generation(g_config));
    if (!g_state)
        gp_log("state ERROR: %s\n", state_error);
    if (!g_state) {
        gc_config_close(g_config);
        g_config = NULL;
        flock(g_pid_fd, LOCK_UN);
        close(g_pid_fd);
        g_pid_fd = -1;
        return 1;
    }

    /* Init slots */
    for (int s = 0; s < MAX_SLOTS; s++)
        reset_slot_record(&g_slots[s]);
    for (int s = 0; s < GC_BT_MAX_PADS; s++)
        g_bt_gcslot[s] = -1;
    atomic_store(&g_assign_slot, -1);

    signal(SIGTERM, request_shutdown);
    signal(SIGINT, request_shutdown);
    {
        pthread_t watchdog_tid;
        if (pthread_create(&watchdog_tid, NULL, shutdown_watchdog, NULL) == 0)
            pthread_detach(watchdog_tid);
    }

    sceUserServiceInitialize(NULL);
    notify("Ghost-toothAPI ready — plug in controllers now");

    ret=scePadInit(); gp_log("scePadInit: 0x%08x\n", ret);
    ret=scePadSetProcessPrivilege(1); gp_log("scePadSetProcessPrivilege: 0x%08x\n", ret);

    /* klog is taken on demand, per VDA insert — never held for the process
     * lifetime (it is a single-reader device). */
    if (gc_dualsense_fx_start(g_config) != 0)
        gp_log("DualSense FX worker failed to start\n");

    GcGhostpadOps ghostpad_ops = {
        NULL, ghostpad_connected, ghostpad_input, ghostpad_disconnected
    };
    g_ghostpad = gc_ghostpad_start(&ghostpad_ops);
    if (!g_ghostpad)
        gp_log("GhostPad network service failed to initialize\n");
    else
        gc_ghostpad_configure(g_ghostpad, runtime.ghostpad_port,
                              runtime.ghostpad_stale_ms);

    (void)gc_ensure_real_directory_tree(GC_DATA_ROOT "/bt/maps", 0755);
    {
        GcBtOps bt_ops = { NULL, bt_ready, bt_report, bt_state, bt_lost };
        g_bt = gc_bt_start(&bt_ops, GC_DATA_ROOT "/bt/controllers.db");
    }
    if (!g_bt)
        gp_log("Bluetooth input source failed to initialize\n");
    else
        gp_log("Bluetooth input source started\n");

    /* The old global handle sweep deleted virtual devices owned by unrelated
     * payloads. Ghostcontrol now deletes only handles stored in its own slots. */
    pthread_t mgr_tid;
    int manager_started = 0;
    if (pthread_create(&mgr_tid, NULL, controller_manager_thread, NULL)==0) {
        manager_started = 1;
        gp_log("Manager thread started\n");
    } else {
        gc_state_set_service(g_state, GC_SERVICE_ERROR,
                             "controller manager thread failed to start");
    }

    if (runtime.autostart)
        (void)service_start();
    else
        gc_state_set_service(g_state, GC_SERVICE_INACTIVE, NULL);

    GcIpcOps ipc_ops = {g_state, ipc_status, ipc_command, ipc_log_line};
    char ipc_error[256] = "";
    g_ipc = gc_ipc_start(runtime.ipc_port, GC_DATA_ROOT, &ipc_ops,
                         ipc_error, sizeof(ipc_error));
    if (g_ipc)
#ifdef GC_IPC_BIND_LAN
        gp_log("IPC ready 0.0.0.0:%u (LAN bind) token=%s\n", (unsigned)runtime.ipc_port,
               gc_ipc_token_path(g_ipc));
#else
        gp_log("IPC ready 127.0.0.1:%u token=%s\n", (unsigned)runtime.ipc_port,
               gc_ipc_token_path(g_ipc));
#endif
    else {
        gp_log("IPC ERROR: %s\n", ipc_error);
        gc_state_set_service(g_state, GC_SERVICE_ERROR, ipc_error);
    }

    /* Keep-alive */
    uint32_t tick = 0;
    while (!g_signal_shutdown) {
        usleep(1000000);
        tick++;
        if (tick % 10 == 0) {
            int active = active_slot_count();
            for (int slot = 0; slot < MAX_SLOTS; ++slot) {
                pthread_mutex_lock(&g_slot_lock);
                int slot_active = g_slots[slot].usb_active;
                pthread_mutex_unlock(&g_slot_lock);
                if (slot_active)
                    publish_slot_state(slot, NULL, NULL, -1, -2);
            }
            gp_log("alive tick=%u active_slots=%d\n", tick, active);
        }
    }

    /* Release the singleton locks before the risky USB/VDA teardown: a wedged
     * teardown (unkillable kernel call) must never block the next deploy. */
    if (g_pid_fd >= 0) {
        flock(g_pid_fd, LOCK_UN);
        gp_log("shutdown: singleton lock released before teardown\n");
    }
    gc_singleton_release_secondary();
    gc_ipc_stop(g_ipc);
    gc_dualsense_fx_stop();
    int stop_result = service_stop();
    gc_ghostpad_stop(g_ghostpad);
    g_ghostpad = NULL;
    gc_bt_stop(g_bt);
    g_bt = NULL;
    for (int s = 0; s < MAX_SLOTS; ++s) {
        pthread_mutex_lock(&g_slot_lock);
        int32_t bt_handle = (g_slots[s].kind == CTRL_BT ||
                             g_slots[s].kind == CTRL_BT_LE)
                            ? g_slots[s].handle : -1;
        if (bt_handle >= 0) g_slots[s].usb_active = 0;
        pthread_mutex_unlock(&g_slot_lock);
        if (bt_handle >= 0)
            (void)release_vda_for_slot(s, bt_handle);
    }
    /* Keep the manager alive during service_stop so it can finish any VDA
     * delete quarantine. Only then ask the manager itself to exit. */
    atomic_store(&g_shutdown, 1);
    if (manager_started)
        pthread_join(mgr_tid, NULL);
    vda_klog_shutdown();
    if (stop_result == 0 && active_slot_count() == 0) {
        gc_state_set_service(g_state, GC_SERVICE_INACTIVE, NULL);
        gc_config_close(g_config);
        gc_state_close(g_state);
        g_config = NULL;
        g_state = NULL;
    } else {
        /* Do not free stores still reachable by detached USB workers. Process
         * teardown reclaims them after the OS terminates all threads. */
        gp_log("shutdown: retaining shared stores for active workers\n");
    }
    flock(g_pid_fd, LOCK_UN);
    close(g_pid_fd);
    return 0;
}

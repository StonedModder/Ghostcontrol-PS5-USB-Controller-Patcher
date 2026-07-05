/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ghost-Control v5 by StonedModder: Multi-controller support
 * USB HID controllers → virtual DualSense devices on PS5
 *
 * Patch Manba V2 NBJr:
 *   - PC/XInput mode                (VID=045E PID=028E)
 *   - Switch USB mode               (VID=057E PID=2009)
 *
 * Each detected controller gets its own VDA device + force_bind
 * assignment dialog + VDI injection thread.
 * Hotplug: plug in any time, disconnect any time.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#endif

#include "shellui_pad.h"
#include "gc_types.h"
#include "usb_helpers.h"
#include "controller_nintendo.h"
#include "controller_xbox.h"
#include "controller_ds4.h"
#include "controller_mamba.h"

/* ── Logging ──────────────────────────────────────────────────────────── */
#define LOG_DIR  "/data/ghostpad"
#define LOG_PATH "/data/ghostpad/gc_status.log"
#define PID_PATH "/data/ghostpad/gc_main.pid"
#define LOG_MAX  480

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_log_fd = -1;

void ghostpad_status_log_reset(void) {
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
    mkdir(LOG_DIR, 0755);
    g_log_fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    pthread_mutex_unlock(&g_log_lock);
}

void ghostpad_status_log(const char *fmt, ...) {
    char buf[LOG_MAX]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf)-1, fmt, ap); va_end(ap);
    buf[LOG_MAX-1] = '\0';
    klog_printf("%s", buf);
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) {
        size_t n = strnlen(buf, sizeof(buf));
        write(g_log_fd, buf, n);
        if (n && buf[n-1] != '\n') write(g_log_fd, "\n", 1);
    }
    pthread_mutex_unlock(&g_log_lock);
}
#define gp_log(...) ghostpad_status_log("[GC] " __VA_ARGS__)

/* ── SCE stubs ────────────────────────────────────────────────────────── */
extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *outUserId);
extern int32_t sceUserServiceGetForegroundUser(int32_t *outUserId);
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadGetHandle(int32_t userId, int32_t type, int32_t index);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t deviceType);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *padData);
extern int32_t sceKernelSendNotificationRequest(int unk0, void *req, size_t size, int unk1);

#define VIRTUAL_DEVICE_TYPE_DUALSENSE 3

/* ── Multi-controller slots ───────────────────────────────────────────── */
#define MAX_SLOTS 4

typedef struct {
    volatile int32_t handle;        /* VDA handle, -1 = slot free */
    volatile int     vdi_ready;
    volatile int     usb_active;    /* USB reader thread is running */
    volatile int     release_requested;
    volatile int     released_pause;
    volatile int     release_wait_neutral;
    volatile int     confirmed;     /* user pressed a button — assignment done */
    volatile int     usb_fd;        /* open ugen fd, -1 if none — for clean teardown */
    char             dev_path[32];  /* ugen path claimed by this slot */
    uint16_t         vid, pid;
    volatile uint64_t virtual_dev_id;
    volatile uint64_t evicted_physical_dev;
    volatile int      physical_evict_done;
    volatile uint32_t inject_count;
} ctrl_slot_t;

static ctrl_slot_t     g_slots[MAX_SLOTS];
static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t         g_inject_uid = 0x10000000;
static volatile uint64_t g_last_physical_pad_dev = 0;
static volatile uint64_t g_pending_physical_recover_dev = 0;
static volatile uint64_t g_recovered_physical_pad_dev = 0;
static volatile int      g_physical_recover_attempts = 0;
static volatile int      g_physical_recover_last_scan = -1000;
static uint64_t          g_virtual_id_history[32];
static unsigned          g_virtual_id_history_w = 0;

/* Assignment serialization: only ONE controller may show its assignment
 * dialog at a time. g_assign_slot = slot awaiting user confirmation, or -1.
 * Without this, multiple dialogs stack and all bind to the same user. */
static volatile int    g_assign_slot = -1;

/* ── klog device-ID queue ─────────────────────────────────────────────── */
#define KLOG_QSIZE 16
static uint64_t        g_klog_q[KLOG_QSIZE];
static int             g_klog_qw = 0, g_klog_qr = 0;
static pthread_mutex_t g_klog_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint64_t dev_id;
    int32_t  handle;
    int32_t  open_type;
} klog_open_pad_t;

#define KLOG_OPEN_PAD_COUNT 16
static klog_open_pad_t g_klog_open_pad[KLOG_OPEN_PAD_COUNT];
static unsigned        g_klog_open_pad_w = 0;

static void klog_open_pad_store(uint64_t dev_id, int32_t open_type, int32_t handle);
static int32_t klog_find_open_pad_handle(uint64_t dev_id);
static int32_t klog_wait_open_pad_handle(uint64_t dev_id, int ms);
static uint64_t klog_find_physical_open_pad(uint64_t virtual_dev_id);
static int is_our_virtual_device_id(uint64_t dev_id);
static void remember_virtual_device_id(uint64_t dev_id);
static int any_mamba_slot_active(void);
static void remember_physical_pad_for_recovery(uint64_t dev_id, const char *reason);
static void physical_recovery_tick(int scan);

static void maybe_stop_mamba_for_same_user_physical_open(uint64_t dev_id,
                                                         int32_t open_handle,
                                                         const char *line);
static void klog_enqueue(uint64_t id) {
    pthread_mutex_lock(&g_klog_lock);
    int next = (g_klog_qw + 1) % KLOG_QSIZE;
    if (next != g_klog_qr) { g_klog_q[g_klog_qw] = id; g_klog_qw = next; }
    pthread_mutex_unlock(&g_klog_lock);
}

static uint64_t klog_dequeue_ms(int ms) {
    for (int t = 0; t < ms; t += 100) {
        pthread_mutex_lock(&g_klog_lock);
        if (g_klog_qw != g_klog_qr) {
            uint64_t id = g_klog_q[g_klog_qr];
            g_klog_qr = (g_klog_qr + 1) % KLOG_QSIZE;
            pthread_mutex_unlock(&g_klog_lock);
            return id;
        }
        pthread_mutex_unlock(&g_klog_lock);
        usleep(100000);
    }
    return 0;
}

/* ── Notification ─────────────────────────────────────────────────────── */
typedef struct { char _unk[45]; char message[3075]; } NotifyRequest;
static void notify(const char *fmt, ...) {
    NotifyRequest req; va_list ap;
    memset(&req, 0, sizeof(req));
    va_start(ap, fmt); vsnprintf(req.message, sizeof(req.message), fmt, ap); va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ── klog capture thread ──────────────────────────────────────────────── */
static uint64_t parse_hex_str(const char *s) {
    uint64_t v = 0;
    while (*s) {
        char c = *s++;
        if      (c >= '0' && c <= '9') v = (v<<4)|(c-'0');
        else if (c >= 'a' && c <= 'f') v = (v<<4)|(c-'a'+10);
        else if (c >= 'A' && c <= 'F') v = (v<<4)|(c-'A'+10);
        else break;
    }
    return v;
}

static uint64_t parse_uint_auto(const char *s, const char **endp, int *ok) {
    uint64_t v = 0;
    int base = 10;
    int got = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    while (*s) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = (v * (uint64_t)base) + (uint64_t)d;
        got = 1;
        s++;
    }

    if (endp) *endp = s;
    if (ok) *ok = got;
    return v;
}

static int parse_open_pad_line(const char *line, uint64_t *out_dev_id,
                               int32_t *out_open_type, int32_t *out_open_index,
                               int32_t *out_handle) {
    const char *open = strstr(line, "Open Pad [");
    if (!open)
        return 0;

    const char *p = open + 10;
    int ok_dev = 0;
    uint64_t dev_id = parse_uint_auto(p, &p, &ok_dev);
    if (!ok_dev)
        return 0;

    const char *comma = strchr(p, ',');
    if (!comma)
        return 0;
    comma++;

    int ok_type = 0;
    uint64_t open_type = parse_uint_auto(comma, &p, &ok_type);
    if (!ok_type || open_type > 0x7fffffffU)
        return 0;

    comma = strchr(p, ',');
    if (!comma)
        return 0;
    comma++;

    int ok_index = 0;
    uint64_t open_index = parse_uint_auto(comma, &p, &ok_index);
    if (!ok_index || open_index > 0x7fffffffU)
        return 0;

    const char *ret = strstr(p, "ret=");
    if (!ret)
        return 0;
    ret += 4;

    int ok_ret = 0;
    uint64_t h = parse_uint_auto(ret, NULL, &ok_ret);
    if (!ok_ret || h > 0x7fffffffu)
        return 0;

    *out_dev_id = dev_id;
    *out_open_type = (int32_t)open_type;
    *out_open_index = (int32_t)open_index;
    *out_handle = (int32_t)h;
    return 1;
}

static void parse_klog_line(const char *line) {
    if (strstr(line, "[payload.elf] [GC]") ||
        strstr(line, "[payload.elf] [Ghostpad]")) {
        return;
    }

    if (strstr(line, "USERASSIGNED") || strstr(line, "DEVICE_DELETED")) {
        gp_log("klog pad event: %.420s\n", line);
    }

    uint64_t open_dev_id = 0;
    int32_t open_type = -1;
    int32_t open_index = -1;
    int32_t open_handle = -1;
    if (!strstr(line, "[GC] klog: Open Pad") &&
        parse_open_pad_line(line, &open_dev_id, &open_type, &open_index, &open_handle)) {
        gp_log("klog: Open Pad dev=0x%llx type=%d index=%d handle=0x%x line=%.320s\n",
               (unsigned long long)open_dev_id, open_type, open_index,
               (uint32_t)open_handle, line);
        klog_open_pad_store(open_dev_id, open_type, open_handle);
        if (open_type == 0 && !is_our_virtual_device_id(open_dev_id)) {
            if (!any_mamba_slot_active()) {
                remember_physical_pad_for_recovery(open_dev_id,
                                                   "physical Open Pad while Manba is off");
            } else {
                maybe_stop_mamba_for_same_user_physical_open(open_dev_id, open_handle, line);
            }
        }
    }

    if (strstr(line, "[GC] klog:"))           return;
    if (!strstr(line, "DEVICE_ADDED"))        return;
    if (!strstr(line, "subType:22"))          return;
    const char *p = strstr(line, "DeviceId:0x");
    if (!p) p = strstr(line, "deviceId=0x");
    if (!p) return;
    p += 11;
    uint64_t id = parse_hex_str(p);
    if (!id) return;
    if (strstr(line, "capabilityBattery:0")) {
        gp_log("klog: VDA device 0x%llx\n", (unsigned long long)id);
        remember_virtual_device_id(id);
        klog_enqueue(id);
    } else {
        gp_log("klog: non-VDA pad device 0x%llx line=%.360s\n",
               (unsigned long long)id, line);
        if (!any_mamba_slot_active())
            remember_physical_pad_for_recovery(id,
                                               "fresh physical DEVICE_ADDED while Manba is off");
    }
}

static void *klog_capture_thread(void *arg) {
    (void)arg;
    char buf[512], line[1024]; size_t line_len = 0;
    int fd = open("/dev/klog", O_RDONLY);
    if (fd < 0) {
        gp_log("klog: /dev/klog errno=%d, trying TCP 127.0.0.1:3232\n", errno);
        struct sockaddr_in sin; int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return NULL;
        memset(&sin,0,sizeof(sin)); sin.sin_family=AF_INET;
        sin.sin_addr.s_addr=inet_addr("127.0.0.1"); sin.sin_port=htons(3232);
        for (int r=0; r<10; r++) {
            if (connect(sock,(struct sockaddr*)&sin,sizeof(sin))==0){fd=sock;gp_log("klog: connected\n");break;}
            usleep(500000);
        }
        if (fd<0){close(sock);return NULL;}
    }
    while (1) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len < 0) { if (errno==EINTR) continue; break; }
        if (len == 0) { usleep(10000); continue; }
        for (ssize_t i=0; i<len; i++) {
            char c = buf[i];
            if (c=='\n'||line_len>=sizeof(line)-1){line[line_len]='\0';parse_klog_line(line);line_len=0;}
            else if (c!='\r') line[line_len++]=c;
        }
    }
    close(fd); return NULL;
}

/* ── VDI injection ────────────────────────────────────────────────────── */
static void inject_pad(int slot, const ScePadData *pad) {
    int32_t h = g_slots[slot].handle;
    if (h < 0 || !g_slots[slot].vdi_ready) return;
    int vr = scePadVirtualDeviceInsertData(h, pad);
    uint32_t n = ++g_slots[slot].inject_count;
    if ((n % 600) == 0)
        gp_log("slot[%d] VDI #%u ret=0x%08x\n", slot, n, (uint32_t)vr);
    static int vdi_err_logged = 0;
    if (vr != 0 && !vdi_err_logged) { gp_log("VDI error 0x%08x\n",(uint32_t)vr); vdi_err_logged=1; }
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

/* Match a (vid,pid) against our supported controller table.
 * Returns 1 if recognized — fills out_vid/out_pid even if PID is unknown
 * (so DS4-family clones with novel PIDs still hit the DS4 path).
 * NOTE: Xbox is NOT matched here — it is detected by GIP interface protocol
 * in probe_one_path (covers all Xbox One/Series PIDs, rejects non-controller
 * Microsoft USB devices). */
static int match_known_vidpid(uint16_t vid, uint16_t pid,
                              uint16_t *out_vid, uint16_t *out_pid) {
    if (mamba_is_supported_vidpid(vid, pid)) {
        *out_vid = vid; *out_pid = pid;
        return 1;
    }
    return 0;
}

static int is_our_virtual_device_id(uint64_t dev_id) {
    if (!dev_id)
        return 0;
    for (int s = 0; s < MAX_SLOTS; s++) {
        if (g_slots[s].virtual_dev_id == dev_id)
            return 1;
    }
    for (int i = 0; i < (int)(sizeof(g_virtual_id_history) / sizeof(g_virtual_id_history[0])); i++) {
        if (g_virtual_id_history[i] == dev_id)
            return 1;
    }
    return 0;
}

static void remember_virtual_device_id(uint64_t dev_id) {
    if (!dev_id)
        return;

    pthread_mutex_lock(&g_slot_lock);
    for (int i = 0; i < (int)(sizeof(g_virtual_id_history) / sizeof(g_virtual_id_history[0])); i++) {
        if (g_virtual_id_history[i] == dev_id) {
            pthread_mutex_unlock(&g_slot_lock);
            return;
        }
    }
    g_virtual_id_history[g_virtual_id_history_w %
                         (sizeof(g_virtual_id_history) / sizeof(g_virtual_id_history[0]))] = dev_id;
    g_virtual_id_history_w++;
    pthread_mutex_unlock(&g_slot_lock);
}

static void klog_open_pad_store(uint64_t dev_id, int32_t open_type, int32_t handle) {
    if (!dev_id || handle < 0)
        return;
    pthread_mutex_lock(&g_klog_lock);
    g_klog_open_pad[g_klog_open_pad_w % KLOG_OPEN_PAD_COUNT].dev_id = dev_id;
    g_klog_open_pad[g_klog_open_pad_w % KLOG_OPEN_PAD_COUNT].handle = handle;
    g_klog_open_pad[g_klog_open_pad_w % KLOG_OPEN_PAD_COUNT].open_type = open_type;
    g_klog_open_pad_w++;
    pthread_mutex_unlock(&g_klog_lock);
}

static int32_t klog_find_open_pad_handle(uint64_t dev_id) {
    int32_t handle = -1;
    pthread_mutex_lock(&g_klog_lock);
    for (int i = 0; i < KLOG_OPEN_PAD_COUNT; i++) {
        if (g_klog_open_pad[i].dev_id == dev_id) {
            handle = g_klog_open_pad[i].handle;
            break;
        }
    }
    pthread_mutex_unlock(&g_klog_lock);
    return handle;
}

static uint64_t klog_find_physical_open_pad(uint64_t virtual_dev_id) {
    uint64_t dev_id = 0;
    pthread_mutex_lock(&g_klog_lock);
    int count = g_klog_open_pad_w < KLOG_OPEN_PAD_COUNT ?
                (int)g_klog_open_pad_w : KLOG_OPEN_PAD_COUNT;
    for (int n = 0; n < count; n++) {
        int idx = ((int)g_klog_open_pad_w - 1 - n) % KLOG_OPEN_PAD_COUNT;
        if (idx < 0) idx += KLOG_OPEN_PAD_COUNT;
        klog_open_pad_t rec = g_klog_open_pad[idx];
        if (!rec.dev_id || rec.dev_id == virtual_dev_id)
            continue;
        if (rec.open_type != 0)
            continue;
        if (is_our_virtual_device_id(rec.dev_id))
            continue;
        dev_id = rec.dev_id;
        break;
    }
    pthread_mutex_unlock(&g_klog_lock);
    return dev_id;
}

static int32_t klog_wait_open_pad_handle(uint64_t dev_id, int ms) {
    for (int t = 0; t <= ms; t += 100) {
        int32_t handle = klog_find_open_pad_handle(dev_id);
        if (handle >= 0)
            return handle;
        usleep(100000);
    }
    return -1;
}

static void remember_physical_pad_for_recovery(uint64_t dev_id, const char *reason) {
    if (!dev_id || is_our_virtual_device_id(dev_id))
        return;

    int should_log = 0;
    pthread_mutex_lock(&g_slot_lock);
    g_last_physical_pad_dev = dev_id;
    if (g_recovered_physical_pad_dev != dev_id &&
        g_pending_physical_recover_dev != dev_id) {
        g_pending_physical_recover_dev = dev_id;
        g_physical_recover_attempts = 0;
        g_physical_recover_last_scan = -1000;
        should_log = 1;
    }
    pthread_mutex_unlock(&g_slot_lock);

    if (should_log) {
        gp_log("physical recover scheduled dev=0x%llx reason=%s\n",
               (unsigned long long)dev_id, reason ? reason : "-");
    }
}

static void physical_recovery_tick(int scan) {
    if (any_mamba_slot_active())
        return;

    uint64_t dev = 0;
    int attempt = 0;
    pthread_mutex_lock(&g_slot_lock);
    dev = g_pending_physical_recover_dev;
    if (!dev || (scan - g_physical_recover_last_scan) < 3) {
        pthread_mutex_unlock(&g_slot_lock);
        return;
    }
    g_physical_recover_last_scan = scan;
    attempt = ++g_physical_recover_attempts;
    pthread_mutex_unlock(&g_slot_lock);

    gp_log("physical recover attempt #%d dev=0x%llx user=0x%08x\n",
           attempt, (unsigned long long)dev, (uint32_t)g_inject_uid);
    int br = shellui_pad_force_bind(dev, g_inject_uid);
    gp_log("physical recover force_bind dev=0x%llx ret=%d\n",
           (unsigned long long)dev, br);

    if (br == 0) {
        pthread_mutex_lock(&g_slot_lock);
        if (g_pending_physical_recover_dev == dev)
            g_pending_physical_recover_dev = 0;
        g_recovered_physical_pad_dev = dev;
        pthread_mutex_unlock(&g_slot_lock);
        notify("Ghost-Control by StonedModder: official controller restored");
    }
}

static int any_mamba_slot_active(void) {
    int active = 0;
    pthread_mutex_lock(&g_slot_lock);
    for (int s = 0; s < MAX_SLOTS; s++) {
        if (g_slots[s].usb_active &&
            g_slots[s].vdi_ready &&
            !g_slots[s].released_pause &&
            mamba_is_supported_vidpid(g_slots[s].vid, g_slots[s].pid)) {
            active = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    return active;
}

static void maybe_disconnect_physical_pad_for_slot(int slot) {
    static uint64_t attempted[4];
    static uint64_t last_vdev[4];
    static int fallback_attempted[4];
    uint64_t vdev = g_slots[slot].virtual_dev_id;
    if (!vdev)
        return;
    if (last_vdev[slot] != vdev) {
        last_vdev[slot] = vdev;
        attempted[slot] = 0;
        fallback_attempted[slot] = 0;
    }
    if (g_slots[slot].physical_evict_done)
        return;

    uint64_t phys = klog_find_physical_open_pad(vdev);
    if (!phys) {
        if (!fallback_attempted[slot]) {
            uint64_t swept = 0;
            fallback_attempted[slot] = 1;
            gp_log("slot[%d] no physical Open Pad before Manba dev=0x%llx; sweeping MBus physical ids\n",
                   slot, (unsigned long long)vdev);
            int sr = shellui_pad_disconnect_first_physical_candidate(vdev, &swept);
            gp_log("slot[%d] physical sweep ret=%d dev=0x%llx\n",
                   slot, sr, (unsigned long long)swept);
            if (sr == 0 && swept) {
                attempted[slot] = swept;
                g_slots[slot].evicted_physical_dev = swept;
                pthread_mutex_lock(&g_slot_lock);
                g_recovered_physical_pad_dev = 0;
                pthread_mutex_unlock(&g_slot_lock);
                remember_physical_pad_for_recovery(swept,
                                                   "physical pad swept for Manba");
                g_slots[slot].physical_evict_done = 1;
            }
        }
        return;
    }
    if (phys == attempted[slot])
        return;

    attempted[slot] = phys;
    g_slots[slot].evicted_physical_dev = phys;
    pthread_mutex_lock(&g_slot_lock);
    g_recovered_physical_pad_dev = 0;
    pthread_mutex_unlock(&g_slot_lock);
    remember_physical_pad_for_recovery(phys, "physical pad evicted for Manba");

    gp_log("slot[%d] game opened physical pad dev=0x%llx before Manba dev=0x%llx; disconnecting physical\n",
           slot, (unsigned long long)phys, (unsigned long long)vdev);
    int dr = shellui_pad_disconnect_device(phys);
    gp_log("slot[%d] disconnect physical dev=0x%llx ret=%d\n",
           slot, (unsigned long long)phys, dr);
    if (dr == 0)
        g_slots[slot].physical_evict_done = 1;
}

static int active_evicted_mamba_slot_snapshot(int *out_slot, uint64_t *out_vdev,
                                              uint64_t *out_evicted_phys) {
    int found = 0;
    pthread_mutex_lock(&g_slot_lock);
    for (int s = 0; s < MAX_SLOTS; s++) {
        if (g_slots[s].usb_active &&
            mamba_is_supported_vidpid(g_slots[s].vid, g_slots[s].pid) &&
            g_slots[s].virtual_dev_id &&
            g_slots[s].physical_evict_done) {
            if (out_slot) *out_slot = s;
            if (out_vdev) *out_vdev = g_slots[s].virtual_dev_id;
            if (out_evicted_phys) *out_evicted_phys = g_slots[s].evicted_physical_dev;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_slot_lock);
    return found;
}

static int is_official_like_physical_device(uint64_t dev_id) {
    if (!dev_id || is_our_virtual_device_id(dev_id))
        return 0;
    return ((dev_id & 0xffffu) == 0x0300u);
}

static int local_pad_user_has_handle(int32_t userId, int32_t observedHandle) {
    if (observedHandle < 0)
        return 0;

    const int types[] = {0, 3, 16};
    for (int t = 0; t < (int)(sizeof(types) / sizeof(types[0])); t++) {
        for (int idx = 0; idx < 8; idx++) {
            int32_t h = scePadGetHandle(userId, types[t], idx);
            if (h >= 0) {
                gp_log("reclaim_local: GH user=0x%08x type=%d idx=%d handle=0x%x observed=0x%x\n",
                       (uint32_t)userId, types[t], idx, (uint32_t)h,
                       (uint32_t)observedHandle);
                if ((uint32_t)h == (uint32_t)observedHandle)
                    return 1;
            }
        }
    }

    return 0;
}

static void release_mamba_vda_only(int slot, const char *reason) {
    if (slot < 0 || slot >= MAX_SLOTS)
        return;

    int32_t handle = -1;
    uint64_t vdev = 0;

    pthread_mutex_lock(&g_slot_lock);
    handle = g_slots[slot].handle;
    vdev = g_slots[slot].virtual_dev_id;
    g_slots[slot].release_requested = 0;
    g_slots[slot].released_pause = 1;
    g_slots[slot].release_wait_neutral = 1;
    g_slots[slot].confirmed = 0;
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].handle = -1;
    g_slots[slot].virtual_dev_id = 0;
    g_slots[slot].evicted_physical_dev = 0;
    g_slots[slot].physical_evict_done = 0;
    g_slots[slot].inject_count = 0;
    if (g_assign_slot == slot)
        g_assign_slot = -1;
    pthread_mutex_unlock(&g_slot_lock);

    gp_log("slot[%d] release pause: disconnect VDA handle=0x%x vdev=0x%llx reason=%s\n",
           slot, (uint32_t)handle, (unsigned long long)vdev,
           reason ? reason : "-");
    if (vdev) {
        int vdr = shellui_pad_disconnect_device(vdev);
        gp_log("slot[%d] release pause disconnect virtual dev=0x%llx ret=%d\n",
               slot, (unsigned long long)vdev, vdr);
    }
    if (handle >= 0)
        scePadVirtualDeviceDeleteDevice(handle);
}

static void request_release_mamba_slot(int slot, const char *reason) {
    if (slot < 0 || slot >= MAX_SLOTS)
        return;

    int active = 0;
    int fd = -1;
    int32_t handle = -1;
    uint64_t vdev = 0;

    pthread_mutex_lock(&g_slot_lock);
    if (g_slots[slot].usb_active) {
        g_slots[slot].release_requested = 1;
        active = 1;
        fd = g_slots[slot].usb_fd;
        handle = g_slots[slot].handle;
        vdev = g_slots[slot].virtual_dev_id;
        if (g_assign_slot == slot)
            g_assign_slot = -1;
    }
    pthread_mutex_unlock(&g_slot_lock);

    gp_log("slot[%d] release request active=%d fd=%d handle=0x%x vdev=0x%llx reason=%s\n",
           slot, active, fd, (uint32_t)handle, (unsigned long long)vdev,
           reason ? reason : "-");
}

static void maybe_stop_mamba_for_same_user_physical_open(uint64_t dev_id,
                                                         int32_t open_handle,
                                                         const char *line) {
    int slot = -1;
    uint64_t vdev = 0;
    uint64_t evicted_phys = 0;

    if (!is_official_like_physical_device(dev_id))
        return;
    if (!active_evicted_mamba_slot_snapshot(&slot, &vdev, &evicted_phys))
        return;

    int local_match = local_pad_user_has_handle(g_inject_uid, open_handle);
    int match = local_match ? 3 : shellui_pad_user_has_handle(g_inject_uid, open_handle);
    gp_log("reclaim_check: physical dev=0x%llx handle=0x%x user=0x%08x match=%d local=%d slot[%d] vdev=0x%llx evicted=0x%llx line=%.220s\n",
           (unsigned long long)dev_id, (uint32_t)open_handle,
           (uint32_t)g_inject_uid, match, local_match, slot,
           (unsigned long long)vdev, (unsigned long long)evicted_phys,
           line ? line : "-");

    if (match != 1 && match != 3) {
        for (int pass = 0; pass < 10; pass++) {
            int32_t fg = -1;
            int32_t ret = sceUserServiceGetForegroundUser(&fg);
            gp_log("reclaim_focus: pass=%d ret=0x%08x fg=0x%08x inject=0x%08x physical=0x%llx\n",
                   pass, (uint32_t)ret, (uint32_t)fg,
                   (uint32_t)g_inject_uid, (unsigned long long)dev_id);
            if (ret == 0 && fg == g_inject_uid) {
                match = 2;
                break;
            }
            usleep(150000);
        }
    }

    if (match != 1 && match != 2 && match != 3)
        return;

    gp_log("reclaim: physical pad dev=0x%llx opened on Manba user=0x%08x match=%d; releasing Manba slot[%d] vdev=0x%llx\n",
           (unsigned long long)dev_id, (uint32_t)g_inject_uid, match,
           slot, (unsigned long long)vdev);
    notify("Ghost-Control: official controller took same user - releasing Manba");
    request_release_mamba_slot(slot, "official same user reclaim");
}

/* Probe a /dev/ugen* path to identify controller type.
 * Returns 1 with vid/pid set, 0 if not a known controller.
 *
 * The descriptor-read path (USB_GET_DEVICEINFO) is non-destructive and
 * runs on any bus. The endpoint-topology fallback IS destructive
 * (DRIVER_DETACH + FS_OPEN) and only runs on ugen2.* — internal buses
 * (ugen0/1) hold the disc drive controller and other system devices that
 * must not be detached. */
static int probe_one_path(const char *path, uint16_t *out_vid, uint16_t *out_pid) {
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
    int desc_ok = 0;
    {
        struct usb_device_descriptor desc;
        memset(&desc, 0, sizeof(desc));
        if (ioctl(fd, USB_GET_DEVICE_DESC, &desc) == 0) {
            uint16_t vid = UGETW(desc.idVendor);
            uint16_t pid = UGETW(desc.idProduct);
            desc_ok = 1;
            gp_log("scan: %s VID=0x%04x PID=0x%04x\n", path, vid, pid);
            if (vid == MAMBA_DONGLE_VID && pid == MAMBA_DONGLE_PID) {
                gp_log("scan: %s Manba receiver idle/update mode ignored\n", path);
                close(fd);
                return 0;
            }
            /* 8BitDo Ultimate 2C Wireless (XInput). Its interface signature is
             * 0x5d/0x01 (same as Manba), so match it by VID/PID FIRST and keep
             * the real pair — the generic Manba-interface branch below would
             * normalize it to 045e/028e and then usb_hid_thread would open the
             * wrong (Manba) endpoints. This device needs IN=0x84 / OUT=0x05. */
            if (vid == GC8BITDO_2C_XINPUT_VID && pid == GC8BITDO_2C_XINPUT_PID) {
                gp_log("scan: %s 8BitDo Ultimate 2C Wireless (XInput)\n", path);
                *out_vid = vid; *out_pid = pid;
                close(fd);
                return 1;
            }
            /* Xbox One/Series detection by GIP interface protocol (issue #2).
             * Catches ALL Xbox One/Series pads regardless of PID. Runs first so
             * a non-0x02ea Xbox normalizes to the canonical VID_XBOX/PID_XBOX
             * the usb_hid_thread routing expects. In this Manba patch build,
             * Xbox GIP devices are still ignored after identification. */
            {
                struct usb_interface_descriptor id;
                memset(&id, 0, sizeof(id));
                if (ioctl(fd, USB_GET_RX_INTERFACE_DESC, &id) == 0 &&
                    id.bInterfaceSubClass == XBOX_GIP_SUBCLASS &&
                    id.bInterfaceProtocol == XBOX_GIP_PROTOCOL) {
                    gp_log("scan: %s ignoring Xbox GIP in Manba patch build (sub=0x%02x proto=0x%02x)\n",
                           path, id.bInterfaceSubClass, id.bInterfaceProtocol);
                    close(fd);
                    return 0;
                }
                if (ioctl(fd, USB_GET_RX_INTERFACE_DESC, &id) == 0 &&
                    mamba_is_xinput_interface(id.bInterfaceSubClass,
                                              id.bInterfaceProtocol)) {
                    gp_log("scan: %s Manba/XInput interface (sub=0x%02x proto=0x%02x)\n",
                           path, id.bInterfaceSubClass, id.bInterfaceProtocol);
                    *out_vid = MAMBA_XINPUT_VID; *out_pid = MAMBA_XINPUT_PID;
                    close(fd);
                    return 1;
                }
            }

            if (match_known_vidpid(vid, pid, out_vid, out_pid)) {
                close(fd);
                return 1;
            }
        } else {
            gp_log("scan: %s USB_GET_DEVICE_DESC errno=%d\n", path, errno);
        }
    }
    if (desc_ok) { close(fd); return 0; }

    /* Destructive fallback: only when descriptor ioctl itself failed AND on
     * external bus. On PS5 the descriptor path always works, so this is
     * effectively dead code — kept as a safety net for unknown firmwares. */
    if (strncmp(path, "/dev/ugen2.", 11) != 0) { close(fd); return 0; }

    struct usb_fs_endpoint ep;
    struct usb_fs_init ini;
    struct usb_fs_uninit u;
    memset(&ep,0,sizeof(ep)); memset(&ini,0,sizeof(ini));
    ini.pEndpoints=&ep; ini.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&ini)!=0) { close(fd); return 0; }

    int ii=0; ioctl(fd,USB_IFACE_DRIVER_DETACH,&ii);
    ii=1;     ioctl(fd,USB_IFACE_DRIVER_DETACH,&ii);
    ii=2;     ioctl(fd,USB_IFACE_DRIVER_DETACH,&ii);

    struct usb_fs_open po;
    memset(&po,0,sizeof(po)); po.ep_index=0; po.max_bufsize=64; po.max_frames=1;

    int found = 0;

    /* ep=0x81: Nintendo/Switch uses 64-byte HID; Manba PC/XInput is smaller. */
    po.ep_no=0x81;
    if (ioctl(fd,USB_FS_OPEN,&po)==0) {
        uint32_t mpkt = po.max_packet_length;
        struct usb_fs_close pc; memset(&pc,0,sizeof(pc)); pc.ep_index=0; ioctl(fd,USB_FS_CLOSE,&pc);
        if (mpkt == 64) {
            gp_log("probe: %s ep=0x81 mpkt=%u → Manba/Switch-HID\n", path,(unsigned)mpkt);
            *out_vid=MAMBA_SWITCH_VID; *out_pid=MAMBA_SWITCH_PID;
            found = 1;
            goto done;
        }
        if (mpkt > 0 && mpkt < 64) {
            gp_log("probe: %s ep=0x81 mpkt=%u → Manba/XInput\n", path,(unsigned)mpkt);
            *out_vid=MAMBA_XINPUT_VID; *out_pid=MAMBA_XINPUT_PID;
            found = 1;
            goto done;
        }
    }

    /* Manba patch build: do not claim Xbox/DS4/Sony/HORI fallback devices. */
    memset(&po,0,sizeof(po)); po.ep_index=0; po.max_bufsize=64; po.max_frames=1;
    po.ep_no=0x82;
    if (ioctl(fd,USB_FS_OPEN,&po)==0 && po.max_packet_length>0 && po.max_packet_length<=64) {
        gp_log("probe: %s ep=0x82 mpkt=%u ignored in Manba patch build\n", path,(unsigned)po.max_packet_length);
        struct usb_fs_close pc; memset(&pc,0,sizeof(pc)); pc.ep_index=0; ioctl(fd,USB_FS_CLOSE,&pc);
    }

done:
    memset(&u,0,sizeof(u)); ioctl(fd,USB_FS_UNINIT,&u);
    close(fd);
    return found;
}

/* ── Create VDA and force_bind for a slot ─────────────────────────────── */
static int32_t create_vda_for_slot(int slot) {
    struct { int32_t size; int32_t userId; int32_t pad[6]; } vdp;
    const int32_t SEN = (int32_t)0xDEADBEEFu;
    int is_mamba = mamba_is_supported_vidpid(g_slots[slot].vid, g_slots[slot].pid);

    memset(&vdp,0,sizeof(vdp)); vdp.size=sizeof(vdp); vdp.userId=1;
    for(int k=0;k<6;k++) vdp.pad[k]=SEN;

    if (is_mamba) {
        gp_log("slot[%d] Manba V2 NBJr VDA create for %s\n",
               slot, mamba_name(g_slots[slot].vid, g_slots[slot].pid));
    }
    int ret = scePadVirtualDeviceAddDevice(&vdp, VIRTUAL_DEVICE_TYPE_DUALSENSE);
    gp_log("slot[%d] VDA ret=0x%08x\n", slot, (uint32_t)ret);

    int32_t handle = (ret > 0) ? ret : -1;
    for(int k=0;k<6;k++){
        if(vdp.pad[k]!=SEN && vdp.pad[k]>0){if(handle<0)handle=vdp.pad[k];break;}
    }

    uint64_t dev_id = klog_dequeue_ms(10000);
    if (dev_id) {
        g_slots[slot].virtual_dev_id = dev_id;
        remember_virtual_device_id(dev_id);
        int br = shellui_pad_force_bind(dev_id, g_inject_uid);
        gp_log("slot[%d] force_bind(0x%llx, 0x%08x) ret=%d\n",
               slot, (unsigned long long)dev_id, (uint32_t)g_inject_uid, br);

        int32_t open_handle = klog_wait_open_pad_handle(dev_id, 4000);
        if (open_handle >= 0) {
            gp_log("slot[%d] klog Open Pad remote handle=0x%x for dev=0x%llx\n",
                   slot, (uint32_t)open_handle, (unsigned long long)dev_id);
        } else {
            gp_log("slot[%d] no Open Pad remote handle for dev=0x%llx\n",
                   slot, (unsigned long long)dev_id);
        }
        handle = (int32_t)(dev_id & 0xffffffffu);
        gp_log("slot[%d] using local VDA handle=0x%x for VDI\n",
               slot, (uint32_t)handle);
        maybe_disconnect_physical_pad_for_slot(slot);
    } else if (handle >= 0) {
        gp_log("slot[%d] klog timeout — using direct handle %d\n", slot, handle);
    } else {
        gp_log("slot[%d] GetHandle scan...\n", slot);
        static const int32_t uids[]={1,0x10000000,(int32_t)0xffffffff};
        for(int ui=0;ui<3&&handle<0;ui++)
            for(int idx=0;idx<8&&handle<0;idx++){
                handle=scePadGetHandle(uids[ui],3,idx);
                if(handle>=0) gp_log("slot[%d] GetHandle uid=0x%08x idx=%d h=%d\n",
                                     slot,(uint32_t)uids[ui],idx,handle);
            }
    }
    if (handle>=0)
        gp_log("slot[%d] VDI handle=0x%x ready\n", slot, (uint32_t)handle);
    else
        gp_log("slot[%d] ERROR: no VDA handle\n", slot);
    return handle;
}

/* ── USB HID thread ───────────────────────────────────────────────────── */
/* Single-session: receives slot+path+vid+pid, runs until disconnect, then exits.
 * Manager thread handles re-detection after exit. */

typedef struct {
    int      slot;
    char     dev_path[32];
    uint16_t vid, pid;
} usb_thread_arg_t;

static void *usb_hid_thread(void *arg) {
    usb_thread_arg_t *targ = (usb_thread_arg_t *)arg;
    int slot = targ->slot;
    char dev_path[32]; memcpy(dev_path, targ->dev_path, sizeof(dev_path));
    uint16_t vid = targ->vid, pid = targ->pid;
    free(targ);

    struct usb_fs_endpoint eps[2];
    struct usb_fs_init     init;
    struct usb_fs_open     fs_open;
    struct usb_fs_start    start;
    struct usb_fs_complete complete;
    struct usb_fs_stop     stop;
    struct usb_fs_close    fs_close;
    struct usb_fs_uninit   uninit;
    uint8_t  buf[64];
    void    *buffers[1]; uint32_t lengths[1];
    int fd = -1, out_opened = 0;
    int usb_ready_notified = 0;

    gp_log("slot[%d] USB thread: %s VID=0x%04x PID=0x%04x\n",
           slot, dev_path, vid, pid);

    /* ── DS4 / HORIPAD / XIM4: single-pass, no handshake ──────────────── */
    if (vid == VID_SONY || vid == VID_HORI) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] DS4 open fail errno=%d\n", slot, errno); goto exit_slot; }

        { int ii; for(ii=0;ii<4;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(100000);

        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] DS4 FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        /* Try DS4 IN ep 0x84 first (real Sony DS4); fall back to 0x81 (HORI). */
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=DS4_EP_IN;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        int in_ok = (ioctl(fd,USB_FS_OPEN,&fs_open)==0);
        if (!in_ok) {
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

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        /* OUT endpoint is optional — XIM4 is input-only, no rumble feedback. */
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=DS4_EP_OUT;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        if (!out_opened) {
            memset(&fs_open,0,sizeof(fs_open));
            fs_open.ep_index=1; fs_open.ep_no=DS4_EP_OUT_ALT;
            fs_open.max_bufsize=64; fs_open.max_frames=1;
            out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        }
        gp_log("slot[%d] DS4 OUT opened=%d\n", slot, out_opened);
        goto main_loop;
    }

    /* ── Manba V2 PC/XInput mode: single-pass ─────────────────────────── */
    if (mamba_is_xinput_vidpid(vid, pid)) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] Mamba open fail errno=%d\n", slot, errno); goto exit_slot; }

        { int ii; for(ii=0;ii<4;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(120000);

        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] Mamba FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        /* Endpoint layout differs per device: classic Manba is IN=0x81/OUT=0x02,
         * but the composite 8BitDo 2C uses IN=0x84/OUT=0x05 (real config
         * descriptor). Try the device-specific endpoints first, then the other
         * variant, so both work even across cable/dongle differences. */
        int is_8bitdo = (vid == GC8BITDO_2C_XINPUT_VID && pid == GC8BITDO_2C_XINPUT_PID);
        uint8_t in_cands[2]  = { is_8bitdo ? GC8BITDO_2C_XINPUT_EP_IN : MAMBA_XINPUT_EP_IN,
                                 is_8bitdo ? MAMBA_XINPUT_EP_IN       : GC8BITDO_2C_XINPUT_EP_IN };
        uint8_t out_cands[3] = { is_8bitdo ? GC8BITDO_2C_XINPUT_EP_OUT : MAMBA_XINPUT_EP_OUT,
                                 MAMBA_XINPUT_EP_OUT_ALT,
                                 is_8bitdo ? MAMBA_XINPUT_EP_OUT       : GC8BITDO_2C_XINPUT_EP_OUT };

        int in_ok = 0;
        for (int c = 0; c < 2 && !in_ok; c++) {
            memset(&fs_open,0,sizeof(fs_open));
            fs_open.ep_index=0; fs_open.ep_no=in_cands[c];
            fs_open.max_bufsize=64; fs_open.max_frames=1;
            if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) {
                in_ok = 1;
                gp_log("slot[%d] XInput IN ep=0x%02x ok maxpkt=%u\n",
                       slot, in_cands[c], (unsigned)fs_open.max_packet_length);
            }
        }
        if (!in_ok) {
            gp_log("slot[%d] XInput IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        for (int c = 0; c < 3 && !out_opened; c++) {
            memset(&fs_open,0,sizeof(fs_open));
            fs_open.ep_index=1; fs_open.ep_no=out_cands[c];
            fs_open.max_bufsize=64; fs_open.max_frames=1;
            if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) {
                out_opened = 1;
                gp_log("slot[%d] XInput OUT ep=0x%02x\n", slot, out_cands[c]);
            }
        }
        gp_log("slot[%d] XInput OUT opened=%d\n", slot, out_opened);
        if (out_opened) mamba_xinput_send_enable(fd, eps);
        goto main_loop;
    }

    /* ── Xbox One: single-pass ─────────────────────────────────────────── */
    if (pid == PID_XBOX) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] Xbox open fail errno=%d\n", slot, errno); goto exit_slot; }

        { int ii; for(ii=0;ii<4;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(120000); /* hub settle: give USB hub time to propagate detach */
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=4;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] Xbox FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=XBOX_EP_IN;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] Xbox IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] Xbox IN ep=0x%02x ok maxpkt=%u\n",
               slot, XBOX_EP_IN, (unsigned)fs_open.max_packet_length);

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=XBOX_EP_OUT;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        gp_log("slot[%d] Xbox OUT ep=0x%02x opened=%d\n", slot, XBOX_EP_OUT, out_opened);

        xbox_gip_handshake(fd, eps);
        goto main_loop;
    }

    /* ── Nintendo: two-pass ────────────────────────────────────────────── */
    fd = open(dev_path, O_RDWR);
    if (fd < 0) { gp_log("slot[%d] open fail errno=%d\n", slot, errno); goto exit_slot; }
    memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
    init.pEndpoints=eps; init.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&init)!=0){
        gp_log("slot[%d] Nintendo FS_INIT p1 fail\n",slot); close(fd); goto exit_slot;
    }
    { int i0=0,i1=1; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0); ioctl(fd,USB_IFACE_DRIVER_DETACH,&i1); }
    memset(&fs_open,0,sizeof(fs_open));
    fs_open.ep_index=0; fs_open.ep_no=0x81; fs_open.max_bufsize=64; fs_open.max_frames=1;
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
        { int i0=0,i1=1,i2=2;
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0);
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i1);
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2); }
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            close(fd); fd=-1; usleep(50000); continue;
        }
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=0x81; fs_open.max_bufsize=64; fs_open.max_frames=1;
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
    fs_open.ep_index=1; fs_open.ep_no=0x02; fs_open.max_bufsize=64; fs_open.max_frames=1;
    out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
    if (!out_opened) {
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=0x01; fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        if (out_opened) gp_log("slot[%d] Nintendo OUT ep=0x01 (real Switch Pro)\n", slot);
    }
    gp_log("slot[%d] Nintendo OUT opened=%d\n", slot, out_opened);
    if (out_opened) {
        usb_send_cmd(fd,&eps[1],0x80,0x02); usleep(30000);
        usb_send_cmd(fd,&eps[1],0x80,0x04); usleep(50000);
        gp_log("slot[%d] Nintendo [80 02]+[80 04] sent\n", slot);
    }

main_loop: ;
    int is_ds4 = (vid == VID_SONY || vid == VID_HORI);
    int is_mamba_xinput = mamba_is_xinput_vidpid(vid, pid);
    int is_mamba_switch = mamba_is_switch_vidpid(vid, pid);
    int hs_state = (pid==PID_XBOX || is_ds4 || is_mamba_xinput) ? HS_STREAMING : HS_WAIT_81_01;
    uint8_t nintendo_seq = 1;
    g_slots[slot].usb_fd = fd;  /* register fd for clean teardown on SIGTERM */

    while (1) {
        if (g_slots[slot].release_requested) {
            gp_log("slot[%d] release requested - pausing VDA only\n", slot);
            release_mamba_vda_only(slot, "official same user reclaim");
        }

        memset(buf,0,64);
        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths;
        eps[0].aFrames=0; eps[0].status=0;

        memset(&start,0,sizeof(start)); start.ep_index=0;
        if (ioctl(fd,USB_FS_START,&start)!=0) {
            if (errno==EBUSY){
                memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
                usleep(5000);
            } else if (errno==ENXIO||errno==ENOTTY){
                gp_log("slot[%d] START errno=%d — device gone\n",slot,errno); goto reinit;
            } else {
                gp_log("slot[%d] START fatal errno=%d\n",slot,errno); goto reinit;
            }
            continue;
        }

        int ok=0, cerr=0, cw=0;
        for(cw=0;cw<60;cw++){
            memset(&complete,0,sizeof(complete)); complete.ep_index=0;
            if(ioctl(fd,USB_FS_COMPLETE,&complete)==0){ok=1;break;}
            cerr=errno;
            if(cerr==ENXIO||cerr==ENOTTY){gp_log("slot[%d] COMPLETE errno=%d — gone\n",slot,cerr);goto reinit;}
            if(cerr!=EBUSY) break;
            usleep(500);
        }
        if(!ok){
            memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
            continue;
        }
        if(lengths[0]<1) continue;

        uint32_t len = lengths[0];

        ScePadData pad; memset(&pad,0,sizeof(pad)); pad.quat.w=1.0f;
        int injected = 0;

        if (is_ds4) {
            injected = ds4_handle_packet(fd, eps, buf, len, &pad);
        } else if (is_mamba_xinput) {
            injected = mamba_xinput_handle_packet(fd, eps, buf, len, &pad);
        } else if (pid == PID_XBOX) {
            injected = xbox_handle_packet(fd, eps, buf, len, &pad);
        } else {
            if (is_mamba_switch) mamba_log_switch_packet(buf, len);
            injected = nintendo_handle_packet(fd, eps, buf, len, &hs_state, &nintendo_seq, &pad);
            if (injected > 0 && is_mamba_switch)
                pad.leftStick.y = (uint8_t)(255u - pad.leftStick.y);
        }

        if (g_slots[slot].released_pause) {
            if (injected > 0) {
                if (g_slots[slot].release_wait_neutral) {
                    if (pad.buttons == 0) {
                        g_slots[slot].release_wait_neutral = 0;
                        gp_log("slot[%d] release pause neutral seen - waiting for new Manba button\n", slot);
                        notify("Ghost-Control: Manba released - press a button to reassign");
                    }
                    continue;
                }

                if (pad.buttons != 0) {
                    if (g_assign_slot >= 0 && g_assign_slot != slot) {
                        gp_log("slot[%d] release pause reassign blocked by assign_slot=%d\n",
                               slot, g_assign_slot);
                        continue;
                    }

                    gp_log("slot[%d] release pause button press - recreating VDA\n", slot);
                    g_assign_slot = slot;
                    int32_t new_handle = create_vda_for_slot(slot);
                    if (new_handle < 0) {
                        gp_log("slot[%d] release pause VDA recreate failed\n", slot);
                        g_assign_slot = -1;
                        continue;
                    }

                    pthread_mutex_lock(&g_slot_lock);
                    g_slots[slot].handle = new_handle;
                    g_slots[slot].vdi_ready = 1;
                    g_slots[slot].released_pause = 0;
                    g_slots[slot].release_wait_neutral = 0;
                    g_slots[slot].confirmed = 0;
                    g_slots[slot].inject_count = 0;
                    pthread_mutex_unlock(&g_slot_lock);
                    notify("Ghost-Control by StonedModder: slot[%d] ready - press a button to assign", slot);
                    gp_log("slot[%d] release pause VDA recreated handle=0x%x\n",
                           slot, (uint32_t)new_handle);
                }
            }
            continue;
        }

        if (injected > 0) {
            if (!usb_ready_notified) {
                notify("Ghost-Control by StonedModder: slot[%d] streaming - controller active", slot);
                usb_ready_notified = 1;
            }
            /* First real button press confirms the assignment — release the gate
             * so the manager can start the next controller's dialog. */
            if (!g_slots[slot].confirmed && pad.buttons != 0) {
                g_slots[slot].confirmed = 1;
                if (g_assign_slot == slot) g_assign_slot = -1;
                gp_log("slot[%d] assignment confirmed (button press)\n", slot);
            }
            inject_pad(slot, &pad);
            if ((g_slots[slot].inject_count % 600) == 0)
                maybe_disconnect_physical_pad_for_slot(slot);
        }
    }

reinit:
    g_slots[slot].usb_fd = -1;  /* unregister before teardown */
    if (usb_ready_notified) { notify("Ghost-Control by StonedModder: slot[%d] controller disconnected", slot); usb_ready_notified=0; }
    memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
    if (out_opened) {
        memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=1; ioctl(fd,USB_FS_CLOSE,&fs_close);
        out_opened=0;
    }
    memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=0; ioctl(fd,USB_FS_CLOSE,&fs_close);
uninit_exit:
    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd); fd=-1;

exit_slot:
    gp_log("slot[%d] USB thread exiting — freeing slot\n", slot);
    uint64_t evicted_phys = g_slots[slot].evicted_physical_dev;
    uint64_t vdev = g_slots[slot].virtual_dev_id;
    if (vdev) {
        int vdr = shellui_pad_disconnect_device(vdev);
        gp_log("slot[%d] disconnect virtual dev=0x%llx ret=%d\n",
               slot, (unsigned long long)vdev, vdr);
    }
    scePadVirtualDeviceDeleteDevice(g_slots[slot].handle);
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].handle    = -1;
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].usb_active= 0;
    g_slots[slot].release_requested = 0;
    g_slots[slot].released_pause = 0;
    g_slots[slot].release_wait_neutral = 0;
    g_slots[slot].usb_fd    = -1;
    g_slots[slot].virtual_dev_id = 0;
    g_slots[slot].evicted_physical_dev = 0;
    g_slots[slot].physical_evict_done = 0;
    g_slots[slot].dev_path[0] = '\0';
    pthread_mutex_unlock(&g_slot_lock);
    if (evicted_phys)
        remember_physical_pad_for_recovery(evicted_phys,
                                           "Manba slot exited");
    return NULL;
}

/* ── Controller manager thread ────────────────────────────────────────── */
static void *controller_manager_thread(void *arg) {
    (void)arg;
    int scan = 0;
    gp_log("Manager thread started (MAX_SLOTS=%d)\n", MAX_SLOTS);

    while (1) {
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

        for (int i = 0; i < n_paths; i++) {
            const char *path = ugen_paths[i];

            /* Serialize assignment: if a controller is still awaiting the user's
             * button press to confirm its dialog, do not start another one. */
            if (g_assign_slot >= 0) break;

            /* Skip if already claimed by an active slot */
            int busy = 0;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].usb_active && strcmp(g_slots[s].dev_path, path) == 0) {
                    busy = 1; break;
                }
            }
            pthread_mutex_unlock(&g_slot_lock);
            if (busy) continue;

            /* Try to identify controller */
            uint16_t vid=0, pid=0;
            if (!probe_one_path(path, &vid, &pid)) continue;

            if (mamba_is_supported_vidpid(vid, pid) && any_mamba_slot_active()) {
                if ((scan % 5) == 0)
                    gp_log("manager: %s ignored because another Manba slot is active\n", path);
                continue;
            }

            /* Find free slot */
            int slot = -1;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].handle < 0 && !g_slots[s].usb_active) { slot=s; break; }
            }
            pthread_mutex_unlock(&g_slot_lock);

            if (slot < 0) {
                if ((scan % 5) == 0) gp_log("manager: all %d slots full\n", MAX_SLOTS);
                continue;
            }

            const char *name =
                mamba_is_supported_vidpid(vid,pid) ? mamba_name(vid,pid) :
                                                     "Unknown";

            gp_log("manager: %s at %s → slot[%d]\n", name, path, slot);
            notify("Ghost-Control by StonedModder: %s detected - assign user on screen", name);

            /* Claim the slot path before VDA so manager skips it if we retry.
             * Set the assignment gate — released when user confirms (button press). */
            pthread_mutex_lock(&g_slot_lock);
            strncpy(g_slots[slot].dev_path, path, sizeof(g_slots[slot].dev_path)-1);
            g_slots[slot].usb_active = 1; /* tentatively claimed */
            g_slots[slot].release_requested = 0;
            g_slots[slot].released_pause = 0;
            g_slots[slot].release_wait_neutral = 0;
            g_slots[slot].confirmed  = 0;
            g_slots[slot].vid = vid;
            g_slots[slot].pid = pid;
            g_slots[slot].evicted_physical_dev = 0;
            g_slots[slot].physical_evict_done = 0;
            pthread_mutex_unlock(&g_slot_lock);
            g_assign_slot = slot;

            /* Create VDA and force_bind (shows PS5 assignment dialog) */
            int32_t handle = create_vda_for_slot(slot);
            if (handle < 0) {
                gp_log("manager: slot[%d] VDA failed — releasing\n", slot);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].usb_active = 0;
                g_slots[slot].release_requested = 0;
                g_slots[slot].released_pause = 0;
                g_slots[slot].release_wait_neutral = 0;
                g_slots[slot].dev_path[0] = '\0';
                g_slots[slot].handle = -1;
                g_slots[slot].virtual_dev_id = 0;
                g_slots[slot].evicted_physical_dev = 0;
                g_slots[slot].physical_evict_done = 0;
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
                continue;
            }

            pthread_mutex_lock(&g_slot_lock);
            g_slots[slot].handle      = handle;
            g_slots[slot].vdi_ready   = 1;
            g_slots[slot].inject_count = 0;
            pthread_mutex_unlock(&g_slot_lock);

            /* Launch USB reader thread */
            usb_thread_arg_t *targ = malloc(sizeof(*targ));
            if (!targ) {
                gp_log("manager: malloc fail for slot[%d]\n", slot);
                scePadVirtualDeviceDeleteDevice(handle);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].handle=-1; g_slots[slot].vdi_ready=0;
                g_slots[slot].usb_active=0; g_slots[slot].release_requested=0; g_slots[slot].released_pause=0; g_slots[slot].release_wait_neutral=0; g_slots[slot].dev_path[0]='\0';
                g_slots[slot].virtual_dev_id=0;
                g_slots[slot].evicted_physical_dev=0;
                g_slots[slot].physical_evict_done=0;
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
                continue;
            }
            targ->slot = slot;
            strncpy(targ->dev_path, path, sizeof(targ->dev_path)-1);
            targ->vid=vid; targ->pid=pid;

            pthread_t tid;
            if (pthread_create(&tid, NULL, usb_hid_thread, targ) != 0) {
                gp_log("manager: pthread_create fail slot[%d]\n", slot);
                free(targ);
                scePadVirtualDeviceDeleteDevice(handle);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].handle=-1; g_slots[slot].vdi_ready=0;
                g_slots[slot].usb_active=0; g_slots[slot].release_requested=0; g_slots[slot].released_pause=0; g_slots[slot].release_wait_neutral=0; g_slots[slot].dev_path[0]='\0';
                g_slots[slot].virtual_dev_id=0;
                g_slots[slot].evicted_physical_dev=0;
                g_slots[slot].physical_evict_done=0;
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
            } else {
                pthread_detach(tid);
                gp_log("manager: slot[%d] USB thread started handle=0x%x\n",
                       slot, (uint32_t)handle);
                notify("Ghost-Control by StonedModder: slot[%d] ready - press a button to assign", slot);
            }
            /* One controller per scan pass — assignment gate blocks the rest
             * until the user confirms this one with a button press. */
            break;
        }

        /* Assignment timeout: if the user never presses a button, release the
         * gate after ~30s so the queue does not stall forever. */
        static int assign_wait = 0;
        if (g_assign_slot >= 0) {
            if (++assign_wait > 6) {  /* 6 * 2s = 12s */
                gp_log("manager: assignment timeout slot[%d] — releasing gate\n", g_assign_slot);
                g_assign_slot = -1;
                assign_wait = 0;
            }
        } else {
            assign_wait = 0;
        }

        physical_recovery_tick(scan);

        scan++;
        if ((scan % 5) == 0) {
            /* Log active slots every 10s */
            int active = 0;
            for (int s = 0; s < MAX_SLOTS; s++)
                if (g_slots[s].usb_active) active++;
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

/* ── main ─────────────────────────────────────────────────────────────── */
/* Clean USB teardown on termination. When a new payload instance kills this one
 * (SIGTERM), release every claimed ugen device so the controller is left in a
 * clean state — otherwise the killed payload leaves endpoints open / the driver
 * detached, and the next instance can't re-handshake without a physical replug.
 * close()/_exit() are async-signal-safe; the USB ioctls run at process death. */
static void cleanup_and_exit(int sig) {
    (void)sig;
    for (int s = 0; s < MAX_SLOTS; s++) {
        int fd = g_slots[s].usb_fd;
        if (fd >= 0) {
            struct usb_fs_stop  st; memset(&st,0,sizeof(st));
            st.ep_index=0; ioctl(fd,USB_FS_STOP,&st);
            st.ep_index=1; ioctl(fd,USB_FS_STOP,&st);
            struct usb_fs_close fc; memset(&fc,0,sizeof(fc));
            fc.ep_index=0; ioctl(fd,USB_FS_CLOSE,&fc);
            fc.ep_index=1; ioctl(fd,USB_FS_CLOSE,&fc);
            struct usb_fs_uninit un; memset(&un,0,sizeof(un));
            ioctl(fd,USB_FS_UNINIT,&un);
            close(fd);
            g_slots[s].usb_fd = -1;
        }
        if (g_slots[s].handle >= 0)
            scePadVirtualDeviceDeleteDevice(g_slots[s].handle);
    }
    _exit(0);
}

int main(void) {
    int32_t userId=-1, fgUser=-1; int ret;

    ghostpad_status_log_reset();
    gp_log("Ghost-Control by StonedModder - Patch Manba V2 NBJr starting - %d slots\n", MAX_SLOTS);
    notify("Ghost-Control by StonedModder - Patch Manba V2 NBJr");

    /* Kill previous instance */
    { int pfd=open(PID_PATH,O_RDONLY);
      if(pfd>=0){char pb[16]={0};read(pfd,pb,15);close(pfd);
        pid_t old=(pid_t)atoi(pb);
        if(old>0&&old!=getpid()){gp_log("Killing prev pid=%d\n",old);kill(old,SIGTERM);usleep(1200000);}
      }
      int pfd2=open(PID_PATH,O_WRONLY|O_CREAT|O_TRUNC,0600);
      if(pfd2>=0){char pb[16];snprintf(pb,sizeof(pb),"%d",getpid());write(pfd2,pb,strlen(pb));close(pfd2);}
    }

    /* Init slots */
    for (int s = 0; s < MAX_SLOTS; s++) {
        g_slots[s].handle     = -1;
        g_slots[s].vdi_ready  = 0;
        g_slots[s].usb_active = 0;
        g_slots[s].release_requested = 0;
        g_slots[s].released_pause = 0;
        g_slots[s].release_wait_neutral = 0;
        g_slots[s].confirmed  = 0;
        g_slots[s].usb_fd     = -1;
        g_slots[s].virtual_dev_id = 0;
        g_slots[s].evicted_physical_dev = 0;
        g_slots[s].physical_evict_done = 0;
        g_slots[s].dev_path[0]= '\0';
    }
    g_assign_slot = -1;

    /* Clean teardown when the next deploy kills us — releases the controllers */
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT,  cleanup_and_exit);

    sceUserServiceInitialize(NULL);
    sceUserServiceGetInitialUser(&userId);
    sceUserServiceGetForegroundUser(&fgUser);
    gp_log("userId=0x%08x fgUser=0x%08x\n", (uint32_t)userId, (uint32_t)fgUser);
    g_inject_uid = (fgUser > 0) ? fgUser : userId;
    if ((uint32_t)userId<0x10000000u||(uint32_t)userId>0x1000000Fu) userId=0x10000000;
    gp_log("inject_uid=0x%08x\n", (uint32_t)g_inject_uid);

    elevate_credentials();

    ret=scePadInit(); gp_log("scePadInit: 0x%08x\n", ret);
    ret=scePadSetProcessPrivilege(1); gp_log("scePadSetProcessPrivilege: 0x%08x\n", ret);

    /* Clean up any orphaned VDA devices */
    for (int dh=0; dh<64; dh++) {
        if (scePadVirtualDeviceDeleteDevice(dh)==0) gp_log("deleteDevice(%d)\n", dh);
    }

    /* Start klog capture thread first — must be running before any VDA call */
    pthread_t klog_tid;
    if (pthread_create(&klog_tid, NULL, klog_capture_thread, NULL)==0) {
        pthread_detach(klog_tid);
        gp_log("klog thread started\n");
    }
    usleep(300000); /* let klog thread connect before first VDA */

    /* Start controller manager — handles all detection, VDA creation, USB threads */
    pthread_t mgr_tid;
    if (pthread_create(&mgr_tid, NULL, controller_manager_thread, NULL)==0) {
        pthread_detach(mgr_tid);
        gp_log("Manager thread started\n");
    }

    /* Keep-alive */
    uint32_t tick = 0;
    while (1) {
        usleep(1000000);
        tick++;
        if (tick % 10 == 0) {
            int active = 0;
            for (int s = 0; s < MAX_SLOTS; s++) if (g_slots[s].usb_active) active++;
            gp_log("alive tick=%u active_slots=%d\n", tick, active);
        }
    }
    return 0;
}

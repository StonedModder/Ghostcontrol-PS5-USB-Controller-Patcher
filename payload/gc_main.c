/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ghost-Control v5: Multi-controller support
 * USB HID controllers → virtual DualSense devices on PS5
 *
 * Supports up to MAX_SLOTS (4) simultaneous controllers:
 *   - 8BitDo / Nintendo Switch Pro  (VID=057E PID=2009)
 *   - Xbox One S                    (VID=045E PID=02EA)
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
#include "controller_gcpad.h"
#include "controller_n64.h"
#include "controller_ds4.h"
#include "gip_detect.h"

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
    volatile int     confirmed;     /* user pressed a button — assignment done */
    char             dev_path[32];  /* ugen path claimed by this slot */
    uint16_t         vid, pid;
    volatile uint32_t inject_count;
} ctrl_slot_t;

static ctrl_slot_t     g_slots[MAX_SLOTS];
static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t         g_inject_uid = 0x10000000;

/* Assignment serialization: only ONE controller may show its assignment
 * dialog at a time. g_assign_slot = slot awaiting user confirmation, or -1.
 * Without this, multiple dialogs stack and all bind to the same user. */
static volatile int    g_assign_slot = -1;

/* ── klog device-ID queue ─────────────────────────────────────────────── */
#define KLOG_QSIZE 16
static uint64_t        g_klog_q[KLOG_QSIZE];
static int             g_klog_qw = 0, g_klog_qr = 0;
static pthread_mutex_t g_klog_lock = PTHREAD_MUTEX_INITIALIZER;

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

static void parse_klog_line(const char *line) {
    if (!strstr(line, "DEVICE_ADDED"))        return;
    if (!strstr(line, "subType:22"))          return;
    if (!strstr(line, "capabilityBattery:0")) return;
    const char *p = strstr(line, "DeviceId:0x");
    if (!p) p = strstr(line, "deviceId=0x");
    if (!p) return;
    p += 11;
    uint64_t id = parse_hex_str(p);
    if (!id) return;
    gp_log("klog: VDA device 0x%llx\n", (unsigned long long)id);
    klog_enqueue(id);
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

static const char *UGEN_PATHS[] = {
    "/dev/ugen2.2","/dev/ugen2.3","/dev/ugen2.4","/dev/ugen2.5",
    "/dev/ugen2.6","/dev/ugen2.7","/dev/ugen2.8","/dev/ugen2.9",
    "/dev/ugen1.2","/dev/ugen0.2","/dev/ugen0.3",
};
#define N_UGEN_PATHS ((int)(sizeof(UGEN_PATHS)/sizeof(UGEN_PATHS[0])))

#ifndef USB_SHORT_XFER_OK
#define USB_SHORT_XFER_OK 0x0004  /* from dev/usb/usbdi.h */
#endif

/* Read a USB string descriptor as ASCII into out (may be NULL) and log it. */
static void probe_log_string(int fd, const char *path, const char *what, uint8_t idx,
                             char *out, int outlen, int quiet) {
    if (out && outlen > 0) out[0] = 0;
    if (!idx) { if (!quiet) gp_log("probe: %s %s: (none)\n", path, what); return; }
    uint8_t raw[128]; memset(raw,0,sizeof(raw));
    struct usb_gen_descriptor gd; memset(&gd,0,sizeof(gd));
    gd.ugd_data = raw; gd.ugd_maxlen = sizeof(raw); gd.ugd_string_index = idx;
    gd.ugd_lang_id = 0x0409;
    if (ioctl(fd, USB_GET_STRING_DESC, &gd) != 0) {
        if (!quiet) gp_log("probe: %s %s: read failed errno=%d\n", path, what, errno);
        return;
    }
    char s[64]; int n = 0;
    for (int i = 2; i + 1 < (int)gd.ugd_actlen && n < (int)sizeof(s) - 1; i += 2)
        s[n++] = (raw[i] >= 0x20 && raw[i] < 0x7f) ? (char)raw[i] : '?';
    s[n] = 0;
    if (!quiet) gp_log("probe: %s %s: \"%s\"\n", path, what, s);
    if (out && outlen > 0) { strncpy(out, s, (size_t)outlen - 1); out[outlen - 1] = 0; }
}

/* Read the active configuration descriptor with a standard GET_DESCRIPTOR
 * control request and check it for an Xbox One/Series (GIP) interface. */
static int probe_is_gip(int fd, const char *path, int quiet) {
    uint8_t cd[512]; memset(cd,0,sizeof(cd));
    struct usb_ctl_request cr; memset(&cr,0,sizeof(cr));
    cr.ucr_data = cd;
    cr.ucr_flags = USB_SHORT_XFER_OK;
    cr.ucr_request.bmRequestType = 0x80;   /* device-to-host, standard, device */
    cr.ucr_request.bRequest = 0x06;        /* GET_DESCRIPTOR */
    USETW(cr.ucr_request.wValue, 0x0200);  /* CONFIGURATION, index 0 */
    USETW(cr.ucr_request.wIndex, 0);
    USETW(cr.ucr_request.wLength, sizeof(cd));
    if (ioctl(fd, USB_DO_REQUEST, &cr) != 0) {
        if (!quiet) gp_log("probe: %s config descriptor read failed errno=%d\n", path, errno);
        return 0;
    }
    return gip_cdesc_has_gip_iface(cd, cr.ucr_actlen);
}

/* ── Settings: /data/ghostpad/config.ini ─────────────────────────────── */
#define CONFIG_PATH "/data/ghostpad/config.ini"
#define DEFAULT_CONFIG_TEXT \
    "# Ghostcontrol settings - edit over FTP, then restart the payload.\n" \
    "#\n" \
    "# ds4 = native   : leave a USB PS4 controller to the PS5 (default;\n" \
    "#                  works in PS4 games and menus)\n" \
    "# ds4 = takeover : EXPERIMENTAL - present it as a virtual DualSense for\n" \
    "#                  PS5 games. The PS5 currently fights this: expect\n" \
    "#                  resets, or a connection that does nothing.\n" \
    "ds4=native\n"
/* DS4 takeover is EXPERIMENTAL: the PS5 fights it (device resets, or a
 * stable connection whose input the PS5 ignores). Default: leave it native. */
static int g_ds4_takeover = 0;

static void load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        f = fopen(CONFIG_PATH, "w");
        if (f) {
            fputs(DEFAULT_CONFIG_TEXT, f);
            fclose(f);
        }
        gp_log("config: %s created with defaults (ds4=native)\n", CONFIG_PATH);
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[32], val[32];
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == 0) continue;
        if (sscanf(p, " %31[^= \t] = %31s", key, val) != 2) continue;
        if (strcmp(key, "ds4") == 0) {
            if (strcmp(val, "native") == 0) g_ds4_takeover = 0;
            else if (strcmp(val, "takeover") == 0) g_ds4_takeover = 1;
            else gp_log("config: unknown ds4 value \"%s\" — keeping %s\n",
                        val, g_ds4_takeover ? "takeover" : "native");
        }
    }
    fclose(f);
    gp_log("config: ds4=%s\n", g_ds4_takeover ? "takeover" : "native");
}

/* Per-path "already logged as unsupported" key (vid<<16|pid), to keep the
 * log quiet while an unsupported device stays plugged in. */
static uint32_t g_probe_seen[N_UGEN_PATHS];
static int probe_path_index(const char *path) {
    for (int i = 0; i < N_UGEN_PATHS; i++) if (strcmp(UGEN_PATHS[i], path) == 0) return i;
    return -1;
}
static uint32_t probe_seen_key(const char *path) {
    int i = probe_path_index(path); return i >= 0 ? g_probe_seen[i] : 0;
}
static void probe_seen_set(const char *path, uint32_t key) {
    int i = probe_path_index(path); if (i >= 0) g_probe_seen[i] = key;
}

/* Probe one ugen2.x path to identify controller type.
 * Returns 1 with vid/pid set, 0 if not a known controller.
 * Skips non-ugen2 paths (not on external USB bus). */
static int probe_one_path(const char *path, uint16_t *out_vid, uint16_t *out_pid) {
    if (strncmp(path, "/dev/ugen2.", 11) != 0) return 0;

    int fd = open(path, O_RDWR|O_NONBLOCK);
    if (fd < 0) return 0;

    struct usb_fs_endpoint ep;
    struct usb_fs_init ini;
    struct usb_fs_uninit u;
    memset(&ep,0,sizeof(ep)); memset(&ini,0,sizeof(ini));
    ini.pEndpoints=&ep; ini.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&ini)!=0) { close(fd); return 0; }

    /* No driver detach here: identification below uses only EP0 control
     * requests, so the PS5's own driver stays attached to devices we don't
     * take over (e.g. a natively supported PS4 controller, USB storage).
     * The USB reader thread detaches the controllers it claims. */

    struct usb_fs_open po;
    memset(&po,0,sizeof(po)); po.ep_index=0; po.max_bufsize=64; po.max_frames=1;

    int found = 0;

    /* Identify by vendor ID from the device descriptor when the PS5 allows it.
     * Endpoint shape alone is ambiguous: some Xbox pads also have a 64-byte
     * IN at 0x81, which the endpoint rules below would call "Nintendo". */
    int have_dd = 0, quiet = 0;
    uint16_t dv = 0, dp = 0;
    {
        struct usb_device_descriptor dd; memset(&dd,0,sizeof(dd));
        if (ioctl(fd,USB_GET_DEVICE_DESC,&dd)==0) {
            have_dd = 1;
            dv = UGETW(dd.idVendor); dp = UGETW(dd.idProduct);
            /* An unsupported device is re-probed every scan: log it only once */
            quiet = (probe_seen_key(path) == (((uint32_t)dv << 16) | dp));
            if (!quiet)
                gp_log("probe: %s descriptor vid=%04x pid=%04x bcdDevice=%04x\n",
                       path, dv, dp, UGETW(dd.bcdDevice));
            char mfr[64];
            probe_log_string(fd, path, "manufacturer", dd.iManufacturer, mfr, sizeof(mfr), quiet);
            probe_log_string(fd, path, "product", dd.iProduct, NULL, 0, quiet);
            if (dv==VID_SWITCH && strncmp(mfr, N64PRO_MANUFACTURER, strlen(N64PRO_MANUFACTURER))==0) {
                /* N64-style pad posing as a Switch Pro Controller on consoles */
                gp_log("probe: %s → N64-style pad in Switch Pro mode\n", path);
                *out_vid=VID_SWITCH; *out_pid=PID_N64PRO; found=1; goto done;
            }
            if (dv==VID_XBOX) {
                gp_log("probe: %s → Xbox (by vendor ID)\n", path);
                *out_vid=VID_XBOX; *out_pid=PID_XBOX; found=1; goto done;
            }
            if (dv==VID_SWITCH) {
                gp_log("probe: %s → Nintendo (by vendor ID)\n", path);
                *out_vid=VID_SWITCH; *out_pid=PID_SWITCH; found=1; goto done;
            }
            if (dv==VID_N64 && dp==PID_N64) {
                gp_log("probe: %s → N64-style pad (by vendor ID)\n", path);
                *out_vid=VID_N64; *out_pid=PID_N64; found=1; goto done;
            }
            if (ds4_is_ds4(dv, dp)) {
                if (g_ds4_takeover) {
                    gp_log("probe: %s → PS4 controller (DualShock 4), taking over\n", path);
                    *out_vid=VID_SONY; *out_pid=PID_DS4; found=1; goto done;
                }
                if (!quiet) gp_log("probe: %s PS4 controller left native (ds4=native)\n", path);
                probe_seen_set(path, ((uint32_t)dv << 16) | dp);
                goto done;
            }
            if (dv==VID_GCPAD && dp==PID_GCPAD) {
                gp_log("probe: %s → GameCube-style pad (by vendor ID)\n", path);
                *out_vid=VID_GCPAD; *out_pid=PID_GCPAD; found=1; goto done;
            }
            /* Unknown vendor: try the GIP signature, then the endpoint rules */
        } else {
            gp_log("probe: %s descriptor read failed errno=%d\n", path, errno);
        }
    }

    /* Third-party Xbox One/Series pads (PowerA, PDP, Hori...) have their own
     * vendor IDs but share the GIP interface signature ff/47/d0. */
    if (probe_is_gip(fd, path, quiet)) {
        gp_log("probe: %s → Xbox (GIP interface ff/47/d0)\n", path);
        *out_vid=VID_XBOX; *out_pid=PID_XBOX; found=1; goto done;
    }

    /* Identified but not supported: leave it with the PS5's driver */
    if (have_dd) {
        if (!quiet) gp_log("probe: %s %04x:%04x not supported — left alone\n", path, dv, dp);
        probe_seen_set(path, ((uint32_t)dv << 16) | dp);
        goto done;
    }

    /* Descriptor unreadable: fall back to endpoint guessing, which needs the
     * PS5 driver detached to open endpoints. */
    { int i0=0,i1=1,i2=2;
      ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0);
      ioctl(fd,USB_IFACE_DRIVER_DETACH,&i1);
      ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2); }

    /* ep=0x81: maxpkt=64 → Nintendo, maxpkt=8 → DragonRise GameCube-style pad */
    po.ep_no=0x81;
    if (ioctl(fd,USB_FS_OPEN,&po)==0) {
        uint32_t mpkt = po.max_packet_length;
        struct usb_fs_close pc; memset(&pc,0,sizeof(pc)); pc.ep_index=0; ioctl(fd,USB_FS_CLOSE,&pc);
        if (mpkt==64) {
            gp_log("probe: %s ep=0x81 mpkt=%u → Nintendo\n", path,(unsigned)mpkt);
            *out_vid=VID_SWITCH; *out_pid=PID_SWITCH;
            found = 1;
            goto done;
        }
        if (mpkt==8) {
            /* USB_GET_DEVICEINFO fails on PS5 ugen; when it works, require 0079:0006 */
            struct usb_device_info di; memset(&di,0,sizeof(di));
            int have_di = (ioctl(fd,USB_GET_DEVICEINFO,&di)==0);
            if (!have_di || (di.udi_vendorNo==VID_GCPAD && di.udi_productNo==PID_GCPAD)) {
                gp_log("probe: %s ep=0x81 mpkt=8 → GameCube-style pad\n", path);
                *out_vid=VID_GCPAD; *out_pid=PID_GCPAD;
                found = 1;
                goto done;
            }
            gp_log("probe: %s ep=0x81 mpkt=8 vid=%04x pid=%04x — unknown 8-byte HID, skipped\n",
                   path, di.udi_vendorNo, di.udi_productNo);
        }
    }

    /* Xbox One: ep=0x82, maxpkt in (0,64] */
    memset(&po,0,sizeof(po)); po.ep_index=0; po.max_bufsize=64; po.max_frames=1;
    po.ep_no=0x82;
    if (ioctl(fd,USB_FS_OPEN,&po)==0 && po.max_packet_length>0 && po.max_packet_length<=64) {
        gp_log("probe: %s ep=0x82 mpkt=%u → Xbox One\n", path,(unsigned)po.max_packet_length);
        struct usb_fs_close pc; memset(&pc,0,sizeof(pc)); pc.ep_index=0; ioctl(fd,USB_FS_CLOSE,&pc);
        *out_vid=VID_XBOX; *out_pid=PID_XBOX;
        found = 1;
        goto done;
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
    memset(&vdp,0,sizeof(vdp)); vdp.size=sizeof(vdp); vdp.userId=1;
    for(int k=0;k<6;k++) vdp.pad[k]=SEN;

    int ret = scePadVirtualDeviceAddDevice(&vdp, VIRTUAL_DEVICE_TYPE_DUALSENSE);
    gp_log("slot[%d] VDA ret=0x%08x\n", slot, (uint32_t)ret);

    int32_t handle = (ret > 0) ? ret : -1;
    for(int k=0;k<6;k++){
        if(vdp.pad[k]!=SEN && vdp.pad[k]>0){if(handle<0)handle=vdp.pad[k];break;}
    }

    uint64_t dev_id = klog_dequeue_ms(10000);
    if (dev_id) {
        handle = (int32_t)(dev_id & 0xffffffffu);
        int br = shellui_pad_force_bind(dev_id, g_inject_uid);
        gp_log("slot[%d] force_bind(0x%llx, 0x%08x) ret=%d\n",
               slot, (unsigned long long)dev_id, (uint32_t)g_inject_uid, br);
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
    struct timeval t_session; gettimeofday(&t_session, NULL);
    uint32_t hid_xfer_len = 64;   /* plain HID pads: exact IN packet size */

    gp_log("slot[%d] USB thread: %s VID=0x%04x PID=0x%04x\n",
           slot, dev_path, vid, pid);

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

        /* Xbox pads use IN 0x82/OUT 0x02 (One S) or IN 0x81/OUT 0x01 (others) */
        int xin = -1;
        { static const uint8_t ins[2] = { XBOX_EP_IN, 0x81 };
          for (int k = 0; k < 2 && xin < 0; k++) {
              memset(&fs_open,0,sizeof(fs_open));
              fs_open.ep_index=0; fs_open.ep_no=ins[k];
              fs_open.max_bufsize=64; fs_open.max_frames=1;
              if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) xin = ins[k];
          } }
        if (xin < 0){
            gp_log("slot[%d] Xbox IN fail (0x82 and 0x81) errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] Xbox IN ep=0x%02x ok maxpkt=%u\n",
               slot, xin, (unsigned)fs_open.max_packet_length);

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        /* OUT endpoint matches the IN number (0x82→0x02, 0x81→0x01); try both */
        int xout = (xin == 0x81) ? 0x01 : XBOX_EP_OUT;
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=xout;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        if (!out_opened) {
            xout = (xout == 0x01) ? XBOX_EP_OUT : 0x01;
            memset(&fs_open,0,sizeof(fs_open));
            fs_open.ep_index=1; fs_open.ep_no=xout;
            fs_open.max_bufsize=64; fs_open.max_frames=1;
            out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        }
        gp_log("slot[%d] Xbox OUT ep=0x%02x opened=%d\n", slot, xout, out_opened);

        xbox_gip_handshake(fd, eps);
        goto main_loop;
    }

    /* ── DualShock 4: HID on interface 3, IN 0x84, no handshake ─────────── */
    if (vid == VID_SONY && pid == PID_DS4) {
        int attempt;
        for (attempt = 0; attempt < 5; attempt++) {
            fd = open(dev_path, O_RDWR);
            if (fd < 0) { gp_log("slot[%d] DS4 open fail errno=%d\n", slot, errno); goto exit_slot; }
            /* Detach all four interfaces (0-2 headset audio, 3 controller).
             * Leaving the audio attached kept the PS5 treating the device as
             * its native PS4 controller, and the virtual DualSense got no
             * assignment prompt and no effect. */
            { int ii; for (ii = 0; ii <= DS4_IFACE; ii++) {
                  int i2 = ii; int r = ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);
                  if (attempt == 0) gp_log("slot[%d] DS4 detach iface%d ret=%d errno=%d\n",
                                           slot, ii, r, r ? errno : 0); } }
            memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
            init.pEndpoints=eps; init.ep_index_max=1;
            if (ioctl(fd,USB_FS_INIT,&init)==0) {
                memset(&fs_open,0,sizeof(fs_open));
                fs_open.ep_index=0; fs_open.ep_no=DS4_EP_IN;
                fs_open.max_bufsize=64; fs_open.max_frames=1;
                if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) break;
                memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
            }
            gp_log("slot[%d] DS4 OPEN retry %d errno=%d\n", slot, attempt, errno);
            close(fd); fd=-1;
            usleep(50000);
        }
        if (fd < 0) { gp_log("slot[%d] DS4 give up\n", slot); goto exit_slot; }
        gp_log("slot[%d] DS4 IN ep=0x%02x ok maxpkt=%u\n",
               slot, DS4_EP_IN, (unsigned)fs_open.max_packet_length);
        hid_xfer_len = fs_open.max_packet_length;
        if (hid_xfer_len == 0 || hid_xfer_len > 64) hid_xfer_len = 64;

        buffers[0]=buf; lengths[0]=hid_xfer_len;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=100; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;
        goto main_loop;
    }

    /* ── Plain HID pads (GameCube-style, N64-style): IN only, no handshake ── */
    if ((pid == PID_GCPAD && vid == VID_GCPAD) || (pid == PID_N64 && vid == VID_N64)) {
        int attempt;
        for (attempt = 0; attempt < 5; attempt++) {
            fd = open(dev_path, O_RDWR);
            if (fd < 0) { gp_log("slot[%d] gcpad open fail errno=%d\n", slot, errno); goto exit_slot; }
            { int i0=0; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0); }
            memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
            init.pEndpoints=eps; init.ep_index_max=1;
            if (ioctl(fd,USB_FS_INIT,&init)==0) {
                memset(&fs_open,0,sizeof(fs_open));
                fs_open.ep_index=0; fs_open.ep_no=0x81;  /* GCPAD_EP_IN == N64_EP_IN */
                fs_open.max_bufsize=64; fs_open.max_frames=1;
                if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) break;
                memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
            }
            gp_log("slot[%d] gcpad OPEN retry %d errno=%d\n", slot, attempt, errno);
            close(fd); fd=-1;
            usleep(50000);
        }
        if (fd < 0) { gp_log("slot[%d] gcpad give up\n", slot); goto exit_slot; }
        gp_log("slot[%d] HID pad IN ep=0x81 ok maxpkt=%u\n",
               slot, (unsigned)fs_open.max_packet_length);
        /* Reads must request exactly wMaxPacketSize (8 for gcpad, 64 for N64) */
        hid_xfer_len = fs_open.max_packet_length;
        if (hid_xfer_len == 0 || hid_xfer_len > 64) hid_xfer_len = 64;

        buffers[0]=buf; lengths[0]=hid_xfer_len;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=100; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;
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
    const int is_gcpad = (pid == PID_GCPAD && vid == VID_GCPAD);
    const int is_n64   = (pid == PID_N64   && vid == VID_N64);
    const int is_ds4   = (vid == VID_SONY && pid == PID_DS4);
    const int is_hid   = is_gcpad || is_n64 || is_ds4;
    int hs_state = (pid==PID_XBOX || is_hid) ? HS_STREAMING : HS_WAIT_81_01;
    uint8_t nintendo_seq = 1;
    /* HID pad transfers run with a 100ms timeout, so wait up to ~120ms for COMPLETE */
    const int cw_max = is_hid ? 240 : 60;
    int hid_logged = 0;
    struct timeval ds4_tcheck; gettimeofday(&ds4_tcheck, NULL);   /* DIAG (DS4 resets) */

    while (1) {
        /* DIAG: once a second, see whether a PS5 driver has re-attached to the
         * DS4's interfaces; if iface 3 is reclaimed, log it and detach again. */
        if (is_ds4) {
            struct timeval tn; gettimeofday(&tn, NULL);
            if (tn.tv_sec != ds4_tcheck.tv_sec) {
                ds4_tcheck = tn;
                long el = (long)(tn.tv_sec - t_session.tv_sec);
                for (int ifc = 0; ifc <= DS4_IFACE; ifc++) {
                    int i2 = ifc;
                    if (ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &i2) == 0) {
                        char name[64]; memset(name, 0, sizeof(name));
                        struct usb_gen_descriptor gd; memset(&gd, 0, sizeof(gd));
                        gd.ugd_data = name; gd.ugd_maxlen = sizeof(name) - 1; gd.ugd_iface_index = (uint8_t)ifc;
                        ioctl(fd, USB_GET_IFACE_DRIVER, &gd);
                        int i3 = ifc; int r = ioctl(fd, USB_IFACE_DRIVER_DETACH, &i3);
                        gp_log("slot[%d] DS4 t=%lds: driver \"%s\" re-attached to iface%d — re-detach ret=%d\n",
                               slot, el, name, ifc, r);
                    }
                }
                if (el % 20 == 0) {   /* periodic snapshot of all four interfaces */
                    int a[4];
                    for (int ifc = 0; ifc < 4; ifc++) { int i2 = ifc; a[ifc] = (ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &i2) == 0); }
                    gp_log("slot[%d] DS4 t=%lds: drivers active iface0-3 = %d%d%d%d\n",
                           slot, el, a[0], a[1], a[2], a[3]);
                }
            }
        }
        memset(buf,0,64);
        /* The PS5 never completes a 64-byte read on an 8-byte EP —
         * plain HID pads must request exactly their packet size. */
        buffers[0]=buf; lengths[0]= is_hid ? hid_xfer_len : 64;
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
        for(cw=0;cw<cw_max;cw++){
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

        if (pid == PID_XBOX) {
            injected = xbox_handle_packet(fd, eps, buf, len, &pad);
        } else if (is_hid) {
            if (hid_logged < 3) {
                hid_logged++;
                gp_log("slot[%d] HID pad report len=%u: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       slot, (unsigned)len, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
            }
            injected = is_ds4 ? ds4_handle_packet(buf, len, &pad)
                     : is_n64 ? n64_handle_packet(buf, len, &pad)
                              : gcpad_handle_packet(buf, len, &pad);
        } else {
            injected = nintendo_handle_packet(fd, eps, buf, len, &hs_state, &nintendo_seq, &pad);
            /* N64-style pad in Pro mode: same handshake, its own button layout */
            if (injected > 0 && pid == PID_N64PRO && len >= 12 &&
                (buf[0] == 0x30 || buf[0] == 0x21 || buf[0] == 0x00))
                n64pro_parse_0x30(buf, &pad);
        }

        if (injected > 0) {
            if (!usb_ready_notified) {
                notify("Ghostcontrol: slot[%d] streaming — controller active", slot);
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
        }
    }

reinit:
    { struct timeval tn; gettimeofday(&tn, NULL);
      gp_log("slot[%d] session lasted %lds\n", slot, (long)(tn.tv_sec - t_session.tv_sec)); }
    if (usb_ready_notified) { notify("Ghostcontrol: slot[%d] controller disconnected", slot); usb_ready_notified=0; }
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
    scePadVirtualDeviceDeleteDevice(g_slots[slot].handle);
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].handle    = -1;
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].usb_active= 0;
    g_slots[slot].dev_path[0] = '\0';
    pthread_mutex_unlock(&g_slot_lock);
    return NULL;
}

/* ── Controller manager thread ────────────────────────────────────────── */
static void *controller_manager_thread(void *arg) {
    (void)arg;
    int scan = 0;
    gp_log("Manager thread started (MAX_SLOTS=%d)\n", MAX_SLOTS);

    while (1) {
        for (int i = 0; i < N_UGEN_PATHS; i++) {
            const char *path = UGEN_PATHS[i];

            /* Skip non-external-bus paths */
            if (strncmp(path, "/dev/ugen2.", 11) != 0) continue;

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
                (vid==VID_SWITCH && pid==PID_SWITCH) ? "Nintendo Switch Pro / 8BitDo" :
                (vid==VID_SWITCH && pid==PID_N64PRO) ? "N64-style USB pad" :
                (vid==VID_NATIVE && pid==PID_NATIVE) ? "8BitDo Native" :
                (vid==VID_XBOX   && pid==PID_XBOX)   ? "Xbox controller" :
                (vid==VID_GCPAD  && pid==PID_GCPAD)  ? "GameCube-style USB pad" :
                (vid==VID_N64    && pid==PID_N64)    ? "N64-style USB pad" :
                (vid==VID_SONY   && pid==PID_DS4)    ? "PS4 controller" : "Unknown";

            gp_log("manager: %s at %s → slot[%d]\n", name, path, slot);
            notify("Ghostcontrol: %s detected — assign user on screen", name);

            /* Claim the slot path before VDA so manager skips it if we retry.
             * Set the assignment gate — released when user confirms (button press). */
            pthread_mutex_lock(&g_slot_lock);
            strncpy(g_slots[slot].dev_path, path, sizeof(g_slots[slot].dev_path)-1);
            g_slots[slot].usb_active = 1; /* tentatively claimed */
            g_slots[slot].confirmed  = 0;
            g_slots[slot].vid = vid;
            g_slots[slot].pid = pid;
            pthread_mutex_unlock(&g_slot_lock);
            g_assign_slot = slot;

            /* Create VDA and force_bind (shows PS5 assignment dialog) */
            int32_t handle = create_vda_for_slot(slot);
            if (handle < 0) {
                gp_log("manager: slot[%d] VDA failed — releasing\n", slot);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].usb_active = 0;
                g_slots[slot].dev_path[0] = '\0';
                g_slots[slot].handle = -1;
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
                g_slots[slot].usb_active=0; g_slots[slot].dev_path[0]='\0';
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
                g_slots[slot].usb_active=0; g_slots[slot].dev_path[0]='\0';
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
            } else {
                pthread_detach(tid);
                gp_log("manager: slot[%d] USB thread started handle=0x%x\n",
                       slot, (uint32_t)handle);
                notify("Ghostcontrol: slot[%d] ready — press a button to assign", slot);
            }
            /* One controller per scan pass — assignment gate blocks the rest
             * until the user confirms this one with a button press. */
            break;
        }

        /* Assignment timeout: if the user never presses a button, release the
         * gate after ~30s so the queue does not stall forever. */
        static int assign_wait = 0;
        if (g_assign_slot >= 0) {
            if (++assign_wait > 15) {  /* 15 * 2s = 30s */
                gp_log("manager: assignment timeout slot[%d] — releasing gate\n", g_assign_slot);
                g_assign_slot = -1;
                assign_wait = 0;
            }
        } else {
            assign_wait = 0;
        }

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
int main(void) {
    int32_t userId=-1, fgUser=-1; int ret;

    ghostpad_status_log_reset();
    gp_log("Ghost-Control v5 starting — %d slots\n", MAX_SLOTS);
    notify("Ghostcontrol by StonedModder — plug in controllers now");

    /* Kill previous instance */
    { int pfd=open(PID_PATH,O_RDONLY);
      if(pfd>=0){char pb[16]={0};read(pfd,pb,15);close(pfd);
        pid_t old=(pid_t)atoi(pb);
        if(old>0&&old!=getpid()){gp_log("Killing prev pid=%d\n",old);kill(old,SIGTERM);usleep(600000);}
      }
      int pfd2=open(PID_PATH,O_WRONLY|O_CREAT|O_TRUNC,0600);
      if(pfd2>=0){char pb[16];snprintf(pb,sizeof(pb),"%d",getpid());write(pfd2,pb,strlen(pb));close(pfd2);}
    }

    load_config();

    /* Init slots */
    for (int s = 0; s < MAX_SLOTS; s++) {
        g_slots[s].handle     = -1;
        g_slots[s].vdi_ready  = 0;
        g_slots[s].usb_active = 0;
        g_slots[s].confirmed  = 0;
        g_slots[s].dev_path[0]= '\0';
    }
    g_assign_slot = -1;

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

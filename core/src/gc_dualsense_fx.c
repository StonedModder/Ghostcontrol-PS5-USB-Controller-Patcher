#define _DEFAULT_SOURCE
#include "gc_dualsense_fx.h"
#include "gc_dualsense_fx_remote.h"
#include "gc_dualsense_fx_scan.h"
#include "dualsense_fx/dualsense_fx.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int32_t sceUserServiceGetLoginUserIdList(int32_t users[4]);
extern unsigned ghostcontrol_copy_owned_vda_handles(int32_t *handles,
                                                     unsigned capacity);
extern int ghostcontrol_setup_busy(void);
extern void ghostpad_status_log(const char *format, ...);
#ifdef __PROSPERO__
#include "gc_types.h"
#include "gc_remote.h"
#include "gc_dualsense_fx_scan.h"
extern int32_t scePadGetHandle(int32_t userId, int32_t type, int32_t index);
extern int32_t scePadOpen(int32_t userId, int32_t type, int32_t index, int32_t bus);
extern int32_t scePadReadState(int32_t handle, ScePadData *data);
extern int32_t scePadSetLightBar(int32_t handle, const void *color);
extern int32_t scePadResetLightBar(int32_t handle);
/* The local path drives a connected pad itself, so it needs the same
 * vibration call the remote path resolves; libScePad exports it. */
typedef struct { uint8_t large_motor, small_motor; } GcFxLocalVibration;
extern int32_t scePadSetVibration(int32_t handle, const GcFxLocalVibration *vibration);
#endif

static pthread_mutex_t g_fx_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_fx_thread;
static atomic_int g_fx_stop;
static int g_fx_started;
static GcConfigStore *g_fx_config;
static GcDualSenseFxStatus g_fx_status;
typedef struct {
    int active;
    unsigned large, small, on_ms, off_ms, repeats;
    uint64_t started_ms;
} GcRumblePlan;
static GcRumblePlan g_rumble;
static uint64_t fx_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

static int fx_library_self_test(void)
{
    unsigned char report[DS_USB_REPORT_LEN];
    const ds_rgb color = {0x12, 0x34, 0x56};
    ds_fx_fill_usb_report(report, color);
    return report[0] == 0x02 && report[1] == 0x02 && report[2] == 0x04 &&
           report[45] == 0x12 && report[46] == 0x34 && report[47] == 0x56;
}

static int runtime_fx_equal(const GcRuntimeConfig *a,
                            const GcRuntimeConfig *b)
{
    return a->dualsense_fx_enabled == b->dualsense_fx_enabled &&
           a->dualsense_fx_effect == b->dualsense_fx_effect &&
           a->dualsense_fx_period_ms == b->dualsense_fx_period_ms &&
           a->dualsense_fx_step_ms == b->dualsense_fx_step_ms &&
           a->dualsense_fx_r == b->dualsense_fx_r &&
           a->dualsense_fx_g == b->dualsense_fx_g &&
           a->dualsense_fx_b == b->dualsense_fx_b;
}

static void build_remote_control(const GcRuntimeConfig *runtime, ds_rgb color,
                                 uint64_t now_ms,
                                 GcDualSenseFxRemoteControl *control)
{
    memset(control, 0, sizeof(*control));
    control->enabled = runtime->dualsense_fx_enabled;
    control->r = (unsigned)color.r;
    control->g = (unsigned)color.g;
    control->b = (unsigned)color.b;
    pthread_mutex_lock(&g_fx_lock);
    if (g_rumble.active) {
        if (!g_rumble.started_ms)
            g_rumble.started_ms = now_ms;
        control->rumble_active = 1;
        const uint64_t cycle = (uint64_t)g_rumble.on_ms + g_rumble.off_ms;
        const uint64_t elapsed = now_ms - g_rumble.started_ms;
        if (g_rumble.repeats && elapsed >= cycle * g_rumble.repeats) {
            g_rumble.active = 0;
        } else if (elapsed % cycle < g_rumble.on_ms) {
            control->rumble_large = g_rumble.large;
            control->rumble_small = g_rumble.small;
        }
    }
    pthread_mutex_unlock(&g_fx_lock);
    for (unsigned i = 0; i < 4; ++i)
        control->users[i] = -1;
    if (sceUserServiceGetLoginUserIdList(control->users) == 0) {
        for (unsigned i = 0; i < 4; ++i)
            if (control->users[i] >= 0) ++control->user_count;
    }
    control->excluded_count = ghostcontrol_copy_owned_vda_handles(
        control->excluded, 4);
    if (control->excluded_count > 4)
        control->excluded_count = 4;
}

#ifdef __PROSPERO__
typedef struct { uint8_t r, g, b; } GcFxLocalColor;

static int local_handle_excluded(const GcDualSenseFxRemoteControl *control,
                                 int32_t handle)
{
    unsigned n = control->excluded_count;
    if (n > 4) n = 4;
    for (unsigned i = 0; i < n; ++i)
        if (control->excluded[i] == handle) return 1;
    return 0;
}

static unsigned local_fx_apply(const GcDualSenseFxRemoteControl *control,
                               int write_probe)
{
    int32_t users[6];
    unsigned user_count = 0, found = 0;
    static unsigned probe_logs;
    static unsigned last_found, last_user_count;
    static int summary_valid, last_enabled;
    static int local_rumble_last;
    for (unsigned i = 0; i < 4; ++i)
        if (control->users[i] >= 0) users[user_count++] = control->users[i];
    users[user_count++] = 0;
    users[user_count++] = (int32_t)0xffffffff;
    for (unsigned u = 0; u < user_count && found < 4; ++u) {
        for (int32_t type = 0; type <= 3 && found < 4; ++type) {
            for (int32_t index = 0; index < 8 && found < 4; ++index) {
                int32_t handle = scePadGetHandle(users[u], type, index);
                int opened = 0;
                if (handle < 0 && write_probe) {
                    handle = scePadOpen(users[u], type, index, 0);
                    opened = handle >= 0;
                }
                if (write_probe && probe_logs < 64) {
                    ++probe_logs;
                    ghostpad_status_log("[GC FX local] user=0x%x type=%d idx=%d handle=%d (0x%x) opened=%d\n",
                                        users[u], type, index, handle, (unsigned)handle, opened);
                }
                if (handle < 0 || local_handle_excluded(control, handle))
                    continue;
                ScePadData sample;
                memset(&sample, 0, sizeof(sample));
                if (scePadReadState(handle, &sample) != 0 || !sample.connected)
                    continue;
                if (control->enabled) {
                    GcFxLocalColor color = {
                        (uint8_t)control->r, (uint8_t)control->g, (uint8_t)control->b
                    };
                    if (scePadSetLightBar(handle, &color) == 0) ++found;
                } else if (scePadResetLightBar(handle) == 0) {
                    ++found;
                }
                /* Rumble is independent of the lightbar switch, matching the
                 * remote path: the plan runs whenever a test asks for it, and
                 * the motors are released once when it ends. */
                if (control->rumble_active) {
                    const GcFxLocalVibration vibration = {
                        (uint8_t)control->rumble_large,
                        (uint8_t)control->rumble_small
                    };
                    (void)scePadSetVibration(handle, &vibration);
                } else if (local_rumble_last) {
                    const GcFxLocalVibration off = {0, 0};
                    (void)scePadSetVibration(handle, &off);
                }
            }
        }
    }
    if (write_probe && (!summary_valid || found != last_found ||
                        user_count != last_user_count ||
                        control->enabled != last_enabled)) {
        ghostpad_status_log("[GC FX local] applied controllers=%u users=%u enabled=%d\n",
                            found, user_count, control->enabled);
        last_found = found;
        last_user_count = user_count;
        last_enabled = control->enabled;
        summary_valid = 1;
    }
    local_rumble_last = control->rumble_active;
    return found;
}

#define GC_DSFX_SCAN_PAGE 0x1000u
#define GC_DSFX_SCAN_SPAN 0x20000u
#define GC_DSFX_SCAN_BIAS 0x20000u

static int shellui_motion_live(pid_t pid, intptr_t base, intptr_t *out_addr,
                               float out[GC_DSFX_MOTION_FLOATS])
{
    static intptr_t cached;
    static pid_t cached_pid;
    static unsigned logged;
    uint8_t page[GC_DSFX_SCAN_PAGE];
    intptr_t start = base + (intptr_t)GC_DSFX_SCAN_BIAS - (intptr_t)(GC_DSFX_SCAN_SPAN / 2);
    size_t pages, i, c;
    if (start < 0x1000)
        start = 0x1000;
    if (cached && cached_pid == pid) {
        float sample[GC_DSFX_MOTION_FLOATS];
        if (gc_remote_read(pid, cached, sample, sizeof(sample)) == 0 &&
            gc_dualsense_fx_motion_shape(sample)) {
            *out_addr = cached;
            memcpy(out, sample, sizeof(sample));
            return 1;
        }
        cached = 0;
        cached_pid = 0;
    }
    pages = GC_DSFX_SCAN_SPAN / GC_DSFX_SCAN_PAGE;
    unsigned live_probes = 0;
    for (i = 0; i < pages && live_probes < 12; ++i) {
        intptr_t addr = start + (intptr_t)(i * GC_DSFX_SCAN_PAGE);
        size_t offs[8];
        size_t n;
        if (gc_remote_read(pid, addr, page, sizeof(page)) != 0)
            continue;
        n = gc_dualsense_fx_motion_candidates(page, sizeof(page), offs, 8);
        for (c = 0; c < n && live_probes < 12; ++c) {
            float first[GC_DSFX_MOTION_FLOATS], second[GC_DSFX_MOTION_FLOATS];
            intptr_t cand = addr + (intptr_t)offs[c];
            if (gc_remote_read(pid, cand, first, sizeof(first)) != 0)
                continue;
            ++live_probes;
            usleep(80000);
            if (gc_remote_read(pid, cand, second, sizeof(second)) != 0)
                continue;
            if (!gc_dualsense_fx_motion_shape(first) ||
                !gc_dualsense_fx_motion_live(first, second))
                continue;
            cached = cand;
            cached_pid = pid;
            if (logged < 8) {
                ++logged;
                ghostpad_status_log("[GC FX shellui] live DualSense motion addr=0x%lx pid=%d\n",
                                    (unsigned long)cand, (int)pid);
            }
            *out_addr = cand;
            memcpy(out, second, sizeof(second));
            return 1;
        }
    }
    if (logged < 8) {
        ++logged;
        ghostpad_status_log("[GC FX shellui] no live DualSense motion pid=%d libpad=0x%lx\n",
                            (int)pid, (unsigned long)base);
    }
    return 0;
}

static unsigned shellui_fx_apply(const GcDualSenseFxRemoteControl *control,
                                 int write_probe,
                                 float motion_values[GC_DSFX_MOTION_FLOATS],
                                 int *motion_valid)
{
    pid_t pids[4];
    size_t count;
    pid_t pid;
    intptr_t base, motion = 0;
    uint32_t pad = 0;
    intptr_t get_handle, open_pad, set_bar, reset_bar;
    intptr_t set_vibration = 0, set_vibration_mode = 0;
    static intptr_t color_addr;
    static pid_t color_pid;
    uint8_t color[4];
    uint8_t vibration[2];
    GcRemoteSession session;
    unsigned found = 0, user_count = 0, u;
    int32_t users[6];
    int32_t index;
    static unsigned call_logs;
    static unsigned rumble_logs;
    static unsigned last_found;
    static intptr_t last_motion;
    static int summary_valid, last_enabled;

    *motion_valid = 0;
    if (!control || !gc_remote_available())
        return 0;
    count = gc_remote_find_pids("SceShellUI", pids, 4);
    if (!count)
        return 0;
    pid = pids[0];
    base = gc_remote_library_base(pid, "libScePad");
    if (!base)
        return 0;
    *motion_valid = shellui_motion_live(pid, base, &motion, motion_values);
    if (gc_remote_get_library(pid, "libScePad", &pad) != 0)
        return 0;
    get_handle = gc_remote_resolve(pid, pad, "scePadGetHandle");
    open_pad = gc_remote_resolve(pid, pad, "scePadOpen");
    set_bar = gc_remote_resolve(pid, pad, "scePadSetLightBar");
    reset_bar = gc_remote_resolve(pid, pad, "scePadResetLightBar");
    set_vibration = gc_remote_resolve(pid, pad, "scePadSetVibration");
    set_vibration_mode = gc_remote_resolve(pid, pad, "scePadSetVibrationMode");
    if (!get_handle || !open_pad || !set_bar || !reset_bar)
        return 0;
    if (color_pid != pid) {
        color_addr = 0;
        color_pid = 0;
    }
    if (!color_addr) {
        intptr_t mapped = 0;
        if (gc_remote_map(pid, 0x4000, &mapped) != 0)
            return 0;
        color_addr = mapped;
        color_pid = pid;
    }
    color[0] = (uint8_t)control->r;
    color[1] = (uint8_t)control->g;
    color[2] = (uint8_t)control->b;
    color[3] = 0;
    vibration[0] = (uint8_t)control->rumble_large;
    vibration[1] = (uint8_t)control->rumble_small;
    if (gc_remote_write(pid, color_addr, color, sizeof(color)) != 0 ||
        gc_remote_write(pid, color_addr + 16, vibration, sizeof(vibration)) != 0)
        return 0;
    memset(&session, 0, sizeof(session));
    if (gc_remote_session_open(&session, pid) != 0)
        return 0;
    for (u = 0; u < 4; ++u)
        if (control->users[u] >= 0)
            users[user_count++] = control->users[u];
    users[user_count++] = 0;
    users[user_count++] = (int32_t)0xffffffff;
    for (u = 0; u < user_count && found < 4; ++u) {
        for (index = 0; index < 8 && found < 4; ++index) {
            uint64_t args[6] = {
                (uint64_t)(uint32_t)users[u], 0, (uint64_t)(uint32_t)index, 0, 0, 0
            };
            int64_t handle = -1;
            intptr_t fn;
            uint64_t call[6];
            int64_t result = -1;
            unsigned ex, e, excluded = 0;
            if (gc_remote_session_call(&session, get_handle, args, &handle) != 0) {
                if (call_logs < 32) {
                    ++call_logs;
                    ghostpad_status_log("[GC FX shellui] GetHandle call failed user=0x%x idx=%d\n",
                                        users[u], index);
                }
                goto shellui_done;
            }
            if (write_probe && call_logs < 32) {
                ++call_logs;
                ghostpad_status_log("[GC FX shellui] GetHandle user=0x%x idx=%d handle=%lld i32=%d (0x%x)\n",
                                    users[u], index, (long long)handle,
                                    (int32_t)handle, (unsigned)(int32_t)handle);
            }
            if (!gc_dualsense_fx_handle_is_valid((int32_t)handle) && index == 0) {
                uint64_t oargs[6] = {
                    (uint64_t)(uint32_t)users[u], 0, 0, 0, 0, 0
                };
                int64_t opened = -1;
                if (gc_remote_session_call(&session, open_pad, oargs, &opened) != 0)
                    goto shellui_done;
                handle = opened;
                if (write_probe && call_logs < 32) {
                    ++call_logs;
                    ghostpad_status_log("[GC FX shellui] Open user=0x%x -> i32=%d (0x%x)\n",
                                        users[u], (int32_t)handle,
                                        (unsigned)(int32_t)handle);
                }
            }
            if (!gc_dualsense_fx_handle_is_valid((int32_t)handle))
                continue;
            ex = control->excluded_count > 4 ? 4 : control->excluded_count;
            for (e = 0; e < ex; ++e)
                if (control->excluded[e] == (int32_t)handle)
                    excluded = 1;
            if (excluded)
                continue;
            fn = control->enabled ? set_bar : reset_bar;
            call[0] = (uint64_t)(uint32_t)(int32_t)handle;
            call[1] = control->enabled ? (uint64_t)color_addr : 0;
            call[2] = call[3] = call[4] = call[5] = 0;
            if (gc_remote_session_call(&session, fn, call, &result) != 0)
                goto shellui_done;
            if (result == 0)
                ++found;
            if (set_vibration) {
                if (set_vibration_mode && control->rumble_active) {
                    uint64_t mode_args[6] = {
                        (uint64_t)(uint32_t)(int32_t)handle,
                        GC_DSFX_VIBRATION_MODE_RUMBLE, 0, 0, 0, 0
                    };
                    (void)gc_remote_session_call(&session, set_vibration_mode,
                                                 mode_args, &result);
                }
                uint64_t vibration_args[6] = {
                    (uint64_t)(uint32_t)(int32_t)handle,
                    (uint64_t)(color_addr + 16), 0, 0, 0, 0
                };
                if (gc_remote_session_call(&session, set_vibration,
                                           vibration_args, &result) != 0)
                    goto shellui_done;
                if (set_vibration_mode && !control->rumble_active) {
                    uint64_t mode_args[6] = {
                        (uint64_t)(uint32_t)(int32_t)handle,
                        GC_DSFX_VIBRATION_MODE_HAPTICS, 0, 0, 0, 0
                    };
                    (void)gc_remote_session_call(&session, set_vibration_mode,
                                                 mode_args, &result);
                }
                if (control->rumble_active && rumble_logs < 8) {
                    ++rumble_logs;
                    ghostpad_status_log("[GC FX shellui] rumble handle=%d large=%u small=%u result=%lld\n",
                                        (int32_t)handle,
                                        control->rumble_large,
                                        control->rumble_small,
                                        (long long)result);
                }
            }
        }
    }
shellui_done:
    (void)gc_remote_session_close(&session);
    if (write_probe && (!summary_valid || found != last_found ||
                        motion != last_motion ||
                        control->enabled != last_enabled)) {
        ghostpad_status_log("[GC FX shellui] applied controllers=%u motion=0x%lx enabled=%d\n",
                            found, (unsigned long)motion, control->enabled);
        last_found = found;
        last_motion = motion;
        last_enabled = control->enabled;
        summary_valid = 1;
    }
    return found;
}
#else
static unsigned local_fx_apply(const GcDualSenseFxRemoteControl *control, int write_probe)
{
    (void)control;
    (void)write_probe;
    return 0;
}
static unsigned shellui_fx_apply(const GcDualSenseFxRemoteControl *control,
                                 int write_probe,
                                 float motion_values[GC_DSFX_MOTION_FLOATS],
                                 int *motion_valid)
{
    (void)control;
    (void)write_probe;
    (void)motion_values;
    *motion_valid = 0;
    return 0;
}
#endif
static void publish_status(const GcRuntimeConfig *runtime, ds_rgb color,
                           const GcDualSenseFxRemoteStatus *remote,
                           int remote_live, const float *motion,
                           int motion_valid)
{
    pthread_mutex_lock(&g_fx_lock);
    g_fx_status.running = g_fx_started && !atomic_load(&g_fx_stop);
    g_fx_status.enabled = runtime->dualsense_fx_enabled;
    g_fx_status.effect = runtime->dualsense_fx_effect;
    g_fx_status.period_ms = runtime->dualsense_fx_period_ms;
    g_fx_status.step_ms = runtime->dualsense_fx_step_ms;
    g_fx_status.r = runtime->dualsense_fx_r;
    g_fx_status.g = runtime->dualsense_fx_g;
    g_fx_status.b = runtime->dualsense_fx_b;
    g_fx_status.current_r = (unsigned)color.r;
    g_fx_status.current_g = (unsigned)color.g;
    g_fx_status.current_b = (unsigned)color.b;
    g_fx_status.controllers = remote_live && remote ? remote->controllers : 0;
    if (remote_live && remote) {
        g_fx_status.writes = remote->writes;
        g_fx_status.errors = remote->errors;
        g_fx_status.resets = remote->resets;
        g_fx_status.last_error = remote->last_error;
    }
    g_fx_status.motion_valid = motion_valid &&
        gc_dualsense_fx_motion_shape(motion);
    if (g_fx_status.motion_valid) {
        memcpy(g_fx_status.quat, motion, sizeof(g_fx_status.quat));
        memcpy(g_fx_status.gravity, motion + 4, sizeof(g_fx_status.gravity));
        memcpy(g_fx_status.gyro, motion + 7, sizeof(g_fx_status.gyro));
    }
    g_fx_status.rumble_active = g_rumble.active;
    g_fx_status.rumble_large = g_rumble.large;
    g_fx_status.rumble_small = g_rumble.small;
    g_fx_status.rumble_on_ms = g_rumble.on_ms;
    g_fx_status.rumble_off_ms = g_rumble.off_ms;
    g_fx_status.rumble_repeats = g_rumble.repeats;
    pthread_mutex_unlock(&g_fx_lock);
}
static void *fx_worker(void *unused)
{
    (void)unused;
    GcRuntimeConfig runtime = {0}, previous = {0};
    uint64_t started_ms = fx_now_ms();
    uint64_t next_config_ms = 0, next_remote_status_ms = 0;
    uint64_t next_remote_start_ms = 0, next_shellui_ms = 0;
    uint64_t next_local_ms = 0, next_probe_ms = 0;
    uint64_t zero_controller_since_ms = 0;
    int have_previous = 0, remote_live = 0;
    int remote_error_reported = 0;
    unsigned local_controllers = 0;
    GcDualSenseFxRemoteStatus remote_status = {0};
    float motion[GC_DSFX_MOTION_FLOATS] = {0};
    int motion_valid = 0;

    while (!atomic_load(&g_fx_stop)) {
        const uint64_t now = fx_now_ms();
        if (now >= next_config_ms) {
            GcRuntimeConfig next;
            if (gc_config_snapshot(g_fx_config, &next, NULL) == 0) {
                if (!have_previous || !runtime_fx_equal(&next, &previous)) {
                    started_ms = now;
                    previous = next;
                    have_previous = 1;
                }
                runtime = next;
            }
            next_config_ms = now + 33;
        }

        ds_fx_cfg cfg = ds_fx_default();
        cfg.effect = runtime.dualsense_fx_effect;
        cfg.period_ms = (int)runtime.dualsense_fx_period_ms;
        cfg.step_ms = (int)runtime.dualsense_fx_step_ms;
        cfg.base.r = (int)runtime.dualsense_fx_r;
        cfg.base.g = (int)runtime.dualsense_fx_g;
        cfg.base.b = (int)runtime.dualsense_fx_b;
        ds_rgb color = ds_fx_colour(&cfg, (unsigned long)(now - started_ms));
        GcDualSenseFxRemoteControl control;
        build_remote_control(&runtime, color, now, &control);
        const int probe = now >= next_probe_ms;
        if (probe)
            next_probe_ms = now + 5000;
        if (now >= next_local_ms) {
            local_controllers = local_fx_apply(&control, probe);
            next_local_ms = now + gc_dualsense_fx_local_scan_delay(
                local_controllers, runtime.dualsense_fx_step_ms);
        }
        unsigned shellui_controllers = 0;
        if (!local_controllers && !ghostcontrol_setup_busy() &&
            now >= next_shellui_ms) {
            shellui_controllers = shellui_fx_apply(&control, probe, motion,
                                                   &motion_valid);
            next_shellui_ms = now + (control.rumble_active ? 33 :
                                     (shellui_controllers ? 250 : 5000));
            if (shellui_controllers) {
                remote_status.controllers = shellui_controllers;
                remote_status.running = 1;
                remote_status.writes += 1;
                remote_status.scans += 1;
            } else if (!remote_live)
                remote_status.controllers = 0;
        }
        if (local_controllers) {
            remote_status.controllers = local_controllers;
            remote_status.running = 1;
            remote_status.writes += 1;
            remote_status.scans += 1;
        }

        if ((local_controllers || shellui_controllers) && control.enabled)
            control.enabled = 0;
        if (!remote_live &&
            !ghostcontrol_setup_busy() &&
            now >= next_remote_start_ms) {
            if (gc_dualsense_fx_remote_start(&control) == 0) {
                remote_live = 1;
                remote_error_reported = 0;
                zero_controller_since_ms = 0;
                next_remote_start_ms = 0;
                memset(&remote_status, 0, sizeof(remote_status));
                ghostpad_status_log("[GC FX] remote pad worker started for %u logged-in user(s)\n",
                                    control.user_count);
            } else {
                next_remote_start_ms = now + 10000;
                if (!remote_error_reported) {
                    remote_error_reported = 1;
                    pthread_mutex_lock(&g_fx_lock);
                ++g_fx_status.errors;
                g_fx_status.last_error = -1001;
                pthread_mutex_unlock(&g_fx_lock);
                    ghostpad_status_log("[GC FX] no pad-owner process currently exposes a DualSense; retrying\n");
                }
            }
        }
        if (remote_live) {
            if (gc_dualsense_fx_remote_update(&control) != 0) {
                ghostpad_status_log("[GC FX] remote pad worker update failed; restarting\n");
                gc_dualsense_fx_remote_stop();
                remote_live = 0;
                zero_controller_since_ms = 0;
                next_remote_start_ms = now + 5000;
                next_remote_status_ms = now + 1000;
            } else if (now >= next_remote_status_ms) {
                if (gc_dualsense_fx_remote_status(&remote_status) != 0 ||
                    !remote_status.running) {
                    ghostpad_status_log("[GC FX] remote pad worker heartbeat failed; restarting\n");
                    gc_dualsense_fx_remote_stop();
                    remote_live = 0;
                    zero_controller_since_ms = 0;
                    next_remote_start_ms = now + 5000;
                } else if (!remote_status.controllers) {
                    if (!zero_controller_since_ms) zero_controller_since_ms = now;
                    else if (now - zero_controller_since_ms >= 1500) {
                        ghostpad_status_log("[GC FX] DualSense handle disappeared; reselecting pad owner\n");
                        gc_dualsense_fx_remote_stop();
                        remote_live = 0;
                        zero_controller_since_ms = 0;
                        next_remote_start_ms = now + 5000;
                    }
                } else {
                    zero_controller_since_ms = 0;
                }
                next_remote_status_ms = now + 250;
            }
        }

        publish_status(&runtime, color, &remote_status,
                       remote_live || remote_status.controllers > 0,
                       motion, motion_valid);
        usleep(runtime.dualsense_fx_enabled ? 33000 : 100000);
    }
    if (remote_live)
        gc_dualsense_fx_remote_stop();
    pthread_mutex_lock(&g_fx_lock);
    g_fx_status.running = 0;
    g_fx_status.enabled = 0;
    g_fx_status.controllers = 0;
    pthread_mutex_unlock(&g_fx_lock);
    return NULL;
}

int gc_dualsense_fx_start(GcConfigStore *config)
{
    if (!config || !fx_library_self_test())
        return -1;
    pthread_mutex_lock(&g_fx_lock);
    if (g_fx_started) {
        pthread_mutex_unlock(&g_fx_lock);
        return 0;
    }
    memset(&g_fx_status, 0, sizeof(g_fx_status));
    memset(&g_rumble, 0, sizeof(g_rumble));
    g_fx_config = config;
    atomic_store(&g_fx_stop, 0);
    g_fx_started = 1;
    g_fx_status.running = 1;
    pthread_mutex_unlock(&g_fx_lock);
    if (pthread_create(&g_fx_thread, NULL, fx_worker, NULL) != 0) {
        pthread_mutex_lock(&g_fx_lock);
        g_fx_started = 0;
        g_fx_status.running = 0;
        g_fx_config = NULL;
        pthread_mutex_unlock(&g_fx_lock);
        return -1;
    }
    return 0;
}
void gc_dualsense_fx_stop(void)
{
    pthread_mutex_lock(&g_fx_lock);
    const int started = g_fx_started;
    if (started)
        atomic_store(&g_fx_stop, 1);
    pthread_mutex_unlock(&g_fx_lock);
    if (!started)
        return;
    pthread_join(g_fx_thread, NULL);
    pthread_mutex_lock(&g_fx_lock);
    g_fx_started = 0;
    g_fx_config = NULL;
    pthread_mutex_unlock(&g_fx_lock);
}

void gc_dualsense_fx_status(GcDualSenseFxStatus *status)
{
    if (!status)
        return;
    pthread_mutex_lock(&g_fx_lock);
    *status = g_fx_status;
    pthread_mutex_unlock(&g_fx_lock);
}

int gc_dualsense_fx_rumble(unsigned large, unsigned small,
                           unsigned on_ms, unsigned off_ms,
                           unsigned repeats)
{
    if (large > 255 || small > 255 || !on_ms || on_ms > 60000 ||
        off_ms > 60000 || repeats > 1000 || (!large && !small))
        return -1;
    pthread_mutex_lock(&g_fx_lock);
    g_rumble = (GcRumblePlan){1, large, small, on_ms, off_ms, repeats, 0};
    pthread_mutex_unlock(&g_fx_lock);
    return 0;
}

void gc_dualsense_fx_rumble_stop(void)
{
    pthread_mutex_lock(&g_fx_lock);
    g_rumble.active = 0;
    pthread_mutex_unlock(&g_fx_lock);
}

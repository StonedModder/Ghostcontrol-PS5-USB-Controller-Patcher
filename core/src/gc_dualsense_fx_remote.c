/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "gc_dualsense_fx_remote.h"
#include "gc_remote.h"
#include "gc_types.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

extern void ghostpad_status_log(const char *format, ...);
#define fx_remote_log(...) ghostpad_status_log("[GC FX remote] " __VA_ARGS__)

#define GC_DSFX_PAGE_SIZE 0x4000u
#define GC_DSFX_MAPPING_SIZE (GC_DSFX_PAGE_SIZE * 2u)
#define GC_DSFX_MAX_HANDLES 4u
#define GC_DSFX_SCAN_TICKS 500u
#define GC_DSFX_OWNER_TICKS 500u
#define GC_DSFX_IS_DUALSENSE(vendor, product) \
    ((vendor) == UINT16_C(0x054c) && \
     ((product) == UINT16_C(0x0ce6) || (product) == UINT16_C(0x0df2)))
#define GC_DSFX_CONNECTED_PAD_IS_DUALSENSE(have_usb, vendor, product) \
    (!(have_usb) || GC_DSFX_IS_DUALSENSE((vendor), (product)) || \
     ((vendor) == 0 && (product) == 0))
#define GC_DSFX_HANDLE_VALID(handle) ((handle) >= 0)
#define GC_DSFX_USER_UNASSIGNED ((int32_t)0xffffffff)

typedef struct { uint8_t r, g, b; } GcFxLightBar;
typedef struct {
    volatile GcDualSenseFxRemoteControl control;
    volatile uint64_t owner_deadline_ns;
    volatile uint32_t stop;
    volatile uint32_t ready;
    volatile GcDualSenseFxRemoteStatus status;
    intptr_t fp_get_handle;
    intptr_t fp_read_state;
    intptr_t fp_get_device_id;
    intptr_t fp_get_device_info;
    intptr_t fp_set_lightbar;
    intptr_t fp_reset_lightbar;
    intptr_t fp_set_vibration_mode;
    intptr_t fp_set_vibration;
    intptr_t fp_usleep;
    intptr_t fp_kill;
    intptr_t fp_clock_gettime;
    uint64_t thread_id;
    int32_t owner_pid;
    uint32_t interval_us;
} GcFxRemoteArgs;

_Static_assert(sizeof(GcFxRemoteArgs) < GC_DSFX_PAGE_SIZE,
               "FX remote args exceed RW page");
typedef int32_t (*GcFxGetHandle)(int32_t, int32_t, int32_t);
typedef int32_t (*GcFxReadState)(int32_t, ScePadData *);
typedef int32_t (*GcFxGetDeviceId)(int32_t, int32_t *);
typedef int32_t (*GcFxGetDeviceInfo)(int32_t, void *);
typedef int32_t (*GcFxSetLightBar)(int32_t, const GcFxLightBar *);
typedef int32_t (*GcFxResetLightBar)(int32_t);
typedef struct { uint8_t large_motor, small_motor; } GcFxVibration;
typedef int32_t (*GcFxSetVibrationMode)(int32_t, int32_t);
typedef int32_t (*GcFxSetVibration)(int32_t, const GcFxVibration *);
typedef int (*GcFxUsleep)(uint32_t);
typedef int (*GcFxKill)(int32_t, int32_t);
typedef int (*GcFxClockGettime)(clockid_t, struct timespec *);

int gc_dualsense_fx_is_sony_dualsense(uint16_t vendor, uint16_t product)
{
    return GC_DSFX_IS_DUALSENSE(vendor, product);
}

int gc_dualsense_fx_connected_pad_is_dualsense(int have_usb_identity,
                                               uint16_t vendor,
                                               uint16_t product)
{
    return GC_DSFX_CONNECTED_PAD_IS_DUALSENSE(have_usb_identity, vendor, product);
}

int gc_dualsense_fx_handle_is_valid(int32_t handle)
{
    return GC_DSFX_HANDLE_VALID(handle);
}

int gc_dualsense_fx_remote_status_usable(const GcDualSenseFxRemoteStatus *status)
{
    return status && status->running && status->scans > 0 &&
           status->controllers > 0;
}

extern void *gc_dualsense_fx_remote_stub(void *argument);
extern void gc_dualsense_fx_remote_stub_end(void);
__attribute__((noinline, used, section(".text.gc_dsfx_remote.1")))
void *gc_dualsense_fx_remote_stub(void *argument)
{
    GcFxRemoteArgs *args = argument;
    GcFxGetHandle get_handle = (GcFxGetHandle)(uintptr_t)args->fp_get_handle;
    GcFxReadState read_state = (GcFxReadState)(uintptr_t)args->fp_read_state;
    GcFxGetDeviceId get_device_id = (GcFxGetDeviceId)(uintptr_t)args->fp_get_device_id;
    GcFxGetDeviceInfo get_device_info = (GcFxGetDeviceInfo)(uintptr_t)args->fp_get_device_info;
    GcFxSetLightBar set_lightbar = (GcFxSetLightBar)(uintptr_t)args->fp_set_lightbar;
    GcFxResetLightBar reset_lightbar = (GcFxResetLightBar)(uintptr_t)args->fp_reset_lightbar;
    GcFxSetVibrationMode set_vibration_mode =
        (GcFxSetVibrationMode)(uintptr_t)args->fp_set_vibration_mode;
    GcFxSetVibration set_vibration = (GcFxSetVibration)(uintptr_t)args->fp_set_vibration;
    GcFxUsleep sleep_us = (GcFxUsleep)(uintptr_t)args->fp_usleep;
    GcFxKill kill_process = (GcFxKill)(uintptr_t)args->fp_kill;
    GcFxClockGettime get_time = (GcFxClockGettime)(uintptr_t)args->fp_clock_gettime;
    int32_t handles[GC_DSFX_MAX_HANDLES] = {-1, -1, -1, -1};
    unsigned handle_count = 0, scan_tick = GC_DSFX_SCAN_TICKS;
    unsigned owner_tick = GC_DSFX_OWNER_TICKS;
    unsigned old_large = 0, old_small = 0;
    int was_enabled = 0;

    args->status.running = 1;
    args->ready = 1;
    while (!args->stop) {
        if (++scan_tick >= GC_DSFX_SCAN_TICKS) {
            scan_tick = 0;
            int32_t found[GC_DSFX_MAX_HANDLES] = {-1, -1, -1, -1};
            unsigned found_count = 0;
            unsigned handles_seen = 0;
            unsigned connected_handles = 0;
            unsigned user_count = args->control.user_count;
            if (user_count > 4) user_count = 4;
            for (unsigned u = 0; u < user_count + 2 && found_count < GC_DSFX_MAX_HANDLES; ++u) {
                int32_t user;
                if (u < user_count) {
                    user = args->control.users[u];
                    if (user < 0) continue;
                } else if (u == user_count)
                    user = 0;
                else
                    user = GC_DSFX_USER_UNASSIGNED;
                for (int32_t index = 0; index < 8 && found_count < GC_DSFX_MAX_HANDLES; ++index) {
                    const int32_t handle = get_handle(user, 0, index);
                    if (!GC_DSFX_HANDLE_VALID(handle)) continue;
                    ++handles_seen;
                    int excluded = 0;
                    unsigned excluded_count = args->control.excluded_count;
                    if (excluded_count > 4) excluded_count = 4;
                    for (unsigned e = 0; e < excluded_count; ++e)
                        if (args->control.excluded[e] == handle) excluded = 1;
                    if (excluded) continue;
                    ScePadData sample;
                    int32_t device_id = -1;
                    unsigned char info[256];
                    for (unsigned z = 0; z < sizeof(info); ++z) info[z] = 0;
                    for (unsigned z = 0; z < sizeof(sample); ++z)
                        ((unsigned char *)&sample)[z] = 0;
                    if (read_state(handle, &sample) != 0 || !sample.connected)
                        continue;
                    ++connected_handles;
                    int have_usb = 0;
                    uint16_t vendor = 0, product = 0;
                    if (get_device_id(handle, &device_id) == 0 && device_id >= 0 &&
                        get_device_info(device_id, info) == 0) {
                        have_usb = 1;
                        vendor = (uint16_t)info[16] |
                            (uint16_t)((uint16_t)info[17] << 8);
                        product = (uint16_t)info[18] |
                            (uint16_t)((uint16_t)info[19] << 8);
                    }
                    if (!GC_DSFX_CONNECTED_PAD_IS_DUALSENSE(have_usb, vendor, product))
                        continue;
                    int duplicate = 0;
                    for (unsigned h = 0; h < found_count; ++h)
                        if (found[h] == handle) duplicate = 1;
                    if (!duplicate) found[found_count++] = handle;
                }
            }
            if (was_enabled) {
                for (unsigned old = 0; old < handle_count; ++old) {
                    int still_present = 0;
                    for (unsigned now = 0; now < found_count; ++now)
                        if (handles[old] == found[now]) still_present = 1;
                    if (!still_present) {
                        const int result = reset_lightbar(handles[old]);
                        if (result == 0) ++args->status.resets;
                        else { ++args->status.errors; args->status.last_error = result; }
                    }
                }
            }
            int handles_changed = handle_count != found_count;
            for (unsigned h = 0; !handles_changed && h < found_count; ++h)
                handles_changed = handles[h] != found[h];
            handle_count = found_count;
            for (unsigned h = 0; h < GC_DSFX_MAX_HANDLES; ++h)
                handles[h] = h < found_count ? found[h] : -1;
            args->status.controllers = handle_count;
            args->status.handles_seen = handles_seen;
            args->status.connected_handles = connected_handles;
            ++args->status.scans;
            if (handles_changed)
                old_large = old_small = 256;
        }

        const unsigned large = args->control.rumble_large > 255
            ? 255 : args->control.rumble_large;
        const unsigned small = args->control.rumble_small > 255
            ? 255 : args->control.rumble_small;
        if (large != old_large || small != old_small) {
            const GcFxVibration vibration = {(uint8_t)large, (uint8_t)small};
            for (unsigned h = 0; h < handle_count; ++h) {
                int result;
                if (large || small)
                    result = set_vibration_mode(handles[h],
                                                GC_DSFX_VIBRATION_MODE_RUMBLE);
                else
                    result = 0;
                if (result != 0) {
                    ++args->status.errors;
                    args->status.last_error = result;
                    continue;
                }
                result = set_vibration(handles[h], &vibration);
                if (result == 0) ++args->status.writes;
                else { ++args->status.errors; args->status.last_error = result; }
                if (!large && !small)
                    (void)set_vibration_mode(handles[h],
                                             GC_DSFX_VIBRATION_MODE_HAPTICS);
            }
            old_large = large;
            old_small = small;
        }

        if (args->control.enabled) {
            GcFxLightBar color = {
                (uint8_t)args->control.r,
                (uint8_t)args->control.g,
                (uint8_t)args->control.b
            };
            for (unsigned h = 0; h < handle_count; ++h) {
                const int result = set_lightbar(handles[h], &color);
                if (result == 0) ++args->status.writes;
                else { ++args->status.errors; args->status.last_error = result; }
            }
            was_enabled = 1;
        } else if (was_enabled) {
            for (unsigned h = 0; h < handle_count; ++h) {
                const int result = reset_lightbar(handles[h]);
                if (result == 0) ++args->status.resets;
                else { ++args->status.errors; args->status.last_error = result; }
            }
            was_enabled = 0;
        }

        if (++owner_tick >= GC_DSFX_OWNER_TICKS) {
            owner_tick = 0;
            struct timespec now;
            if (kill_process(args->owner_pid, 0) != 0 ||
                get_time(CLOCK_MONOTONIC, &now) != 0 ||
                (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec >=
                    args->owner_deadline_ns)
                break;
        }
        sleep_us(args->interval_us);
    }
    if (was_enabled) {
        for (unsigned h = 0; h < handle_count; ++h) {
            const int result = reset_lightbar(handles[h]);
            if (result == 0) ++args->status.resets;
            else { ++args->status.errors; args->status.last_error = result; }
        }
    }
    if (old_large || old_small) {
        const GcFxVibration off = {0, 0};
        for (unsigned h = 0; h < handle_count; ++h) {
            (void)set_vibration(handles[h], &off);
            (void)set_vibration_mode(handles[h],
                                     GC_DSFX_VIBRATION_MODE_HAPTICS);
        }
    }
    args->status.running = 0;
    args->ready = 2;
    return NULL;
}

__attribute__((noinline, used, section(".text.gc_dsfx_remote.2")))
void gc_dualsense_fx_remote_stub_end(void)
{
}

typedef struct {
    intptr_t get_handle, read_state;
    intptr_t get_device_id, get_device_info;
    intptr_t set_lightbar, reset_lightbar, set_vibration_mode, set_vibration;
    intptr_t usleep, kill, clock_gettime;
    intptr_t pthread_create, pthread_join;
} GcFxRemoteSymbols;
typedef struct {
    pid_t pid;
    intptr_t mapping;
    intptr_t args_address;
    size_t mapping_size;
    uint64_t thread_id;
    int launched;
    int joined;
    GcRemoteSession trace;
} GcFxRemoteState;

static GcFxRemoteState g_remote;

static int remote_call(GcRemoteSession *session, intptr_t function,
                       uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       int64_t *result)
{
    uint64_t args[6] = {a1, a2, a3, a4, 0, 0};
    return gc_remote_session_call(session, function, args, result);
}

static int resolve_symbols(pid_t pid, GcFxRemoteSymbols *symbols)
{
    uint32_t pad = 0, kernel = 0, pthread = 0;
    if (gc_remote_get_library(pid, "libScePad", &pad) != 0)
        return -1;
    if (gc_remote_get_library(pid, "libkernel_sys", &kernel) != 0 &&
        gc_remote_get_library(pid, "libkernel", &kernel) != 0)
        return -1;
    (void)gc_remote_get_library(pid, "libpthread", &pthread);
    memset(symbols, 0, sizeof(*symbols));
#define RESOLVE_PAD(field, name) symbols->field = gc_remote_resolve(pid, pad, name)
    RESOLVE_PAD(get_handle, "scePadGetHandle");
    RESOLVE_PAD(read_state, "scePadReadState");
    RESOLVE_PAD(get_device_id, "scePadGetDeviceId");
    RESOLVE_PAD(get_device_info, "scePadGetDeviceInfo");
    RESOLVE_PAD(set_lightbar, "scePadSetLightBar");
    RESOLVE_PAD(reset_lightbar, "scePadResetLightBar");
    RESOLVE_PAD(set_vibration_mode, "scePadSetVibrationMode");
    RESOLVE_PAD(set_vibration, "scePadSetVibration");
#undef RESOLVE_PAD
    symbols->usleep = gc_remote_resolve(pid, kernel, "usleep");
    symbols->kill = gc_remote_resolve(pid, kernel, "kill");
    symbols->clock_gettime = gc_remote_resolve(pid, kernel, "clock_gettime");
    symbols->pthread_create = gc_remote_resolve(pid, kernel, "pthread_create");
    symbols->pthread_join = gc_remote_resolve(pid, kernel, "pthread_join");
    if ((!symbols->pthread_create || !symbols->pthread_join) && pthread) {
        if (!symbols->pthread_create)
            symbols->pthread_create = gc_remote_resolve(pid, pthread, "pthread_create");
        if (!symbols->pthread_join)
            symbols->pthread_join = gc_remote_resolve(pid, pthread, "pthread_join");
    }
    return symbols->get_handle && symbols->read_state &&
           symbols->get_device_id && symbols->get_device_info &&
           symbols->set_lightbar && symbols->reset_lightbar &&
           symbols->set_vibration_mode && symbols->set_vibration &&
           symbols->usleep && symbols->kill && symbols->clock_gettime &&
           symbols->pthread_create && symbols->pthread_join ? 0 : -1;
}

static void fill_args(GcFxRemoteArgs *args, const GcFxRemoteSymbols *symbols,
                      const GcDualSenseFxRemoteControl *control)
{
    memset(args, 0, sizeof(*args));
    args->control = *control;
    args->fp_get_handle = symbols->get_handle;
    args->fp_read_state = symbols->read_state;
    args->fp_get_device_id = symbols->get_device_id;
    args->fp_get_device_info = symbols->get_device_info;
    args->fp_set_lightbar = symbols->set_lightbar;
    args->fp_reset_lightbar = symbols->reset_lightbar;
    args->fp_set_vibration_mode = symbols->set_vibration_mode;
    args->fp_set_vibration = symbols->set_vibration;
    args->fp_usleep = symbols->usleep;
    args->fp_kill = symbols->kill;
    args->fp_clock_gettime = symbols->clock_gettime;
    args->owner_pid = (int32_t)getpid();
    args->interval_us = 2000;
}
static uint64_t owner_deadline_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec + UINT64_C(3000000000);
}

static int start_in_pid(pid_t pid, const GcDualSenseFxRemoteControl *control)
{
    GcFxRemoteSymbols symbols;
    GcFxRemoteArgs args;
    if (resolve_symbols(pid, &symbols) != 0)
        return -1;
    memset(&g_remote, 0, sizeof(g_remote));
    g_remote.pid = pid;
    g_remote.mapping_size = GC_DSFX_MAPPING_SIZE;
    if (gc_remote_map(pid, g_remote.mapping_size, &g_remote.mapping) != 0)
        goto fail;
    g_remote.args_address = g_remote.mapping + GC_DSFX_PAGE_SIZE;
    if (gc_remote_session_open(&g_remote.trace, pid) != 0)
        goto fail;
    fill_args(&args, &symbols, control);
    args.owner_deadline_ns = owner_deadline_ns();
    if (!args.owner_deadline_ns)
        goto fail;
    size_t code_size = (uintptr_t)gc_dualsense_fx_remote_stub_end -
                       (uintptr_t)gc_dualsense_fx_remote_stub;
    if (!code_size || code_size > GC_DSFX_PAGE_SIZE)
        goto fail;
    if (gc_remote_write(pid, g_remote.mapping,
                        gc_dualsense_fx_remote_stub, code_size) != 0 ||
        gc_remote_write(pid, g_remote.args_address, &args, sizeof(args)) != 0 ||
        gc_remote_protect(pid, g_remote.mapping, GC_DSFX_PAGE_SIZE,
                          PROT_READ | PROT_EXEC) != 0)
        goto fail;

    int64_t create_result = -1;
    g_remote.launched = 1;
    if (remote_call(&g_remote.trace, symbols.pthread_create,
                    (uint64_t)(g_remote.args_address + offsetof(GcFxRemoteArgs, thread_id)),
                    0, (uint64_t)g_remote.mapping, (uint64_t)g_remote.args_address,
                    &create_result) != 0 || create_result != 0)
        goto fail;
    if (gc_remote_read(pid,
                       g_remote.args_address + offsetof(GcFxRemoteArgs, thread_id),
                       &g_remote.thread_id, sizeof(g_remote.thread_id)) != 0 ||
        !g_remote.thread_id || gc_remote_session_close(&g_remote.trace) != 0)
        goto fail;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        uint32_t ready = 0;
        if (gc_remote_read(pid,
                           g_remote.args_address + offsetof(GcFxRemoteArgs, ready),
                           &ready, sizeof(ready)) == 0 && ready == 1) {
            fx_remote_log("worker ready pid=%d\n", pid);
            return 0;
        }
        usleep(10000);
    }
fail:
    fx_remote_log("worker start failed pid=%d\n", pid);
    if (g_remote.trace.pid)
        (void)gc_remote_session_close(&g_remote.trace);
    if (!g_remote.launched && g_remote.mapping)
        (void)gc_remote_unmap(pid, g_remote.mapping, g_remote.mapping_size);
    if (!g_remote.launched)
        memset(&g_remote, 0, sizeof(g_remote));
    return -1;
}

int gc_dualsense_fx_remote_start(const GcDualSenseFxRemoteControl *control)
{
    if (!control || !gc_remote_available())
        return -1;
    if (g_remote.pid)
        return gc_dualsense_fx_remote_update(control);
    static const char *const targets[] = {
        "SceRemotePlay", "SceShellCore", "SceHidControl"
    };
    pid_t pids[8];
    for (unsigned t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        size_t count = gc_remote_find_pids(targets[t], pids, 8);
        for (size_t i = 0; i < count; ++i) {
            if (start_in_pid(pids[i], control) == 0) {
                GcDualSenseFxRemoteStatus probe;
                memset(&probe, 0, sizeof(probe));
                for (unsigned attempt = 0; attempt < 80; ++attempt) {
                    if (gc_dualsense_fx_remote_status(&probe) == 0 && probe.scans > 0)
                        break;
                    usleep(25000);
                }
                if (gc_dualsense_fx_remote_status_usable(&probe)) {
                    fx_remote_log("using %s pid=%d scans=%llu handles=%u connected=%u dualsense=%u\n",
                                  targets[t], pids[i], (unsigned long long)probe.scans,
                                  probe.handles_seen, probe.connected_handles, probe.controllers);
                    return 0;
                }
                fx_remote_log("candidate %s pid=%d rejected scans=%llu handles=%u connected=%u dualsense=%u\n",
                              targets[t], pids[i], (unsigned long long)probe.scans,
                              probe.handles_seen, probe.connected_handles, probe.controllers);
                gc_dualsense_fx_remote_stop();
                continue;
            }
            if (g_remote.launched)
                return -1;
        }
    }
    fx_remote_log("no attachable pad service found\n");
    return -1;
}

int gc_dualsense_fx_remote_update(const GcDualSenseFxRemoteControl *control)
{
    if (!control || !g_remote.pid || !g_remote.args_address)
        return -1;
    const uint64_t deadline = owner_deadline_ns();
    if (!deadline)
        return -1;
    if (gc_remote_write(g_remote.pid,
                        g_remote.args_address + offsetof(GcFxRemoteArgs, control),
                        control, sizeof(*control)) != 0 ||
        gc_remote_write(g_remote.pid,
                        g_remote.args_address + offsetof(GcFxRemoteArgs, owner_deadline_ns),
                        &deadline, sizeof(deadline)) != 0)
        return -1;
    return 0;
}

int gc_dualsense_fx_remote_status(GcDualSenseFxRemoteStatus *status)
{
    if (!status || !g_remote.pid || !g_remote.args_address)
        return -1;
    GcDualSenseFxRemoteStatus remote_status;
    if (gc_remote_read(g_remote.pid,
                       g_remote.args_address + offsetof(GcFxRemoteArgs, status),
                       &remote_status, sizeof(remote_status)) != 0)
        return -1;
    *status = remote_status;
    return 0;
}

void gc_dualsense_fx_remote_stop(void)
{
    if (!g_remote.pid)
        return;
    uint32_t stop = 1, ready = 0;
    (void)gc_remote_write(g_remote.pid,
                          g_remote.args_address + offsetof(GcFxRemoteArgs, stop),
                          &stop, sizeof(stop));
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (gc_remote_read(g_remote.pid,
                           g_remote.args_address + offsetof(GcFxRemoteArgs, ready),
                           &ready, sizeof(ready)) == 0 && ready == 2)
            break;
        usleep(10000);
    }
    GcFxRemoteSymbols symbols;
    if (ready == 2 && g_remote.thread_id &&
        resolve_symbols(g_remote.pid, &symbols) == 0 &&
        gc_remote_session_open(&g_remote.trace, g_remote.pid) == 0) {
        int64_t result = -1;
        if (remote_call(&g_remote.trace, symbols.pthread_join,
                        g_remote.thread_id, 0, 0, 0, &result) == 0 && result == 0)
            g_remote.joined = 1;
        (void)gc_remote_session_close(&g_remote.trace);
    }
    if (ready == 2 && g_remote.mapping)
        (void)gc_remote_unmap(g_remote.pid, g_remote.mapping, g_remote.mapping_size);
    fx_remote_log("worker stopped pid=%d ready=%u joined=%d\n",
                  g_remote.pid, ready, g_remote.joined);
    memset(&g_remote, 0, sizeof(g_remote));
}

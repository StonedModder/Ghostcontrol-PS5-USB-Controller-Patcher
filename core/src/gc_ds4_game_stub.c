/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_ds4_game_stub.h"
#define INLINE static __attribute__((always_inline)) inline
#define STUB __attribute__((noinline, used, section(".text.gc_ds4_game")))

INLINE int snapshot_pad(GcDs4GameArgs *args, int32_t handle, ScePadData *pad)
{
    if (!args || args->magic != GC_DS4_GAME_MAGIC ||
        args->layout != GC_DS4_GAME_LAYOUT || args->pad_size != sizeof(*pad) ||
        !__atomic_load_n(&args->active, __ATOMIC_ACQUIRE)) return 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t epoch = __atomic_load_n(&args->routing_epoch, __ATOMIC_ACQUIRE);
        uint32_t sequence = __atomic_load_n(&args->sequence, __ATOMIC_ACQUIRE);
        if (epoch & 1u) continue;
        uint32_t count = args->route_count;
        if (count > GC_DS4_MAX_ROUTES) return 0;
        int route = -1;
        for (uint32_t slot = 0; slot < count; ++slot) {
            /* A local pad index is never an identity across users. */
            if (args->routes[slot].pad_handle == handle) {
                if (route >= 0) return 0;
                route = (int)slot;
            }
        }
        if (route < 0) return 0;
        const volatile uint8_t *src = (const volatile uint8_t *)
            &args->banks[sequence & 1u].pads[route];
        volatile uint8_t *dst = (volatile uint8_t *)pad;
        for (size_t i = 0; i < sizeof(*pad); ++i) dst[i] = src[i];
        if (sequence != __atomic_load_n(&args->sequence, __ATOMIC_ACQUIRE) ||
            epoch != __atomic_load_n(&args->routing_epoch, __ATOMIC_ACQUIRE))
            continue;
        if (!pad->connected || !args->clock_gettime_fn) return 0;
        int (*get_time)(clockid_t, struct timespec *) =
            (void *)(uintptr_t)args->clock_gettime_fn;
        struct timespec now;
        uint64_t deadline = __atomic_load_n(&args->deadline_ns, __ATOMIC_ACQUIRE);
        if (get_time(CLOCK_MONOTONIC, &now) != 0 ||
            (uint64_t)now.tv_sec * UINT64_C(1000000000) +
                (uint64_t)now.tv_nsec >= deadline) {
            if (deadline != __atomic_load_n(&args->deadline_ns, __ATOMIC_ACQUIRE))
                continue;
            if (__atomic_exchange_n(&args->active, 0, __ATOMIC_ACQ_REL))
                __atomic_fetch_add(&args->lease_expirations, 1, __ATOMIC_RELAXED);
            return 0;
        }
        return __atomic_load_n(&args->active, __ATOMIC_ACQUIRE) != 0 &&
            epoch == __atomic_load_n(&args->routing_epoch, __ATOMIC_ACQUIRE) &&
            sequence == __atomic_load_n(&args->sequence, __ATOMIC_ACQUIRE);
    }
    __atomic_fetch_add(&args->snapshot_fallbacks, 1, __ATOMIC_RELAXED);
    return 0;
}

INLINE int copy_pad(GcDs4GameArgs *args, int32_t handle, void *output, int kind)
{
    ScePadData sample;
    if (!output || !snapshot_pad(args, handle, &sample)) return 0;
    const volatile uint8_t *src = (const volatile uint8_t *)&sample;
    volatile uint8_t *dst = output;
    for (size_t i = 0; i < sizeof(sample); ++i) dst[i] = src[i];
    __atomic_fetch_add(&args->calls[kind], 1, __ATOMIC_RELAXED);
    return 1;
}

#define STATE_STUB(name, kind) \
STUB int32_t name(int32_t handle, void *output, GcDs4GameArgs *args) { \
    int32_t (*original)(int32_t, void *) = (void *)(uintptr_t)args->originals[kind]; \
    int32_t native = original(handle, output); \
    return copy_pad(args, handle, output, kind) ? 0 : native; \
}
#define READ_STUB(name, kind) \
STUB int32_t name(int32_t handle, void *output, int32_t count, GcDs4GameArgs *args) { \
    int32_t (*original)(int32_t, void *, int32_t) = (void *)(uintptr_t)args->originals[kind]; \
    int32_t native = original(handle, output, count); \
    return count > 0 && copy_pad(args, handle, output, kind) ? 1 : native; \
}

STATE_STUB(gc_ds4_game_read_state_stub, 0)
STATE_STUB(gc_ds4_game_read_state_ext_stub, 1)
READ_STUB(gc_ds4_game_read_stub, 2)
READ_STUB(gc_ds4_game_read_ext_stub, 3)
STATE_STUB(gc_ds4_game_data_internal_stub, 4)

STUB int32_t gc_ds4_game_controller_info_stub(
    int32_t handle, void *output, GcDs4GameArgs *args)
{
    int32_t (*original)(int32_t, void *) = (void *)(uintptr_t)args->originals[5];
    int32_t native = original(handle, output);
    ScePadData sample;
    if (!output || !snapshot_pad(args, handle, &sample)) return native;
    volatile uint8_t *info = output;
    info[0] = 0; info[1] = 0; info[2] = 0x80; info[3] = 0x3f;
    info[4] = 0x80; info[5] = 7; info[6] = 0xae; info[7] = 3;
    info[8] = 2; info[9] = 2; info[10] = 0; info[11] = 1;
    info[12] = 1;
    for (unsigned i = 13; i < 28; ++i) info[i] = 0;
    __atomic_fetch_add(&args->calls[5], 1, __ATOMIC_RELAXED);
    return 0;
}

STUB int32_t gc_ds4_game_set_vibration_stub(
    int32_t handle, const void *parameter, GcDs4GameArgs *args)
{
    if (!args) return -1;
    int32_t (*original)(int32_t, const void *) =
        (void *)(uintptr_t)args->originals[6];
    if (!original) return -1;
    int32_t result = original(handle, parameter);
    if (result || !parameter || !args || args->magic != GC_DS4_GAME_MAGIC ||
        args->layout != GC_DS4_GAME_LAYOUT ||
        !__atomic_load_n(&args->active, __ATOMIC_ACQUIRE)) return result;
    const volatile uint8_t *vibration = parameter;
    uint8_t large = vibration[0], small = vibration[1];
    for (uint32_t route = 0; route < args->route_count; ++route) {
        if (args->routes[route].pad_handle != handle) continue;
        GcDs4GameRumble *state = &args->rumble[route];
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            uint32_t sequence = __atomic_load_n(&state->sequence, __ATOMIC_ACQUIRE);
            if (sequence & 1u) continue;
            if (__atomic_load_n(&state->large, __ATOMIC_RELAXED) == large &&
                __atomic_load_n(&state->small, __ATOMIC_RELAXED) == small) break;
            uint32_t writing = sequence + 1u;
            if (!__atomic_compare_exchange_n(&state->sequence, &sequence, writing,
                                              0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;
            __atomic_store_n(&state->large, large, __ATOMIC_RELAXED);
            __atomic_store_n(&state->small, small, __ATOMIC_RELAXED);
            __atomic_store_n(&state->sequence, writing + 1u, __ATOMIC_RELEASE);
            __atomic_fetch_add(&args->calls[6], 1, __ATOMIC_RELAXED);
            break;
        }
        break;
    }
    return result;
}
STUB void gc_ds4_game_stub_end(void) {}

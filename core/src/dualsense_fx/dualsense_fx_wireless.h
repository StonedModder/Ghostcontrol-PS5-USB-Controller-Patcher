/* dualsense_fx_wireless.h — acquire a wireless DualSense and set its lightbar.
 * Header-only. No allocation, no libm, no I/O, no PS5 headers.
 * Pair with dualsense_fx.h for the colour/effect side.
 * Public domain / CC0.
 *
 * Confirmed on PS5 5.50 (Ghostcontrol test.42):
 *   GetHandle(user, 0, 0) → 0x80920008  (not connected)
 *   Open(user, 0, 0)      → 0x34a0401   (real handle)
 *   SetLightBar(handle, rgb) → 0        (lightbar updates over Bluetooth)
 *
 * Call Open/SetLightBar from the process that owns the pad (SceShellUI),
 * on a stopped thread (pt_call / PT_ATTACH). Do not inject a pthread that
 * calls libScePad IPC — that collides with ShellUI's pad socket.
 *
 *     #include "dualsense_fx.h"
 *     #include "dualsense_fx_wireless.h"
 *
 *     ds_rgb c = ds_fx_colour(&fx, elapsed_ms);
 *     int32_t h = ds_fx_pad_acquire(ctx, get_handle, open_pad, user, 0, 0);
 *     if (ds_fx_pad_handle_ok(h))
 *         set_lightbar(ctx, h, (uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b);
 */
#ifndef DUALSENSE_FX_WIRELESS_H
#define DUALSENSE_FX_WIRELESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define DS_FX_UNUSED __attribute__((unused))
#else
#define DS_FX_UNUSED
#endif

#define DS_FX_PAD_TYPE_STANDARD 0
#define DS_FX_PAD_INDEX_PRIMARY 0

typedef int32_t (*ds_fx_pad_lookup_fn)(void *ctx, int32_t user,
                                       int32_t type, int32_t index);
typedef int32_t (*ds_fx_pad_lightbar_fn)(void *ctx, int32_t handle,
                                         uint8_t r, uint8_t g, uint8_t b);

static DS_FX_UNUSED int ds_fx_pad_handle_ok(int32_t handle)
{
    return handle >= 0;
}

static DS_FX_UNUSED int32_t ds_fx_pad_acquire(void *ctx, ds_fx_pad_lookup_fn get_handle,
                                 ds_fx_pad_lookup_fn open_pad, int32_t user,
                                 int32_t type, int32_t index)
{
    int32_t handle = get_handle ? get_handle(ctx, user, type, index) : -1;
    if (ds_fx_pad_handle_ok(handle))
        return handle;
    if (!open_pad)
        return handle;
    return open_pad(ctx, user, type, index);
}

static DS_FX_UNUSED unsigned ds_fx_pad_apply(void *ctx, ds_fx_pad_lookup_fn get_handle,
                                ds_fx_pad_lookup_fn open_pad,
                                ds_fx_pad_lightbar_fn set_lightbar,
                                ds_fx_pad_lightbar_fn reset_lightbar,
                                const int32_t *users, unsigned user_count,
                                int enabled, uint8_t r, uint8_t g, uint8_t b)
{
    unsigned u, found = 0;
    if (!users || !user_count)
        return 0;
    for (u = 0; u < user_count && found < 4; ++u) {
        int32_t handle = ds_fx_pad_acquire(ctx, get_handle, open_pad, users[u],
                                           DS_FX_PAD_TYPE_STANDARD,
                                           DS_FX_PAD_INDEX_PRIMARY);
        int32_t result;
        if (!ds_fx_pad_handle_ok(handle))
            continue;
        if (enabled)
            result = set_lightbar ? set_lightbar(ctx, handle, r, g, b) : -1;
        else
            result = reset_lightbar ? reset_lightbar(ctx, handle, 0, 0, 0) : -1;
        if (result == 0)
            ++found;
    }
    return found;
}

#ifdef __cplusplus
}
#endif
#endif

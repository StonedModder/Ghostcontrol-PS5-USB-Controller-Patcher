/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#define GC_DSFX_VIBRATION_MODE_HAPTICS 1
#define GC_DSFX_VIBRATION_MODE_RUMBLE 2

typedef struct {
    int enabled;
    unsigned r, g, b;
    int rumble_active;
    unsigned rumble_large, rumble_small;
    unsigned user_count;
    int32_t users[4];
    unsigned excluded_count;
    int32_t excluded[4];
} GcDualSenseFxRemoteControl;

typedef struct {
    int running;
    unsigned controllers;
    unsigned handles_seen;
    unsigned connected_handles;
    uint64_t scans;
    uint64_t writes;
    uint64_t errors;
    uint64_t resets;
    int last_error;
} GcDualSenseFxRemoteStatus;

int gc_dualsense_fx_remote_start(const GcDualSenseFxRemoteControl *control);
int gc_dualsense_fx_remote_update(const GcDualSenseFxRemoteControl *control);
int gc_dualsense_fx_remote_status(GcDualSenseFxRemoteStatus *status);
void gc_dualsense_fx_remote_stop(void);
int gc_dualsense_fx_is_sony_dualsense(uint16_t vendor, uint16_t product);
int gc_dualsense_fx_connected_pad_is_dualsense(int have_usb_identity,
                                               uint16_t vendor,
                                               uint16_t product);
int gc_dualsense_fx_handle_is_valid(int32_t handle);
int gc_dualsense_fx_remote_status_usable(const GcDualSenseFxRemoteStatus *status);

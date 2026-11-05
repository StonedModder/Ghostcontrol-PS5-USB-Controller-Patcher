#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gc_config.h"

typedef struct {
    int running;
    int enabled;
    int effect;
    unsigned period_ms;
    unsigned step_ms;
    unsigned r;
    unsigned g;
    unsigned b;
    unsigned current_r;
    unsigned current_g;
    unsigned current_b;
    unsigned controllers;
    uint64_t writes;
    uint64_t errors;
    uint64_t resets;
    int last_error;
    int rumble_active;
    unsigned rumble_large, rumble_small;
    unsigned rumble_on_ms, rumble_off_ms, rumble_repeats;
    int motion_valid;
    float quat[4], gravity[3], gyro[3];
} GcDualSenseFxStatus;

static inline unsigned gc_dualsense_fx_local_scan_delay(unsigned controllers,
                                                         unsigned step_ms)
{
    if (!controllers) return 1000;
    return step_ms < 16 ? 16 : step_ms;
}

int gc_dualsense_fx_start(GcConfigStore *config);
void gc_dualsense_fx_stop(void);
void gc_dualsense_fx_status(GcDualSenseFxStatus *status);
int gc_dualsense_fx_rumble(unsigned large, unsigned small,
                           unsigned on_ms, unsigned off_ms,
                           unsigned repeats);
void gc_dualsense_fx_rumble_stop(void);

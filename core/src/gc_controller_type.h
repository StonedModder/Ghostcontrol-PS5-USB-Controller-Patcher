/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string.h>

/* Persisted user intent, NOT Sony device-type numbers or report ABI values.
 * Keep DS5 at zero so old/zero-initialized rules retain their existing intent.
 * PSVR2 left/right are distinct identities, not aliases for a generic gamepad.
 * A backend must explicitly advertise and apply a type before it is usable. */
typedef enum {
    GC_CONTROLLER_OUTPUT_DS5 = 0,
    GC_CONTROLLER_OUTPUT_DS4,
    GC_CONTROLLER_OUTPUT_PSVR2_LEFT,
    GC_CONTROLLER_OUTPUT_PSVR2_RIGHT,
    GC_CONTROLLER_OUTPUT_COUNT
} GcControllerOutputType;

static inline const char *gc_controller_output_name(GcControllerOutputType type)
{
    switch (type) {
    case GC_CONTROLLER_OUTPUT_DS5: return "ds5";
    case GC_CONTROLLER_OUTPUT_DS4: return "ds4";
    case GC_CONTROLLER_OUTPUT_PSVR2_LEFT: return "psvr2_left";
    case GC_CONTROLLER_OUTPUT_PSVR2_RIGHT: return "psvr2_right";
    default: return NULL;
    }
}

static inline int gc_controller_output_parse(const char *text, GcControllerOutputType *out)
{
    if (!text || !out) return -1;
    for (int i = 0; i < GC_CONTROLLER_OUTPUT_COUNT; ++i) {
        GcControllerOutputType type = (GcControllerOutputType)i;
        if (!strcmp(text, gc_controller_output_name(type))) { *out = type; return 0; }
    }
    return -1;
}

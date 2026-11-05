#include "gc_dualsense_fx_scan.h"

#include <string.h>

int gc_dualsense_fx_motion_finite(float value)
{
    return value == value && value >= -20.0f && value <= 20.0f;
}

int gc_dualsense_fx_motion_shape(const float values[GC_DSFX_MOTION_FLOATS])
{
    unsigned i;
    float quat2, grav2;
    if (!values)
        return 0;
    for (i = 0; i < GC_DSFX_MOTION_FLOATS; ++i)
        if (!gc_dualsense_fx_motion_finite(values[i]))
            return 0;
    quat2 = values[0] * values[0] + values[1] * values[1] +
            values[2] * values[2] + values[3] * values[3];
    grav2 = values[4] * values[4] + values[5] * values[5] +
            values[6] * values[6];
    if (quat2 < 0.81f || quat2 > 1.21f)
        return 0;
    if (grav2 < 0.64f || grav2 > 1.5625f)
        return 0;
    if (values[3] < 0.3f && values[3] > -0.3f)
        return 0;
    if (grav2 < 1.0e-12f)
        return 0;
    return 1;
}

int gc_dualsense_fx_motion_live(const float first[GC_DSFX_MOTION_FLOATS],
                               const float second[GC_DSFX_MOTION_FLOATS])
{
    unsigned i;
    if (!first || !second)
        return 0;
    for (i = 0; i < GC_DSFX_MOTION_FLOATS; ++i) {
        float delta = first[i] - second[i];
        if (delta < -1.0e-7f || delta > 1.0e-7f)
            return 1;
    }
    return 0;
}

size_t gc_dualsense_fx_motion_candidates(const uint8_t *data, size_t length,
                                         size_t *offsets, size_t capacity)
{
    size_t found = 0, off;
    if (!data || !offsets || !capacity || length < GC_DSFX_MOTION_FLOATS * 4)
        return 0;
    for (off = 0; off + GC_DSFX_MOTION_FLOATS * 4 <= length; off += 4) {
        float values[GC_DSFX_MOTION_FLOATS];
        memcpy(values, data + off, sizeof(values));
        if (!gc_dualsense_fx_motion_shape(values))
            continue;
        offsets[found++] = off;
        if (found == capacity)
            break;
    }
    return found;
}

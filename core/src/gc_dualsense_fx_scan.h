#pragma once

#include <stddef.h>
#include <stdint.h>

#define GC_DSFX_MOTION_FLOATS 10

int gc_dualsense_fx_motion_finite(float value);
int gc_dualsense_fx_motion_shape(const float values[GC_DSFX_MOTION_FLOATS]);
int gc_dualsense_fx_motion_live(const float first[GC_DSFX_MOTION_FLOATS],
                               const float second[GC_DSFX_MOTION_FLOATS]);
size_t gc_dualsense_fx_motion_candidates(const uint8_t *data, size_t length,
                                         size_t *offsets, size_t capacity);

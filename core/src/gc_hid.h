/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_profile.h"

#define GC_HID_MAX_FIELDS 96
typedef struct {
    uint16_t offset, page, usage, array_base;
    uint8_t bits, report;
    int32_t minimum, maximum;
    uint16_t array_usage_min, array_usage_max;
    uint8_t is_array, null_state;
} GcHidField;

#define GC_HID_FEATURE_L2_ANALOG (1u << 0)
#define GC_HID_FEATURE_R2_ANALOG (1u << 1)
typedef struct {
    GcHidField fields[GC_HID_MAX_FIELDS];
    size_t count;
    /* Fields no standard usage claimed, kept in descriptor order. A
     * vendor-defined report (a Switch Pro Controller's 0x30, and most pads that
     * "need a driver") declares no standard usages, so the semantic pass below
     * collects nothing and the device looks dead. These are mapped positionally
     * instead, so the pad produces input the setup wizard can remap. */
    GcHidField raw_fields[GC_HID_MAX_FIELDS];
    size_t raw_count;
    int numbered;
    unsigned axes;
    unsigned features;
    /* Device-specific default buttons; original HID usages stay unchanged. */
    int ps3_gh_guitar;
} GcHidLayout;

typedef struct {
    GcHidSource source;
    uint16_t field_offset;
    int32_t value;
    int active, is_null;
} GcHidValue;

typedef struct {
    GcHidValue values[GC_HID_MAX_FIELDS];
    size_t count;
} GcHidState;

/* Accept gamepad/joystick application collections, never keyboard/mouse data. */
int gc_hid_parse_descriptor(const uint8_t *data, size_t size, GcHidLayout *layout);
void gc_hid_set_device(GcHidLayout *layout, uint16_t vid, uint16_t pid);
int gc_hid_input(const GcHidLayout *layout, const uint8_t *data, size_t size,
                 ScePadData *pad);
int gc_hid_input_profile(const GcHidLayout *layout, const uint8_t *data,
                         size_t size, GcHidState *state,
                         const GcProfile *profile, ScePadData *pad);
int gc_hid_values(const GcHidLayout *layout, const uint8_t *data, size_t size,
                  GcHidValue *values, size_t capacity, size_t *count);
int gc_hid_source_equal(const GcHidSource *left, const GcHidSource *right);
int gc_hid_state_update(GcHidState *state, const GcHidLayout *layout,
                         const uint8_t *data, size_t size);
void gc_hid_apply_profile_values(const GcHidLayout *layout,
                                 const GcHidValue *values, size_t count,
                                 const GcProfile *profile, ScePadData *pad);

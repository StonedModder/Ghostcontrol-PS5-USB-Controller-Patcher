/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gc_types.h"

#define GC_PROFILE_SCHEMA 3
#define GC_PROFILE_SCHEMA_INSTANCE 4
#define GC_PROFILE_SCHEMA_V2 2
#define GC_PROFILE_SCHEMA_LEGACY 1
#define GC_PROFILE_ID_MAX 64
#define GC_PROFILE_NAME_MAX 96
#define GC_PROFILE_PROTOCOL_MAX 32
#define GC_PROFILE_TEXT_MAX 16384

/* Bounds for a PROFILE_EDIT key/value, mirroring the 1.x client's field rules
 * so the service rejects a malformed edit before it touches a saved profile. */
#define GC_PROFILE_EDIT_ID_MAX 64
#define GC_PROFILE_EDIT_KEY_MAX 64
#define GC_PROFILE_EDIT_VALUE_MAX 512

typedef enum {
    GC_BUTTON_SHARE = 0,
    GC_BUTTON_L3,
    GC_BUTTON_R3,
    GC_BUTTON_OPTIONS,
    GC_BUTTON_UP,
    GC_BUTTON_RIGHT,
    GC_BUTTON_DOWN,
    GC_BUTTON_LEFT,
    GC_BUTTON_L2,
    GC_BUTTON_R2,
    GC_BUTTON_L1,
    GC_BUTTON_R1,
    GC_BUTTON_TRIANGLE,
    GC_BUTTON_CIRCLE,
    GC_BUTTON_CROSS,
    GC_BUTTON_SQUARE,
    GC_BUTTON_PS,
    GC_BUTTON_TOUCHPAD,
    GC_BUTTON_INPUT15,
    GC_BUTTON_INPUT16,
    GC_BUTTON_INPUT17,
    GC_BUTTON_INPUT18,
    GC_BUTTON_COUNT
} GcButton;

typedef enum {
    GC_AXIS_LX = 0,
    GC_AXIS_LY,
    GC_AXIS_RX,
    GC_AXIS_RY,
    GC_AXIS_L2,
    GC_AXIS_R2,
    GC_AXIS_COUNT
} GcAxis;

/* Descriptor coordinates are stable across reports and reconnects. */
typedef struct {
    uint16_t offset, page, usage;
    uint8_t bits, report;
    int32_t minimum, maximum;
} GcHidSource;

typedef struct {
    GcHidSource source;
    int32_t active_value;
    int present;
} GcHidButtonBinding;

typedef struct {
    GcHidSource source;
    int32_t center;
    int polarity;
    int present;
} GcHidAxisBinding;

typedef struct {
    GcAxis source;
    int32_t center, minimum, maximum;
    int polarity;
    int present;
} GcAxisBinding;

typedef struct {
    uint32_t source_mask;
    int present;
} GcButtonPatternBinding;

typedef struct {
    int schema;
    char id[GC_PROFILE_ID_MAX];
    char name[GC_PROFILE_NAME_MAX];
    int enabled;
    int match_any_vid;
    int match_any_pid;
    uint16_t match_vid;
    uint16_t match_pid;
    char protocol[GC_PROFILE_PROTOCOL_MAX];
    uint32_t button_targets[GC_BUTTON_COUNT];
    GcButtonPatternBinding button_patterns[GC_BUTTON_INPUT15];
    GcHidButtonBinding hid_buttons[GC_BUTTON_INPUT15];
    GcHidAxisBinding hid_axes[GC_AXIS_COUNT];
    GcAxisBinding axes[GC_AXIS_COUNT];
    float left_deadzone;
    float right_deadzone;
    float left_sensitivity;
    float right_sensitivity;
    int invert_left_x;
    int invert_left_y;
    int invert_right_x;
    int invert_right_y;
    uint8_t left_trigger_threshold;
    uint8_t right_trigger_threshold;
char owner_controller_id[GC_PROFILE_ID_MAX];
char owner_instance_id[24];
} GcProfile;

void gc_profile_defaults(GcProfile *profile);
int gc_profile_parse(const char *text, size_t length, GcProfile *profile,
                     char *error, size_t error_size);
int gc_profile_serialize(const GcProfile *profile, char *output,
                         size_t output_size);
int gc_profile_match_score(const GcProfile *profile, uint16_t vid,
                           uint16_t pid, const char *protocol);
/* Configuration/editing identity check. Unlike runtime matching, this
 * deliberately includes disabled profiles so guided setup can re-enable and
 * repair an existing exact controller profile instead of trapping its ID. */
int gc_profile_matches_exact_identity(const GcProfile *profile, uint16_t vid,
                                      uint16_t pid, const char *protocol);
void gc_profile_apply(const GcProfile *profile, ScePadData *pad);

/* Splits a "id|key|value" PROFILE_EDIT argument into its three parts and
 * validates them the way the 1.x client did: id is [A-Za-z0-9_.-] shorter
 * than GC_PROFILE_ID_MAX and not auto/./..; key is [A-Za-z0-9_.] at most
 * GC_PROFILE_EDIT_KEY_MAX and not id/schema; value is at most
 * GC_PROFILE_EDIT_VALUE_MAX with no CR, LF or NUL. Only the first two '|'
 * delimit, so a value may itself contain '|'. Returns 0 on success and writes
 * a human-readable reason through `error` on failure. */
int gc_profile_edit_split(const char *argument, char *id, size_t id_size,
                          char *key, size_t key_size, char *value,
                          size_t value_size, char *error, size_t error_size);

/* Applies one key/value to an in-memory profile through the same per-key
 * parser the INI loader uses. 'id' and 'schema' are refused: an edit must not
 * reparent a saved profile or roll its schema back. */
int gc_profile_apply_key(GcProfile *profile, const char *key,
                         const char *value, char *error, size_t error_size);

const char *gc_button_name(GcButton button);
uint32_t gc_button_mask(GcButton button);
int gc_button_from_name(const char *name, GcButton *button);
const char *gc_axis_name(GcAxis axis);
int gc_axis_from_name(const char *name, GcAxis *axis);

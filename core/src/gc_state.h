/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GC_STATE_MAX_SLOTS 4
#define GC_STATE_MAX_NOTIFICATIONS 16
/* Worst-case JSON escaping expands each stored byte to six characters. The
 * fixed snapshot schema remains below this proven bound. */
#define GC_STATE_JSON_MAX 32768

typedef enum {
    GC_SERVICE_INACTIVE = 0,
    GC_SERVICE_STARTING,
    GC_SERVICE_ACTIVE,
    GC_SERVICE_STOPPING,
    GC_SERVICE_ERROR
} GcServiceStatus;

typedef struct {
    int connected;
    int streaming;
    uint16_t vid;
    uint16_t pid;
    int32_t vda_handle;
    int32_t bound_user;
    uint32_t inject_count;
    char name[96];
    char protocol[32];
    char profile[64];
    char device_path[64];
} GcSlotState;

typedef struct {
    uint64_t timestamp_ms;
    char message[192];
} GcNotificationState;

typedef struct {
    char status[96];
    uint32_t connected, routes, imports;
    int32_t game_pid;
    uint64_t frames, publications, gateway_calls, missed_deadlines;
    uint64_t lease_expirations;
} GcWirelessState;

typedef struct {
    GcServiceStatus service;
    int32_t pid;
    uint64_t started_ms;
    uint64_t config_generation;
    char version[32];
    char error[192];
    GcWirelessState wireless;
    GcSlotState slots[GC_STATE_MAX_SLOTS];
    GcNotificationState notifications[GC_STATE_MAX_NOTIFICATIONS];
    size_t notification_count;
} GcStateSnapshot;

typedef struct GcState GcState;

/* Update selected parts of one slot while holding the state lock once.  The
 * LIVE group is the USB/VDA-owned identity and counters; presentation fields
 * are independent so concurrent publishers cannot overwrite one another with
 * stale snapshot data. */
enum GcStateSlotPatchFields {
    GC_STATE_SLOT_PATCH_LIVE       = 1u << 0,
    GC_STATE_SLOT_PATCH_NAME       = 1u << 1,
    GC_STATE_SLOT_PATCH_PROFILE    = 1u << 2,
    GC_STATE_SLOT_PATCH_STREAMING  = 1u << 3,
    GC_STATE_SLOT_PATCH_BOUND_USER = 1u << 4,
};

GcState *gc_state_open(const char *root, const char *version,
                       char *error, size_t error_size);
void gc_state_close(GcState *state);
void gc_state_set_service(GcState *state, GcServiceStatus status,
                          const char *error);
void gc_state_set_generation(GcState *state, uint64_t generation);
void gc_state_set_wireless(GcState *state, const GcWirelessState *wireless);
void gc_state_set_slot(GcState *state, size_t slot, const GcSlotState *value);
void gc_state_patch_slot(GcState *state, size_t slot,
                         const GcSlotState *value, unsigned fields);
void gc_state_clear_slot(GcState *state, size_t slot);
void gc_state_note(GcState *state, const char *message);
int gc_state_snapshot(GcState *state, GcStateSnapshot *snapshot);
int gc_state_json(GcState *state, char *output, size_t output_size);
int gc_state_flush(GcState *state);
const char *gc_service_status_name(GcServiceStatus status);

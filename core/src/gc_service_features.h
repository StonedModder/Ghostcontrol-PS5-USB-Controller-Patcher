/* SPDX-License-Identifier: GPL-3.0-or-later
 * Console-only command surface for GhosttoothAPI. Not linked into host tests.
 * No SystemService, no process attach, no update execution.
 */
#pragma once
#include "gc_types.h"
#include "gc_config.h"
#include <stddef.h>
#include <stdint.h>

struct json_object;
struct GcConfigStore;

/* 1 when the initial-user pad answered, and *data is that sample. */
int gc_service_pad_read(ScePadData *data);

void gc_service_features_init(struct GcConfigStore *config);
void gc_service_features_close(void);
/* Preflight observes actual owned-resource blockers; close checks every
 * release. The caller must serialize admission, tick and native output with
 * cleanup and retain config/original lease on any failure. */
const char *gc_service_features_release_blocker(void);
int gc_service_features_close_checked(void);
/* Partial failed cleanup: no resumed input/reinitialization until confirmed. */
int gc_service_features_retiring(void);
void gc_service_features_tick(uint64_t now_ms);
/* 0 handled, -1 handled as an error, 1 not this module's command. */
int gc_service_features_command(const char *cmd, const char *arg,
                                char *out, size_t cap);
void gc_service_features_slot(struct json_object *slots);
/* 1 when the last pad sample included motion. */
int gc_service_features_motion(float gyro[3], float accel[3]);

void gc_service_features_instance_policy_invalidated(
    GcInstanceInvalidationReason reason, uint64_t invalidation_generation);
/* Acquire-load current revocation epoch; informational until checked under feature. */
uint64_t gc_service_features_instance_revocation_epoch(void);
/* Caller holds feature; clears old draft on mismatch/pending, 0 usable, -1 revoked. */
int gc_service_features_instance_draft_check_held(uint64_t captured_epoch);

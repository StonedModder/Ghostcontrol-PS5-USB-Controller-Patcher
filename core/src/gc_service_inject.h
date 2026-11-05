/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_bt.h"
#include "gc_config.h"
#include "gc_controller_instance.h"
#include "gc_types.h"
#include <stdint.h>
#include <stddef.h>
#include "gc_controller_type.h"

struct GcConfigStore;
#define GC_SERVICE_SOURCE_PATH_MAX 64
#define GC_SERVICE_REPORT_MAX 256
/* Physical source inventory, not a claim that a virtual identity was applied. */
typedef struct {
    char path[GC_SERVICE_SOURCE_PATH_MAX];
    char protocol[32];
    char name[96];
    char transport[16];
    uint16_t vid, pid;
    /* Nonzero connection identity, unique within this process across restarts. */
    uint64_t connection_generation;
    int slot, connected, input_available, virtual_live;
    GcControllerOutputType output_type;
    char output_error[64];
    /* Resident USB read/open faults on this slot. Not a frozen IPC field
     * and not a new connection: transient faults leave connection_generation. */
    unsigned io_faults;
    GcControllerMatch binding;
} GcServiceSource;

/* Set before starting the BT worker; keep the store alive through close. */
void gc_service_inject_set_config(struct GcConfigStore *config);
size_t gc_service_inject_sources(GcServiceSource *out, size_t capacity);
/* Reads original decoded input plus bounded latest raw HID report for setup.
 * Never returns another source's sample or applies its saved remapping. */
int gc_service_inject_read_source(const char *path, ScePadData *out,
                                  uint8_t *raw, size_t raw_capacity,
                                  size_t *raw_length);
/* Atomic original-input read for this exact observed connection. Rejects
 * path/model/protocol/generation mismatches and clears outputs on failure. */
/* Byte window + bit mask carrying this slot's digital controls, derived from the
 * parsed descriptor (1-bit buttons and up-to-4-bit hats, wide fields such as a
 * per-frame counter excluded). Returns 0 and fills both when known, else -1. */
int gc_service_inject_source_digital_span(int slot, const char *path, unsigned *offset, uint32_t *mask);
int gc_service_inject_read_source_checked(const GcServiceSource *expected,
    ScePadData *out, uint8_t *raw, size_t raw_capacity, size_t *raw_length);
/* Acquire only inside a config writer transaction (or with no config in use).
 * Returns 0 with the source-transition fence held for this exact connection;
 * failure holds nothing. Release exactly once after commit/readback/rollback.
 * Never hold the snapshot mutex across filesystem I/O, join source workers or
 * invoke source/lifecycle callbacks while leased. Observed transitions may wait
 * for that I/O; this is not a physical-connectivity or cross-process guarantee. */
int gc_service_inject_source_acquire(const GcServiceSource *expected);
void gc_service_inject_source_release(void);

/* Initialize consumer state/fence before registration; 0 success, -1 unavailable. */
int gc_service_inject_instance_consumer_prepare(GcConfigStore *store);
/* Borrowed non-NULL context is exactly the prepared store pointer. */
void gc_service_inject_instance_policy_invalidated(void *context,
    GcInstanceInvalidationReason reason, uint64_t invalidation_generation);

int gc_service_inject_instance_acquire(const GcControllerMatch *expected);
void gc_service_inject_instance_release(void);

/* Lifecycle: initialize before starting the Bluetooth worker; stop/join that
 * worker before close. Only the resident loop calls tick/backend operations.
 * IPC readers use synchronized snapshots, never backend internals; snapshots
 * may lag a config commit until the resident loop evaluates it.
 * Backend add/update must be bounded: do not mutate config, acquire a nested
 * writer lease, join config/source mutators, or invoke init/close/tick. They
 * hold a policy lease and source-transition fence, not the snapshot mutex.
 * Same-thread ready/lost callbacks are supported; independent source changes
 * complete after the dispatched operation releases its fence. */
const GcBtOps *gc_service_inject_ops(void);
void gc_service_inject_set_bt(GcBt *bt);
void gc_service_inject_init(void);
void gc_service_inject_close(void);
/* Quiescent checked teardown. Preflight is observe-only; 0 from close proves
 * all owned USB/backend resources released. On failure retain owner state and
 * config, never infer release from an empty inventory or void callback. */
const char *gc_service_inject_release_blocker(void);
int gc_service_inject_close_checked(void);
void gc_service_inject_tick(long now);
int gc_service_inject_live(int slot);
int gc_service_inject_identity(int slot, uint16_t *vid, uint16_t *pid);
int gc_service_inject_copy(int slot, ScePadData *out);

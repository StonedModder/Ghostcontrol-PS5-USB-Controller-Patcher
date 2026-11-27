/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "gc_ds4_policy.h"
#include "gc_types.h"
#include "gc_remote.h"

#define GC_DS4_SOURCE_LIBPAD_SYMBOLS 7u

typedef struct {
    uint32_t firmware;
    uint32_t fingerprint_mask;
    uint64_t fingerprints[GC_DS4_SOURCE_LIBPAD_SYMBOLS];
    uint8_t controller_info_abi;
} GcDs4LibpadEvidence;

typedef struct {
    GcDs4Source source;
    ScePadData data;
    int32_t read_result;
} GcDs4SourceFrame;

typedef struct {
    uint32_t sequence;
    uint32_t source_count;
    GcDs4SourceFrame frames[GC_DS4_MAX_ROUTES];
} GcDs4SourceSnapshot;

typedef struct {
    pid_t pid;
    intptr_t process_identity;
    intptr_t mapping;
    size_t mapping_size;
    intptr_t args_address;
    uint64_t thread_id;
    int launch_attempted;
    int joined;
    int32_t pending_handle;
    int32_t owned_handles[GC_DS4_MAX_ROUTES];
    uint32_t owned_count;
    GcRemoteSession trace;
    uint32_t source_count;
    GcDs4Source sources[GC_DS4_MAX_ROUTES];
    GcDs4LibpadEvidence evidence;
} GcDs4SourceReader;

int gc_ds4_source_start(const int32_t *user_ids, size_t user_count,
                        GcDs4SourceReader *reader);
int gc_ds4_source_snapshot(const GcDs4SourceReader *reader,
                           GcDs4SourceSnapshot *snapshot);
int gc_ds4_source_set_vibration(const GcDs4SourceReader *reader,
                                unsigned source_slot, uint8_t large,
                                uint8_t small);
int gc_ds4_source_stop(GcDs4SourceReader *reader);
int gc_ds4_source_heartbeat(const GcDs4SourceReader *reader);
int gc_ds4_source_request_stop(const GcDs4SourceReader *reader);

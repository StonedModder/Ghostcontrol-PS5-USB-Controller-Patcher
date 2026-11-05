/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_ds4_game.h"
#include <time.h>

#define GC_DS4_GAME_MAGIC UINT32_C(0x34434744)
#define GC_DS4_GAME_LAYOUT UINT32_C(0x00030001)
#define GC_DS4_TARGET_PAGE_SIZE 0x4000u
#define GC_DS4_GAME_MAPPING_SIZE (GC_DS4_TARGET_PAGE_SIZE * 2u)

typedef struct {
    int32_t pad_handle;
    int32_t pad_index;
    int32_t user_id;
    uint32_t source_slot;
} GcDs4TargetRoute;

typedef struct {
    ScePadData pads[GC_DS4_MAX_ROUTES];
} GcDs4GameBank;

typedef struct {
    uint32_t magic, layout;
    volatile uint32_t active, sequence, routing_epoch;
    uint32_t route_count, pad_size, mapping_size;
    intptr_t mapping;
    intptr_t originals[GC_DS4_LIBPAD_SYMBOLS];
    intptr_t clock_gettime_fn;
    volatile uint64_t deadline_ns;
    GcDs4TargetRoute routes[GC_DS4_MAX_ROUTES];
    GcDs4GameBank banks[2];
    uint32_t hook_count, reserved;
    intptr_t hook_slots[GC_DS4_MAX_IMPORT_HOOKS];
    intptr_t hook_originals[GC_DS4_MAX_IMPORT_HOOKS];
    intptr_t hook_gateways[GC_DS4_MAX_IMPORT_HOOKS];
    uint32_t hook_protections[GC_DS4_MAX_IMPORT_HOOKS];
    uint32_t hook_kinds[GC_DS4_MAX_IMPORT_HOOKS];
    volatile uint64_t publications;
    volatile uint64_t calls[GC_DS4_LIBPAD_SYMBOLS];
    volatile uint64_t lease_expirations, snapshot_fallbacks;
} GcDs4GameArgs;

_Static_assert(sizeof(ScePadData) == 120, "ScePadData ABI changed");
_Static_assert(sizeof(GcDs4GameArgs) < GC_DS4_TARGET_PAGE_SIZE,
               "game arguments exceed RW page");
int32_t gc_ds4_game_read_state_stub(int32_t, void *, GcDs4GameArgs *);
int32_t gc_ds4_game_read_state_ext_stub(int32_t, void *, GcDs4GameArgs *);
int32_t gc_ds4_game_read_stub(int32_t, void *, int32_t, GcDs4GameArgs *);
int32_t gc_ds4_game_read_ext_stub(int32_t, void *, int32_t, GcDs4GameArgs *);
int32_t gc_ds4_game_data_internal_stub(int32_t, void *, GcDs4GameArgs *);
int32_t gc_ds4_game_controller_info_stub(int32_t, void *, GcDs4GameArgs *);
void gc_ds4_game_stub_end(void);

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GC_DS4_MAX_ROUTES 4u
#define GC_DS4_MAX_PAD_INDEX 7

typedef struct {
    int32_t user_id;
    int32_t pad_index;
    int32_t pad_handle;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t connected;
    uint8_t identity_verified;
} GcDs4Source;

typedef struct {
    int32_t user_id;
    int32_t pad_index;
    int32_t pad_handle;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t connected;
    uint8_t valid;
} GcDs4GamePad;

typedef struct {
    uint8_t source_slot;
    uint8_t game_slot;
    int32_t pad_handle;
} GcDs4Route;

typedef enum {
    GC_DS4_ROUTE_OK = 0,
    GC_DS4_ROUTE_SOURCE_INVALID,
    GC_DS4_ROUTE_SOURCE_DUPLICATE,
    GC_DS4_ROUTE_NO_EXACT_TARGET,
    GC_DS4_ROUTE_AMBIGUOUS_TARGET,
    GC_DS4_ROUTE_TARGET_ALREADY_USED
} GcDs4RouteResult;

int gc_ds4_is_known(uint8_t connected, uint16_t vendor_id,
                    uint16_t product_id);
size_t gc_ds4_select_routes(const GcDs4Source *sources, size_t source_count,
                            const GcDs4GamePad *pads, size_t pad_count,
                            GcDs4Route routes[GC_DS4_MAX_ROUTES],
                            GcDs4RouteResult results[GC_DS4_MAX_ROUTES]);

int gc_ds4_import_scan_complete(uint32_t required_mask,
                                uint32_t discovered_mask,
                                size_t hook_count, int scan_complete,
                                int overflowed);

typedef enum {
    GC_DS4_MAPPING_UNMAP = 0,
    GC_DS4_MAPPING_RETAIN_REUSABLE,
    GC_DS4_MAPPING_RETAIN_QUARANTINED
} GcDs4MappingDisposition;

GcDs4MappingDisposition gc_ds4_mapping_disposition(
    int hook_was_visible, int bridge_disabled, int every_hook_restored);

uint32_t gc_ds4_next_sequence(uint32_t current);


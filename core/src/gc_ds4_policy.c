/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_ds4_policy.h"

#include <limits.h>

int
gc_ds4_is_known(uint8_t connected, uint16_t vendor_id,
                uint16_t product_id)
{
    return connected && vendor_id == UINT16_C(0x054c) &&
        (product_id == UINT16_C(0x05c4) ||
         product_id == UINT16_C(0x09cc) ||
         product_id == UINT16_C(0x0ba0));
}

static int
source_valid(const GcDs4Source *source)
{
    return source && source->identity_verified && source->connected &&
        source->user_id >= 0 && source->pad_index >= 0 &&
        source->pad_index <= GC_DS4_MAX_PAD_INDEX &&
        source->pad_handle > 0;
}

static int
source_duplicate(const GcDs4Source *sources, size_t slot)
{
    for (size_t other = 0; other < slot; ++other) {
        if (sources[other].pad_handle == sources[slot].pad_handle ||
            (sources[other].user_id == sources[slot].user_id &&
             sources[other].pad_index == sources[slot].pad_index))
            return 1;
    }
    return 0;
}

size_t
gc_ds4_select_routes(const GcDs4Source *sources, size_t source_count,
                     const GcDs4GamePad *pads, size_t pad_count,
                     GcDs4Route routes[GC_DS4_MAX_ROUTES],
                     GcDs4RouteResult results[GC_DS4_MAX_ROUTES])
{
    if (!sources || !pads || !routes || !results)
        return 0;
    if (source_count > GC_DS4_MAX_ROUTES)
        source_count = GC_DS4_MAX_ROUTES;

    size_t route_count = 0;
    for (size_t source_slot = 0; source_slot < source_count; ++source_slot) {
        const GcDs4Source *source = &sources[source_slot];
        size_t candidate = SIZE_MAX;
        size_t candidate_count = 0;
        results[source_slot] = GC_DS4_ROUTE_NO_EXACT_TARGET;

        if (!source_valid(source)) {
            results[source_slot] = GC_DS4_ROUTE_SOURCE_INVALID;
            continue;
        }
        if (source_duplicate(sources, source_slot)) {
            results[source_slot] = GC_DS4_ROUTE_SOURCE_DUPLICATE;
            continue;
        }

        for (size_t game_slot = 0; game_slot < pad_count; ++game_slot) {
            const GcDs4GamePad *pad = &pads[game_slot];
            int eligible = gc_ds4_is_known(
                pad->connected, pad->vendor_id, pad->product_id) ||
                (!pad->connected && !pad->valid);
            if (pad->pad_handle <= 0 || !eligible ||
                pad->user_id != source->user_id ||
                pad->pad_index != source->pad_index)
                continue;
            candidate = game_slot;
            ++candidate_count;
        }

        if (candidate_count == 0)
            continue;
        if (candidate_count != 1) {
            results[source_slot] = GC_DS4_ROUTE_AMBIGUOUS_TARGET;
            continue;
        }

        int already_used = 0;
        for (size_t route_slot = 0; route_slot < route_count; ++route_slot) {
            if (routes[route_slot].pad_handle == pads[candidate].pad_handle) {
                already_used = 1;
                break;
            }
        }
        if (already_used) {
            results[source_slot] = GC_DS4_ROUTE_TARGET_ALREADY_USED;
            continue;
        }

        routes[route_count].source_slot = (uint8_t)source_slot;
        routes[route_count].game_slot = (uint8_t)candidate;
        routes[route_count].pad_handle = pads[candidate].pad_handle;
        results[source_slot] = GC_DS4_ROUTE_OK;
        ++route_count;
    }
    return route_count;
}

int
gc_ds4_import_scan_complete(uint32_t required_mask,
                            uint32_t discovered_mask,
                            size_t hook_count, int scan_complete,
                            int overflowed)
{
    return required_mask != 0 && hook_count != 0 && scan_complete &&
        !overflowed && (discovered_mask & required_mask) == required_mask;
}

GcDs4MappingDisposition
gc_ds4_mapping_disposition(int hook_was_visible, int bridge_disabled,
                           int every_hook_restored)
{
    if (!hook_was_visible)
        return GC_DS4_MAPPING_UNMAP;
    if (bridge_disabled && every_hook_restored)
        return GC_DS4_MAPPING_RETAIN_REUSABLE;
    return GC_DS4_MAPPING_RETAIN_QUARANTINED;
}

uint32_t
gc_ds4_next_sequence(uint32_t current)
{
    /* Keep alternating banks even when the nonzero sequence wraps. */
    return current == UINT32_MAX ? 2u : current + 1u;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure form of the identification the console runs at
 * core/src/bt/ps5_vpad.c:470-505. See gc_vpad_identify.h for scope, the
 * "touch nothing" rule and the window citations.
 */
#include "gc_vpad_identify.h"

#define CANDIDATES_MAX 2u   /* beyond one, the exact count is not actionable */

/* A timestamp that is numerically smaller than the window start is either a
 * counter wrap or a clock that stepped backwards; the two are indistinguishable
 * at this level. The console form subtracts into a signed long
 * (core/src/bt/ps5_vpad.c:476-477), which turns a decreasing stamp into a huge
 * negative difference: it never identifies, it just stays pending until the
 * clock catches up. This module refuses instead - UNKNOWN, touch nothing - so a
 * single bad timestamp cannot park an identification forever and cannot
 * identify anything either. Erring towards "touch nothing" is the same rule
 * :481-485 applies to zero and several candidates. */
static GcVpadIdentifyResult clock_went_back(GcVpadIdentify *state)
{
    state->result = GC_VPAD_IDENTIFY_UNKNOWN;
    state->active = 0;
    return state->result;
}

int gc_vpad_identify_configure(GcVpadIdentify *state, uint64_t identify_ms,
                               uint64_t ambiguity_ms)
{
    if (!state || identify_ms == 0 || ambiguity_ms == 0)
        return -1;
    if (state->active)
        return -1;
    state->identify_ms = identify_ms;
    state->ambiguity_ms = ambiguity_ms;
    return 0;
}

void gc_vpad_identify_begin(GcVpadIdentify *state, uint64_t now_ms, int slot)
{
    if (!state || state->active)
        return;
    if (state->identify_ms == 0)
        state->identify_ms = GC_VPAD_IDENTIFY_WINDOW_MS;
    if (state->ambiguity_ms == 0)
        state->ambiguity_ms = GC_VPAD_IDENTIFY_AMBIGUITY_MS;
    state->result = GC_VPAD_IDENTIFY_PENDING;
    state->slot = slot;
    state->active = 1;
    state->candidates = 0;
    state->device_id = 0;
    state->start_ms = now_ms;
    state->first_ms = 0;
}

GcVpadIdentifyResult gc_vpad_identify_on_device(GcVpadIdentify *state,
                                                uint64_t now_ms,
                                                uint64_t device_id)
{
    if (!state)
        return GC_VPAD_IDENTIFY_IDLE;
    /* After a result the console form has already dropped its pending slot and
     * closed the log source (:479-480), so a later line belongs to nobody. */
    if (!state->active)
        return state->result;
    if (now_ms < state->start_ms)
        return clock_went_back(state);
    if (device_id == 0)
        return state->result;               /* not a device id (gc_vda_klog.c:19-20) */
    if (state->candidates > 0 && state->device_id == device_id)
        return state->result;               /* the same device again */
    if (state->candidates == 0) {
        state->device_id = device_id;
        state->candidates = 1;
        state->first_ms = now_ms;
        return state->result;               /* PENDING: the ambiguity window decides */
    }
    state->candidates = CANDIDATES_MAX;
    state->result = GC_VPAD_IDENTIFY_AMBIGUOUS;
    state->active = 0;
    return state->result;
}

GcVpadIdentifyResult gc_vpad_identify_poll(GcVpadIdentify *state,
                                           uint64_t now_ms)
{
    uint64_t elapsed;

    if (!state)
        return GC_VPAD_IDENTIFY_IDLE;
    if (!state->active)
        return state->result;
    if (now_ms < state->start_ms)
        return clock_went_back(state);

    elapsed = now_ms - state->start_ms;
    if (state->candidates == 0) {
        /* :476 - no line yet and the window still open. */
        if (elapsed < state->identify_ms)
            return GC_VPAD_IDENTIFY_PENDING;
        state->result = GC_VPAD_IDENTIFY_UNKNOWN;
        state->active = 0;
        return state->result;
    }
    if (state->candidates == 1) {
        /* :477 - one match, but keep watching for a second one until the
         * ambiguity window or the identify window runs out. */
        if (elapsed >= state->identify_ms ||
            now_ms - state->first_ms >= state->ambiguity_ms) {
            state->result = GC_VPAD_IDENTIFY_IDENTIFIED;
            state->active = 0;
            return state->result;
        }
        return GC_VPAD_IDENTIFY_PENDING;
    }
    state->result = GC_VPAD_IDENTIFY_AMBIGUOUS;
    state->active = 0;
    return state->result;
}

int gc_vpad_identify_may_touch(GcVpadIdentifyResult result)
{
    return result == GC_VPAD_IDENTIFY_IDENTIFIED;
}

void gc_vpad_identify_cancel(GcVpadIdentify *state)
{
    if (!state)
        return;
    state->result = GC_VPAD_IDENTIFY_IDLE;
    state->active = 0;
    state->candidates = 0;
    state->device_id = 0;
    state->first_ms = 0;
}

GcVpadIdentifyResult gc_vpad_identify_result(const GcVpadIdentify *state)
{
    return state ? state->result : GC_VPAD_IDENTIFY_IDLE;
}

uint64_t gc_vpad_identify_device_id(const GcVpadIdentify *state)
{
    if (!state || state->result != GC_VPAD_IDENTIFY_IDENTIFIED)
        return 0;
    return state->device_id;
}

unsigned gc_vpad_identify_candidates(const GcVpadIdentify *state)
{
    return state ? state->candidates : 0u;
}

int gc_vpad_identify_slot(const GcVpadIdentify *state)
{
    return state ? state->slot : -1;
}

const char *gc_vpad_identify_result_name(GcVpadIdentifyResult result)
{
    switch (result) {
    case GC_VPAD_IDENTIFY_IDLE: return "idle";
    case GC_VPAD_IDENTIFY_PENDING: return "pending";
    case GC_VPAD_IDENTIFY_IDENTIFIED: return "identified";
    case GC_VPAD_IDENTIFY_AMBIGUOUS: return "ambiguous";
    case GC_VPAD_IDENTIFY_UNKNOWN: return "unknown";
    }
    return "idle";
}

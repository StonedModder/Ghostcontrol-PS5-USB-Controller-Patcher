/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One-at-a-time virtual-pad identification, as a pure state machine.
 *
 * Console form: core/src/bt/ps5_vpad.c:431-468 (start_identification) and
 * :470-505 (finish_identification / vpad_poll), fed by the single pending slot
 * at :116-121. That form owns a klog descriptor, a credential raise and an MBus
 * bind; none of that is here. The caller opens the virtual device, feeds the
 * device ids it parsed (core/src/gc_vda_klog.c:31-48) with its own timestamps,
 * and applies the result.
 *
 * The rule is the console rule: exactly one candidate inside the window
 * identifies the device; zero candidates when the window closes (UNKNOWN) and
 * two or more distinct ids (AMBIGUOUS) both mean "touch nothing" - do not bind,
 * feed or delete anything (core/src/bt/ps5_vpad.c:481-485).
 *
 * Windows are the caller's: GC_VPAD_IDENTIFY_WINDOW_MS and
 * GC_VPAD_IDENTIFY_AMBIGUITY_MS only record where the console values came from
 * (core/src/bt/ps5_vpad.c:98-99, 3000 ms from AddDevice to its DEVICE_ADDED
 * line and 300 ms watching for a second line after the first match) so a caller
 * can pass the same ones. gc_vpad_identify_configure() overrides them and no
 * other constant is used anywhere in this module.
 *
 * Does not: parse klog lines, read a clock, allocate, lock, or decide what to do
 * with an identified device. There is no static state.
 */
#pragma once

#include <stdint.h>

typedef enum {
    GC_VPAD_IDENTIFY_IDLE = 0,   /* nothing in flight, or cancelled: touch nothing */
    GC_VPAD_IDENTIFY_PENDING,    /* in flight, at least one window still open */
    GC_VPAD_IDENTIFY_IDENTIFIED, /* exactly one candidate: the only touchable outcome */
    GC_VPAD_IDENTIFY_AMBIGUOUS,  /* more than one distinct id: touch nothing */
    GC_VPAD_IDENTIFY_UNKNOWN     /* window closed with none: touch nothing */
} GcVpadIdentifyResult;

/* Caller-owned. Zero-initialise before the first call; begin() then applies the
 * cited window defaults if the caller has not configured its own. */
typedef struct {
    GcVpadIdentifyResult result;
    int slot;
    int active;
    unsigned candidates;   /* capped at 2: beyond one the count is not actionable */
    uint64_t device_id;    /* the first (only) candidate seen */
    uint64_t start_ms;
    uint64_t first_ms;
    uint64_t identify_ms;
    uint64_t ambiguity_ms;
} GcVpadIdentify;

/* Ghostpad-main values mirrored from core/src/bt/ps5_vpad.c:98-99. They are not
 * applied to a machine that has been configured, and nothing in this module
 * derives them. */
#define GC_VPAD_IDENTIFY_WINDOW_MS     3000u
#define GC_VPAD_IDENTIFY_AMBIGUITY_MS   300u

/* Set the two windows in milliseconds. Returns 0, or -1 if a pointer is NULL, a
 * window is 0 (a zero window would look like a measurement that never was), or
 * an identification is in flight (the windows of the window in flight are fixed). */
int gc_vpad_identify_configure(GcVpadIdentify *state, uint64_t identify_ms,
                               uint64_t ambiguity_ms);

/* Open the window for one slot. A no-op while an identification is already in
 * flight: one at a time is the point, because a DEVICE_ADDED line may then only
 * belong to the device just asked for (core/src/bt/ps5_vpad.c:116-121). */
void gc_vpad_identify_begin(GcVpadIdentify *state, uint64_t now_ms, int slot);

/* Feed one device id. Id 0 is not a device id and is ignored; a repeat of the
 * same id is the same device and is not a second candidate. */
GcVpadIdentifyResult gc_vpad_identify_on_device(GcVpadIdentify *state,
                                                uint64_t now_ms,
                                                uint64_t device_id);

/* Advance the windows. Before begin(), after a result, and after cancel() this
 * returns whatever the machine already holds (IDLE at the start). */
GcVpadIdentifyResult gc_vpad_identify_poll(GcVpadIdentify *state,
                                           uint64_t now_ms);

/* 1 only for IDENTIFIED. AMBIGUOUS, UNKNOWN and IDLE all answer 0, which is the
 * "touch nothing" rule made executable (core/src/bt/ps5_vpad.c:481-485). */
int gc_vpad_identify_may_touch(GcVpadIdentifyResult result);

/* Abandon the identification and go back to IDLE. Keeps the configured windows. */
void gc_vpad_identify_cancel(GcVpadIdentify *state);

/* Read-only views. The device id is 0 unless the result is IDENTIFIED. */
GcVpadIdentifyResult gc_vpad_identify_result(const GcVpadIdentify *state);
uint64_t gc_vpad_identify_device_id(const GcVpadIdentify *state);
unsigned gc_vpad_identify_candidates(const GcVpadIdentify *state);
int gc_vpad_identify_slot(const GcVpadIdentify *state);
const char *gc_vpad_identify_result_name(GcVpadIdentifyResult result);

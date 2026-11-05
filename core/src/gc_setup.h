/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_hid.h"
#define GC_SETUP_AXIS_BASE GC_BUTTON_COUNT
#define GC_SETUP_TARGET_COUNT (GC_SETUP_AXIS_BASE + GC_AXIS_COUNT)
#define GC_SETUP_MANUAL_TIMEOUT_MS 30000u
#define GC_SETUP_GUIDED_TIMEOUT_MS 120000u
#define GC_SETUP_REPEAT_CONFIRM_GAP_MS 250u
#define GC_SETUP_GUIDED_CONFIRMATIONS 3
/* Some event-driven XUSB pads omit the neutral packet on button release.
 * A repeated identical report is accepted only after a quiet gap, so held
 * reports cannot confirm themselves. */
#define GC_SETUP_GUIDED_CONFIRM_GAP_MS 250u
typedef struct {
    GcProfile profile;
    char path[32], message[192];
    int active, target, neutral, guided, observed, completed;
    int guided_ready_state; /* 1=waiting wake press, 2=waiting wake release */
    int guided_wake_cycles;
    uint32_t observed_buttons;
    uint32_t consumed_bits;
    uint32_t learned;
    uint32_t pending_pattern;
    int pending_pattern_target;
    uint64_t pending_pattern_ms;
    uint64_t pending_pattern_seen_ms;
    int pending_pattern_released;
    int guided_confirmations;
    /* -1=no repair; GC_SETUP_TARGET_COUNT=finish after repair. */
    int guided_resume_target;
    uint64_t deadline_ms, revision;
    ScePadData input;
    ScePadData axis_reference;
    GcHidValue hid_values[GC_HID_MAX_FIELDS];
    GcHidValue hid_reference[GC_HID_MAX_FIELDS];
    GcHidLayout hid_layout;
    size_t hid_count, hid_reference_count;
    uint32_t learned_axes;
    int hid, axis_reference_valid, axis_centered;
    uint64_t input_ms;
    uint8_t raw_report[32];
    /* First report seen after arming: wake/release are measured as a real
     * change from this baseline, so a descriptor that decodes to nothing can
     * still be woken and released. The dead-band keeps keep-alive stick jitter
     * from reading as a held button. */
    uint8_t wake_baseline[32];
    uint16_t wake_baseline_len;
    int wake_baseline_valid;
    /* Set once a report during the wake handshake shows a decoded control:
     * the parsed pad then owns press AND release, and the raw delta guess is
     * only for descriptors that decode to nothing. */
    int wake_pad_decoded;
    uint16_t raw_len;
    /* Digital byte window and bit mask for this source, derived from its
     * parsed descriptor by the service. Zero means unknown: fall back to the
     * generic mask heuristic. */
    unsigned raw_span_offset;
    uint32_t raw_span_mask;
    /* Noise-learned window for long numbered reports: bytes that change while
     * the pad is untouched (a per-frame counter, a timestamp) are noise and are
     * excluded, so presses in the remaining bytes are the only signal. */
    uint8_t raw_prev[8];
    int raw_prev_valid;
    unsigned raw_samples;
    unsigned raw_noise;
    uint8_t raw_change_count[8];
    int raw_window_ready;
    int raw_valid;
    int raw_learn;
} GcSetup;
/* Caller serializes access; input is the parsed state BEFORE remapping. */
void gc_setup_begin(GcSetup *setup, const GcProfile *profile, const char *path);
/* True when another Begin refers to the current physical/profile draft. */
int gc_setup_same_draft(const GcSetup *setup, const GcProfile *profile,
                        const char *path);
/* Re-arm the same draft without losing learned mappings; reset a new one. */
int gc_setup_rebegin(GcSetup *setup, const GcProfile *profile,
                     const char *path);
/* Seed the most recently observed parsed state without treating it as a press. */
void gc_setup_seed_buttons(GcSetup *setup, uint32_t buttons);
int gc_setup_seed_hid(GcSetup *setup, const GcHidLayout *layout,
                      const uint8_t *report, size_t report_size);
uint32_t gc_setup_learning_buttons(const ScePadData *input);
int gc_setup_arm(GcSetup *setup, int target, int guided, uint64_t now_ms);
void gc_setup_tick(GcSetup *setup, uint64_t now_ms);
int gc_setup_feed(GcSetup *setup, const char *path, uint32_t buttons, uint64_t now_ms);
int gc_setup_feed_input(GcSetup *setup, const char *path,
                        const ScePadData *input, uint64_t now_ms);
int gc_setup_feed_hid(GcSetup *setup, const char *path,
                      const GcHidLayout *layout, const uint8_t *report,
                      size_t report_size, const ScePadData *input,
                      uint64_t now_ms);
void gc_setup_set_raw_span(GcSetup *s, unsigned offset, uint32_t mask);
int gc_setup_feed_raw(GcSetup *setup, const char *path,
                      const uint8_t *report, size_t report_size,
                      const ScePadData *input, uint64_t now_ms);
const char *gc_setup_target_name(int target);
int gc_setup_target_from_name(const char *name, int *target);
void gc_setup_progress(const GcSetup *setup, int *step, int *total,
                       const char **stage);
void gc_setup_skip(GcSetup *setup, uint64_t now_ms);
void gc_setup_cancel(GcSetup *setup, const char *message);

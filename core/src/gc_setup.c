/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_setup.h"
#include "gc_rawmap.h"
#include <stdio.h>
#include <string.h>

static const int order[] = {
    GC_BUTTON_CROSS, GC_BUTTON_CIRCLE, GC_BUTTON_SQUARE,
    GC_BUTTON_TRIANGLE, GC_BUTTON_UP, GC_BUTTON_DOWN, GC_BUTTON_LEFT,
    GC_BUTTON_RIGHT, GC_BUTTON_L1, GC_BUTTON_R1, GC_BUTTON_L2,
    GC_BUTTON_R2, GC_BUTTON_L3, GC_BUTTON_R3, GC_BUTTON_OPTIONS,
    GC_BUTTON_SHARE, GC_BUTTON_PS, GC_BUTTON_TOUCHPAD,
    GC_SETUP_AXIS_BASE + GC_AXIS_LX, GC_SETUP_AXIS_BASE + GC_AXIS_LY,
    GC_SETUP_AXIS_BASE + GC_AXIS_RX, GC_SETUP_AXIS_BASE + GC_AXIS_RY,
    GC_SETUP_AXIS_BASE + GC_AXIS_L2, GC_SETUP_AXIS_BASE + GC_AXIS_R2,
};

static int hid_button_value(const GcHidValue *value);
static int feed_buttons(GcSetup *s, uint32_t buttons, uint64_t now_ms);

static int target_is_axis(int target)
{
    return target >= GC_SETUP_AXIS_BASE && target < GC_SETUP_TARGET_COUNT;
}

const char *gc_setup_target_name(int target)
{
    static const char *const axes[] = {
        "AXIS_LX", "AXIS_LY", "AXIS_RX", "AXIS_RY", "AXIS_L2", "AXIS_R2",
    };
    return target_is_axis(target) ? axes[target - GC_SETUP_AXIS_BASE] :
        target >= 0 && target < GC_BUTTON_INPUT15
            ? gc_button_name((GcButton)target) : "";
}

int gc_setup_target_from_name(const char *name, int *target)
{
    if (!name || !target) return -1;
    if (!strncmp(name, "AXIS_", 5)) {
        GcAxis axis;
        if (gc_axis_from_name(name + 5, &axis) == 0) {
            *target = GC_SETUP_AXIS_BASE + axis;
            return 0;
        }
        return -1;
    }
    GcButton button;
    if (gc_button_from_name(name, &button) || button >= GC_BUTTON_INPUT15)
        return -1;
    *target = button;
    return 0;
}

void gc_setup_progress(const GcSetup *s, int *step, int *total,
                       const char **stage)
{
    if (stage) *stage = !s || s->target < 0 ?
        (s && s->completed ? "done" : "review") :
        target_is_axis(s->target) ? "axes" : "buttons";
    if (total) *total = (int)(sizeof(order) / sizeof(order[0]));
    if (step) {
        *step = s && s->completed ?
            (int)(sizeof(order) / sizeof(order[0])) : 0;
        if (s)
            for (size_t index = 0; index < sizeof(order) / sizeof(order[0]);
                 ++index)
                if (order[index] == s->target) *step = (int)index + 1;
    }
}

void gc_setup_begin(GcSetup *s, const GcProfile *profile, const char *path)
{
    uint64_t revision = s->revision + 1;
    memset(s, 0, sizeof(*s));
    s->revision = revision;
    s->profile = *profile;
    s->profile.schema = GC_PROFILE_SCHEMA;
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->active = 1;
    s->target = -1;
    s->pending_pattern_target = -1;
    s->guided_resume_target = -1;
    s->pending_pattern_ms = 0;
    snprintf(s->message, sizeof(s->message),
             "Ready. Start guided setup or choose one control to learn.");
}

int gc_setup_same_draft(const GcSetup *s, const GcProfile *profile,
                        const char *path)
{
    return s && profile && path && s->active && !strcmp(s->path, path) &&
        !strcmp(s->profile.id, profile->id) &&
        s->profile.match_vid == profile->match_vid &&
        s->profile.match_pid == profile->match_pid &&
        !strcmp(s->profile.protocol, profile->protocol);
}

int gc_setup_rebegin(GcSetup *s, const GcProfile *profile, const char *path)
{
    if (!gc_setup_same_draft(s, profile, path)) {
        gc_setup_begin(s, profile, path);
        return 0;
    }
    s->target = -1;
    s->guided = s->neutral = s->observed = s->completed = 0;
    s->guided_ready_state = 0;
    s->guided_wake_cycles = 0;
    s->observed_buttons = 0;
    s->consumed_bits = 0;
    s->pending_pattern = 0;
    s->pending_pattern_target = -1;
    s->pending_pattern_seen_ms = 0;
    s->pending_pattern_released = 0;
    s->guided_confirmations = 0;
    s->guided_resume_target = -1;
    s->deadline_ms = s->input_ms = 0;
    s->raw_valid = s->raw_learn = 0;
    s->wake_baseline_valid = 0;
    s->wake_baseline_len = 0;
    s->wake_pad_decoded = 0;
    s->raw_len = 0;
    s->hid = s->axis_reference_valid = s->axis_centered = 0;
    s->hid_count = s->hid_reference_count = 0;
    memset(&s->input, 0, sizeof(s->input));
    ++s->revision;
    snprintf(s->message, sizeof(s->message),
             "Ready. Start guided setup or choose one control to learn.");
    return 1;
}

void gc_setup_seed_buttons(GcSetup *s, uint32_t buttons)
{
    s->observed = 1;
    s->observed_buttons = buttons;
    s->neutral = !buttons;
}

static int same_field(const GcHidValue *left, const GcHidValue *right)
{
    return left->field_offset == right->field_offset &&
        left->source.report == right->source.report &&
        left->source.bits == right->source.bits &&
        left->source.page == right->source.page &&
        left->source.minimum == right->source.minimum &&
        left->source.maximum == right->source.maximum;
}

static void merge_hid_values(GcSetup *s, const GcHidValue *values,
                             size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        size_t stored = 0;
        while (stored < s->hid_count &&
               !same_field(&s->hid_values[stored], &values[index]))
            ++stored;
        if (stored == s->hid_count) {
            if (s->hid_count == GC_HID_MAX_FIELDS) continue;
            ++s->hid_count;
        }
        s->hid_values[stored] = values[index];
    }
}

int gc_setup_seed_hid(GcSetup *s, const GcHidLayout *layout,
                      const uint8_t *report, size_t report_size)
{
    GcHidValue values[GC_HID_MAX_FIELDS];
    size_t count = 0;
    if (!s || !gc_hid_values(layout, report, report_size, values,
                             GC_HID_MAX_FIELDS, &count))
        return 0;
    merge_hid_values(s, values, count);
    s->hid_layout = *layout;
    s->hid = 1;
    for (size_t index = 0; index < s->hid_count; ++index)
        if (hid_button_value(&s->hid_values[index]) &&
            s->hid_values[index].active)
            s->neutral = 0;
    memcpy(s->hid_reference, s->hid_values,
           s->hid_count * sizeof(s->hid_values[0]));
    s->hid_reference_count = s->hid_count;
    return 1;
}

uint32_t gc_setup_learning_buttons(const ScePadData *input)
{
    if (!input) return 0;
    uint32_t buttons = input->buttons &
        ~(SCE_PAD_BUTTON_L2 | SCE_PAD_BUTTON_R2);
    if (input->analogButtons.l2 > 192) buttons |= SCE_PAD_BUTTON_L2;
    if (input->analogButtons.r2 > 192) buttons |= SCE_PAD_BUTTON_R2;
    return buttons;
}

static void set_prompt(GcSetup *s)
{
    const unsigned seconds = (s->guided ? GC_SETUP_GUIDED_TIMEOUT_MS
                                         : GC_SETUP_MANUAL_TIMEOUT_MS) / 1000u;
    if (!target_is_axis(s->target)) {
        if (s->guided)
            snprintf(s->message, sizeof(s->message),
                     "Press and release %s 3 times. First press counts. %u sec.",
                     gc_setup_target_name(s->target), seconds);
        else
            snprintf(s->message, sizeof(s->message),
                     "Release all buttons, then press the button for %s. %u seconds.",
                     gc_setup_target_name(s->target), seconds);
        return;
    }
    const GcAxis axis = (GcAxis)(s->target - GC_SETUP_AXIS_BASE);
    const char *motion = axis == GC_AXIS_LX || axis == GC_AXIS_RX ?
        "move it fully RIGHT" : axis == GC_AXIS_LY || axis == GC_AXIS_RY ?
        "move it fully DOWN" : "press it fully";
    snprintf(s->message, sizeof(s->message),
             "Center and release all controls, then %s for %s. %u seconds.",
             motion, gc_setup_target_name(s->target), seconds);
}

static int input_centered(const ScePadData *input)
{
    if (!input ||
        (input->buttons & ~(SCE_PAD_BUTTON_L2 | SCE_PAD_BUTTON_R2)) ||
        input->analogButtons.l2 > 16 || input->analogButtons.r2 > 16)
        return 0;
    const uint8_t axes[] = {input->leftStick.x, input->leftStick.y,
                            input->rightStick.x, input->rightStick.y};
    for (size_t index = 0; index < sizeof(axes); ++index)
        if (axes[index] < 112 || axes[index] > 144)
            return 0;
    return 1;
}

static void set_axis_reference(GcSetup *s, const ScePadData *input)
{
    s->axis_reference = *input;
    memcpy(s->hid_reference, s->hid_values,
           s->hid_count * sizeof(s->hid_values[0]));
    s->hid_reference_count = s->hid_count;
    s->axis_reference_valid = 1;
    s->axis_centered = 1;
}

int gc_setup_arm(GcSetup *s, int target, int guided, uint64_t now_ms)
{
    if (!s->active ||
        !((target >= 0 && target < GC_BUTTON_INPUT15) ||
          target_is_axis(target)))
        return -1;
    const int starting_guide = guided && target == GC_BUTTON_CROSS && s->target < 0;
    if (starting_guide || !guided)
        s->guided_resume_target = -1;
    if (starting_guide) {
        /* A guided walk builds ONE fresh, complete mapping set. Seeding it from
         * whatever an earlier (possibly failed) session learned made every new
         * capture collide with an inherited mapping: the walk repaired in a
         * cycle -- R2 -> L3 -> R3 -> OPTIONS -> R2 -- and never converged.
         * Clearing here keeps the per-control "Learn one X" repairs intact,
         * which arm with guided == 0 and so never take this path. */
        memset(s->profile.button_patterns, 0, sizeof s->profile.button_patterns);
        memset(s->profile.button_targets, 0, sizeof s->profile.button_targets);
        memset(s->profile.hid_buttons, 0, sizeof s->profile.hid_buttons);
        s->learned = 0;
        s->consumed_bits = 0;
        s->raw_prev_valid = 0;
        s->raw_samples = 0;
        s->raw_noise = 0;
        memset(s->raw_change_count, 0, sizeof s->raw_change_count);
        s->raw_window_ready = 0;
    }
    s->target = target;
    s->guided = guided;
    s->guided_ready_state = starting_guide ? 1 : 0;
    s->guided_wake_cycles = starting_guide ? 2 : 0;
    s->completed = 0;
    s->pending_pattern = 0;
    s->pending_pattern_target = -1;
    s->pending_pattern_seen_ms = 0;
    s->pending_pattern_released = 0;
    s->guided_confirmations = 0;
    s->neutral = s->observed && !s->observed_buttons;
    s->deadline_ms = now_ms + (guided ? GC_SETUP_GUIDED_TIMEOUT_MS
                                      : GC_SETUP_MANUAL_TIMEOUT_MS);
    if (target_is_axis(target)) {
        s->axis_centered = 0;
        if (s->observed && s->neutral && input_centered(&s->input))
            set_axis_reference(s, &s->input);
    }
    ++s->revision;
    if (starting_guide)
        snprintf(s->message, sizeof(s->message),
                 "Wake controller: press and release any button twice. Not mapped.");
    else
        set_prompt(s);
    return 0;
}

static void seed_leftover_pending(GcSetup *s, uint32_t leftover, uint64_t now_ms)
{
    uint32_t primary;
    if (!s->guided || s->target < 0 || target_is_axis(s->target) ||
        s->guided_ready_state || !leftover)
        return;
    primary = leftover & ~(leftover - 1);
    s->pending_pattern = primary;
    s->pending_pattern_target = s->target;
    s->pending_pattern_ms = now_ms;
    s->pending_pattern_released = 0;
    s->guided_confirmations = 1;
    s->pending_pattern_seen_ms = now_ms;
    s->neutral = 0;
    snprintf(s->message, sizeof(s->message),
             "1/3 confirmed for %s. Release, then press it 2 more times.",
             gc_setup_target_name(s->target));
}

static int next_guided_target(int target)
{
    for (size_t index = 0; index < sizeof(order) / sizeof(order[0]); ++index)
        if (order[index] == target)
            return index + 1 < sizeof(order) / sizeof(order[0])
                ? order[index + 1] : GC_SETUP_TARGET_COUNT;
    return GC_SETUP_TARGET_COUNT;
}

static void clear_pending(GcSetup *s)
{
    s->pending_pattern = 0;
    s->pending_pattern_target = -1;
    s->pending_pattern_ms = 0;
    s->pending_pattern_seen_ms = 0;
    s->pending_pattern_released = 0;
    s->guided_confirmations = 0;
}

static int confirm_guided_pattern(GcSetup *s, uint32_t pattern,
                                  uint64_t now_ms)
{
    if (!pattern) {
        if (s->pending_pattern && s->pending_pattern_target == s->target &&
            !s->pending_pattern_released) {
            s->pending_pattern_released = 1;
            ++s->revision;
            snprintf(s->message, sizeof(s->message),
                     "%d/3 confirmed for %s. Press it %d more %s.",
                     s->guided_confirmations, gc_setup_target_name(s->target),
                     GC_SETUP_GUIDED_CONFIRMATIONS - s->guided_confirmations,
                     GC_SETUP_GUIDED_CONFIRMATIONS - s->guided_confirmations == 1
                         ? "time" : "times");
        }
        return 0;
    }

    if (!s->pending_pattern || s->pending_pattern_target != s->target ||
        s->pending_pattern != pattern) {
        s->pending_pattern = pattern;
        s->pending_pattern_target = s->target;
        s->guided_confirmations = 1;
    } else if (!s->pending_pattern_released &&
               now_ms - s->pending_pattern_seen_ms <
                   GC_SETUP_GUIDED_CONFIRM_GAP_MS) {
        s->pending_pattern_seen_ms = now_ms;
        return 0;
    } else {
        ++s->guided_confirmations;
    }
    s->pending_pattern_ms = now_ms;
    s->pending_pattern_seen_ms = now_ms;
    s->pending_pattern_released = 0;
    s->neutral = 0;
    s->deadline_ms = now_ms + GC_SETUP_GUIDED_TIMEOUT_MS;
    ++s->revision;
    if (s->guided_confirmations < GC_SETUP_GUIDED_CONFIRMATIONS) {
        snprintf(s->message, sizeof(s->message),
                 "%d/3 confirmed for %s. Release, then press it %d more %s.",
                 s->guided_confirmations, gc_setup_target_name(s->target),
                 GC_SETUP_GUIDED_CONFIRMATIONS - s->guided_confirmations,
                 GC_SETUP_GUIDED_CONFIRMATIONS - s->guided_confirmations == 1
                     ? "time" : "times");
        return 0;
    }
    return 1;
}

static void repair_duplicate(GcSetup *s, int displaced, int captured,
                             uint64_t now_ms)
{
    if (s->guided_resume_target < 0)
        s->guided_resume_target = next_guided_target(captured);
    gc_setup_arm(s, displaced, 1, now_ms);
    snprintf(s->message, sizeof(s->message),
             "%s saved. That signal was previously saved as %s. "
             "Press and release %s 3 times to fix it.",
             gc_setup_target_name(captured), gc_setup_target_name(displaced),
             gc_setup_target_name(displaced));
}

static void advance(GcSetup *s, uint64_t now_ms)
{
    const int completed_guide = s->guided;
    if (s->guided && s->guided_resume_target >= 0) {
        const int resume = s->guided_resume_target;
        s->guided_resume_target = -1;
        if (resume < GC_SETUP_TARGET_COUNT) {
            gc_setup_arm(s, resume, 1, now_ms);
            return;
        }
    }
    if (s->guided)
        for (size_t index = 0; index + 1 < sizeof(order) / sizeof(order[0]);
             ++index)
            if (order[index] == s->target) {
                gc_setup_arm(s, order[index + 1], 1, now_ms);
                return;
            }
    s->target = -1;
    s->guided = 0;
    s->completed = completed_guide;
    ++s->revision;
    snprintf(s->message, sizeof(s->message),
             "Controls captured. Review mappings, then Save profile.");
}

void gc_setup_tick(GcSetup *s, uint64_t now_ms)
{
    if (!s->active || s->target < 0 || now_ms < s->deadline_ms)
        return;

    /* Guided setup is intentionally hands-off: a controller may simply not
     * have every PlayStation control. Once the wake handshake is over, an
     * expired target is the same operation as pressing Skip in the UI. */
    if (s->guided && !s->guided_ready_state) {
        const int skipped = s->target;
        clear_pending(s);
        advance(s, now_ms);
        if (s->target >= 0)
            snprintf(s->message, sizeof(s->message),
                     "Skipped %s: no input. Continue with %s.",
                     gc_setup_target_name(skipped),
                     gc_setup_target_name(s->target));
        else
            snprintf(s->message, sizeof(s->message),
                     "Skipped %s: no input. Review and save.",
                     gc_setup_target_name(skipped));
        return;
    }

    /* Silence cannot prove readiness. Keep the wake step until input verifies
     * the controller or the user explicitly chooses Skip. */
    if (s->guided && s->guided_ready_state) {
        s->deadline_ms = now_ms + GC_SETUP_GUIDED_TIMEOUT_MS;
        ++s->revision;
        snprintf(s->message, sizeof(s->message),
                 "Wake controller: %s. Not mapped.",
                 s->guided_ready_state == 2 ? "release all buttons fully" :
                 s->guided_wake_cycles == 1 ? "press and release once more" :
                 "press and release any button twice");
        return;
    }

    s->target = -1;
    s->guided = 0;
    s->completed = 0;
    s->pending_pattern = 0;
    s->pending_pattern_target = -1;
    s->pending_pattern_seen_ms = 0;
    s->pending_pattern_released = 0;
    s->guided_confirmations = 0;
    s->guided_ready_state = 0;
    s->guided_wake_cycles = 0;
    ++s->revision;
    snprintf(s->message, sizeof(s->message),
             "No control received. Check controller mode, then try again.");
}

static int commit_raw_pattern(GcSetup *s, uint32_t pattern)
{
    GcHidSource source;
    int other;
    if (!s->raw_learn || !s->raw_valid)
        return 0;
    if (gc_rawmap_source_from_mask(s->raw_report, s->raw_len, pattern,
                                   &source) != 0)
        return 0;
    for (other = 0; other < GC_BUTTON_INPUT15; ++other) {
        const GcHidButtonBinding *binding = &s->profile.hid_buttons[other];
        if (other == s->target || !binding->present)
            continue;
        if (binding->source.page == GC_RAWMAP_PAGE &&
            binding->source.report == GC_RAWMAP_REPORT &&
            binding->source.offset == source.offset &&
            binding->source.bits == source.bits)
            return -1;
    }
    s->profile.hid_buttons[s->target] = (GcHidButtonBinding){
        .source = source, .active_value = 1, .present = 1
    };
    s->profile.button_patterns[s->target].present = 0;
    s->profile.button_patterns[s->target].source_mask = 0;
    s->learned |= gc_button_mask((GcButton)s->target);
    return 1;
}

static int raw_pattern_owner(const GcSetup *s, uint32_t pattern)
{
    GcHidSource source;
    if (!s->raw_valid ||
        gc_rawmap_source_from_mask(s->raw_report, s->raw_len, pattern,
                                   &source) != 0)
        return -1;
    for (int other = 0; other < GC_BUTTON_INPUT15; ++other) {
        const GcHidButtonBinding *binding = &s->profile.hid_buttons[other];
        if (other != s->target && binding->present &&
            binding->source.page == GC_RAWMAP_PAGE &&
            binding->source.report == GC_RAWMAP_REPORT &&
            binding->source.offset == source.offset &&
            binding->source.bits == source.bits)
            return other;
    }
    return -1;
}

static int feed_buttons(GcSetup *s, uint32_t buttons, uint64_t now_ms)
{
    uint32_t previous = s->observed ? s->observed_buttons : 0;
    const uint32_t rising = buttons & ~previous;
    const int was_neutral = s->neutral;
    gc_setup_seed_buttons(s, buttons);
    if (s->target < 0 || target_is_axis(s->target))
        return 1;

    if (s->guided_ready_state == 1) {
        if (buttons) {
            s->guided_ready_state = 2;
            s->pending_pattern = 0;
            s->pending_pattern_target = -1;
            s->pending_pattern_ms = 0;
            s->neutral = 0;
            s->deadline_ms = now_ms + GC_SETUP_GUIDED_TIMEOUT_MS;
            ++s->revision;
            snprintf(s->message, sizeof(s->message),
                     "Wake input detected. Release all buttons fully.");
        }
        return 1;
    }
    if (s->guided_ready_state == 2) {
        if (!buttons) {
            s->guided_ready_state = --s->guided_wake_cycles ? 1 : 0;
            s->neutral = 1;
            s->deadline_ms = now_ms + GC_SETUP_GUIDED_TIMEOUT_MS;
            ++s->revision;
            if (s->guided_ready_state)
                snprintf(s->message, sizeof(s->message),
                         "Wake controller: press and release once more. Not mapped.");
            else
                set_prompt(s);
        }
        return 1;
    }

    if (s->guided) {
        int displaced = -1;
        s->consumed_bits &= buttons;
        buttons &= ~s->consumed_bits;
        previous &= ~s->consumed_bits;
        /* A completed step advances while its physical control may still be
         * down. Ignore consumed bits until they are released so a held CROSS
         * cannot steal CIRCLE, and analog-only packets cannot retarget. */
        if (!s->pending_pattern && !was_neutral && buttons == previous)
            return 1;
        if (!buttons) {
            s->neutral = 1;
            confirm_guided_pattern(s, 0, now_ms);
            return 1;
        }
        /* Confirm on the NEWLY pressed bit, never on every bit currently set.
         * A zero-usage descriptor decoded through the raw path carries analog
         * jitter (and a held control stays set) in the mask, so the lowest set
         * bit changes between frames; confirming on it reset the count to 1/3
         * on every packet and the walk never accumulated. `rising` is the set
         * of bits that were not down in the previous frame, which is the actual
         * press edge. A steady frame that adds no bit keeps the pending
         * pattern instead of retargeting. */
        {
            uint32_t press_bits = rising;
            if (!press_bits && s->pending_pattern &&
                (buttons & s->pending_pattern))
                press_bits = s->pending_pattern;
            if (!press_bits)
                return 1;
            if (!confirm_guided_pattern(s, press_bits & ~(press_bits - 1), now_ms))
                return 1;
        }

        const uint32_t pattern = s->pending_pattern;
        const uint32_t target = gc_button_mask((GcButton)s->target);
        int source = -1;
        if (!(pattern & (pattern - 1)))
            for (int index = 0; index < GC_BUTTON_COUNT; ++index)
                if (gc_button_mask((GcButton)index) == pattern) source = index;

        if (!s->raw_learn && source >= 0 &&
            (s->profile.button_targets[source] & s->learned & ~target)) {
            const uint32_t owners =
                s->profile.button_targets[source] & s->learned & ~target;
            for (int other = 0; other < GC_BUTTON_INPUT15; ++other)
                if (owners & gc_button_mask((GcButton)other)) {
                    displaced = other;
                    s->profile.button_targets[source] &=
                        ~gc_button_mask((GcButton)other);
                    s->learned &= ~gc_button_mask((GcButton)other);
                    break;
                }
        }
        if (!s->raw_learn)
        for (int other = 0; other < GC_BUTTON_INPUT15; ++other) {
            if (other != s->target && s->profile.button_patterns[other].present &&
                s->profile.button_patterns[other].source_mask == pattern) {
                displaced = other;
                s->profile.button_patterns[other].present = 0;
                s->profile.button_patterns[other].source_mask = 0;
                s->learned &= ~gc_button_mask((GcButton)other);
                break;
            }
        }

        {
            if (s->raw_learn) {
                displaced = raw_pattern_owner(s, pattern);
                if (displaced >= 0) {
                    s->profile.hid_buttons[displaced].present = 0;
                    s->learned &= ~gc_button_mask((GcButton)displaced);
                }
            }
            const int raw = commit_raw_pattern(s, pattern);
            if (raw < 0) {
                clear_pending(s);
                ++s->revision;
                snprintf(s->message, sizeof(s->message),
                         "That signal is already used. Choose another control for %s.",
                         gc_setup_target_name(s->target));
                return 1;
            }
            if (raw > 0) {
                const uint32_t leftover = buttons & ~pattern;
                const int captured = s->target;
                clear_pending(s);
                s->consumed_bits |= pattern;
                if (displaced >= 0)
                    repair_duplicate(s, displaced, captured, now_ms);
                else
                    advance(s, now_ms);
                if (!(buttons & pattern))
                    seed_leftover_pending(s, leftover, now_ms);
                return 1;
            }
            if (s->raw_learn) {
                s->pending_pattern = 0;
                s->pending_pattern_target = -1;
                s->pending_pattern_released = 0;
                return 1;
            }
        }
        for (int index = 0; index < GC_BUTTON_COUNT; ++index)
            s->profile.button_targets[index] &= ~target;
        s->profile.button_patterns[s->target].present = 0;
        s->profile.button_patterns[s->target].source_mask = 0;
        if (source >= 0) {
            s->learned &= ~s->profile.button_targets[source];
            s->profile.button_targets[source] = target;
        } else {
            s->profile.button_patterns[s->target].source_mask = pattern;
            s->profile.button_patterns[s->target].present = 1;
        }
        s->learned |= target;
        {
            const uint32_t leftover = buttons & ~pattern;
            const int captured = s->target;
            clear_pending(s);
            s->consumed_bits |= pattern;
            if (displaced >= 0)
                repair_duplicate(s, displaced, captured, now_ms);
            else
                advance(s, now_ms);
            if (!(buttons & pattern))
                seed_leftover_pending(s, leftover, now_ms);
        }
        return 1;
    }

    if (!buttons) {
        s->neutral = 1;
    } else if (rising && !(rising & (rising - 1))) {
        int source = -1;
        const int raw = commit_raw_pattern(s, rising);
        s->pending_pattern = 0;
        s->pending_pattern_target = -1;
        s->pending_pattern_ms = 0;
        if (raw > 0) {
            advance(s, now_ms);
            return 1;
        }
        if (raw < 0) {
            ++s->revision;
            snprintf(s->message, sizeof(s->message),
                     "That signal is already used. Choose another control for %s.",
                     gc_setup_target_name(s->target));
            return 1;
        }
        for (int index = 0; index < GC_BUTTON_COUNT; ++index)
            if (gc_button_mask((GcButton)index) == rising) source = index;
        if (source >= 0) {
            const uint32_t target = gc_button_mask((GcButton)s->target);
            for (int index = 0; index < GC_BUTTON_COUNT; ++index)
                s->profile.button_targets[index] &= ~target;
            s->profile.button_patterns[s->target].present = 0;
            s->profile.button_patterns[s->target].source_mask = 0;
            s->learned &= ~s->profile.button_targets[source];
            s->profile.button_targets[source] = target;
            s->learned |= target;
            advance(s, now_ms);
        }
    } else if (rising && rising == buttons) {
        const uint32_t target = gc_button_mask((GcButton)s->target);
        if (s->pending_pattern == rising &&
            s->pending_pattern_target == s->target) {
            for (int other = 0; other < GC_BUTTON_INPUT15; ++other) {
                if (other != s->target && s->profile.button_patterns[other].present &&
                    s->profile.button_patterns[other].source_mask == rising) {
                    s->neutral = 0;
                    ++s->revision;
                    snprintf(s->message, sizeof(s->message),
                             "That multi-signal input is used. Choose another for %s.",
                             gc_setup_target_name(s->target));
                    return 1;
                }
            }
            for (int index = 0; index < GC_BUTTON_COUNT; ++index)
                s->profile.button_targets[index] &= ~target;
            s->profile.button_patterns[s->target].source_mask = rising;
            s->profile.button_patterns[s->target].present = 1;
            s->learned |= target;
            s->pending_pattern = 0;
            s->pending_pattern_target = -1;
            s->pending_pattern_ms = 0;
            advance(s, now_ms);
        } else {
            s->pending_pattern = rising;
            s->pending_pattern_target = s->target;
            s->pending_pattern_ms = now_ms;
            s->neutral = 0;
            ++s->revision;
            snprintf(s->message, sizeof(s->message),
                     "Multi-signal 0x%08x. Release, then press it again for %s.",
                     rising, gc_setup_target_name(s->target));
        }
    } else if (rising) {
        s->pending_pattern = 0;
        s->pending_pattern_target = -1;
        s->pending_pattern_ms = 0;
        s->neutral = 0;
    }
    return 1;
}

static uint8_t axis_value(const ScePadData *input, GcAxis axis)
{
    switch (axis) {
    case GC_AXIS_LX: return input->leftStick.x;
    case GC_AXIS_LY: return input->leftStick.y;
    case GC_AXIS_RX: return input->rightStick.x;
    case GC_AXIS_RY: return input->rightStick.y;
    case GC_AXIS_L2: return input->analogButtons.l2;
    case GC_AXIS_R2: return input->analogButtons.r2;
    default: return 0;
    }
}

static int normalized_controls_neutral(const GcSetup *s,
                                       const ScePadData *input,
                                       uint32_t buttons)
{
    if (buttons)
        return 0;
    if (!s->axis_reference_valid)
        return 1;
    for (int axis = 0; axis < GC_AXIS_COUNT; ++axis) {
        int delta = axis_value(input, (GcAxis)axis) -
                    axis_value(&s->axis_reference, (GcAxis)axis);
        if (delta < -16 || delta > 16)
            return 0;
    }
    return 1;
}

static void capture_normalized_axis(GcSetup *s, const ScePadData *input,
                                    uint64_t now_ms)
{
    const GcAxis target = (GcAxis)(s->target - GC_SETUP_AXIS_BASE);
    int best = -1, best_delta = 0;
    if (!s->axis_centered) {
        for (int source = 0; source < GC_AXIS_COUNT; ++source) {
            int delta = axis_value(input, (GcAxis)source) -
                        axis_value(&s->axis_reference, (GcAxis)source);
            if (delta < -16 || delta > 16) return;
        }
        s->axis_centered = 1;
        return;
    }
    for (int source = 0; source < GC_AXIS_COUNT; ++source) {
        int delta = axis_value(input, (GcAxis)source) -
                    axis_value(&s->axis_reference, (GcAxis)source);
        if (delta < 0) delta = -delta;
        if (delta > best_delta) { best = source; best_delta = delta; }
    }
    if (best < 0 || best_delta < 48) return;
    const int center = axis_value(&s->axis_reference, (GcAxis)best);
    const int current = axis_value(input, (GcAxis)best);
    for (int other = 0; other < GC_AXIS_COUNT; ++other)
        if (other != (int)target && s->profile.axes[other].present &&
            s->profile.axes[other].source == (GcAxis)best) {
            const int trigger_pair =
                ((target == GC_AXIS_L2 && other == GC_AXIS_R2) ||
                 (target == GC_AXIS_R2 && other == GC_AXIS_L2)) &&
                s->profile.axes[other].polarity !=
                    (current > center ? 1 : -1);
            if (trigger_pair)
                continue;
            ++s->revision;
            snprintf(s->message, sizeof(s->message),
                     "That axis is already used. Center it, then choose another for %s.",
                     gc_setup_target_name(s->target));
            return;
        }
    s->profile.axes[target] = (GcAxisBinding){
        .source = (GcAxis)best, .center = center, .minimum = 0,
        .maximum = 255, .polarity = current > center ? 1 : -1, .present = 1,
    };
    s->profile.hid_axes[target].present = 0;
    s->learned_axes |= 1u << target;
    s->axis_centered = 0;
    advance(s, now_ms);
}

int gc_setup_feed_input(GcSetup *s, const char *path,
                        const ScePadData *input, uint64_t now_ms)
{
    if (!s || !input || !s->active || strcmp(path, s->path)) return 0;
    const int suppress = s->target >= 0 || !s->neutral;
    gc_setup_tick(s, now_ms);
    s->input = *input;
    s->input_ms = now_ms;
    uint32_t buttons = gc_setup_learning_buttons(input);
    /* Trigger activation uses 192, but a wake release must reach rest. */
    if (s->guided_ready_state == 2) {
        if (input->analogButtons.l2 > 16) buttons |= SCE_PAD_BUTTON_L2;
        if (input->analogButtons.r2 > 16) buttons |= SCE_PAD_BUTTON_R2;
    }
    /* L2/R2 are synthesized from analog trigger travel by XUSB parsers. Once
     * their own guided steps are complete, they must not masquerade as the
     * next button while the user is still releasing a trigger. */
    if (!s->guided_ready_state && !target_is_axis(s->target) &&
        s->target != GC_BUTTON_L2 && s->target != GC_BUTTON_R2)
        buttons &= ~(SCE_PAD_BUTTON_L2 | SCE_PAD_BUTTON_R2);

    if (target_is_axis(s->target)) {
        const uint32_t blocking_buttons =
            buttons & ~(SCE_PAD_BUTTON_L2 | SCE_PAD_BUTTON_R2);
        if (!blocking_buttons && !s->axis_centered && input_centered(input))
            set_axis_reference(s, input);
        else if (!blocking_buttons && s->axis_centered)
            capture_normalized_axis(s, input, now_ms);
        gc_setup_seed_buttons(s, buttons);
    } else {
        feed_buttons(s, buttons, now_ms);
    }
    s->neutral = normalized_controls_neutral(s, input, buttons);
    return suppress;
}

void gc_setup_set_raw_span(GcSetup *s, unsigned offset, uint32_t mask)
{
    if (!s) return;
    s->raw_span_offset = offset;
    s->raw_span_mask = mask;
}

/* Analog trigger travel at or below this many counts is at rest for the wake
 * handshake (the same dead-band gc_setup_feed_input applies). */
#define GC_SETUP_WAKE_TRAVEL 16

/* Does this report carry evidence that the parser actually decodes this pad?
 * A descriptor with no standard usages yields an all-zero pad, and only then is
 * the raw report the sole signal for the wake handshake. */
static int wake_pad_reports_controls(const ScePadData *input)
{
    if (!input) return 0;
    return input->buttons != 0 ||
        input->analogButtons.l2 > GC_SETUP_WAKE_TRAVEL ||
        input->analogButtons.r2 > GC_SETUP_WAKE_TRAVEL ||
        input->leftStick.x || input->leftStick.y ||
        input->rightStick.x || input->rightStick.y;
}

int gc_setup_feed_raw(GcSetup *s, const char *path,
                      const uint8_t *report, size_t report_size,
                      const ScePadData *input, uint64_t now_ms)
{
    size_t copy;
    uint32_t bits;
    int suppress;
    if (!s || !report || !input || !s->active || strcmp(path, s->path))
        return 0;
    copy = report_size < sizeof(s->raw_report) ? report_size
                                               : sizeof(s->raw_report);
    memcpy(s->raw_report, report, copy);
    s->raw_len = (uint16_t)copy;
    s->raw_valid = 1;
    suppress = s->target >= 0 || !s->neutral;
    /* Baseline is the first report after arming: every later comparison is a
     * difference from it, so a report whose analog bytes sit at 0x80 and whose
     * tail carries a per-frame counter still reads as "nothing pressed". */
    {
        const size_t bn = report_size < sizeof s->wake_baseline
                              ? report_size : sizeof s->wake_baseline;
        if (!s->wake_baseline_valid || s->wake_baseline_len != report_size) {
            memcpy(s->wake_baseline, report, bn);
            s->wake_baseline_len = (uint16_t)report_size;
            s->wake_baseline_valid = 1;
        }
    }
    if (s->guided_ready_state) {
        /* A pad the parser can read reports its own press and its own release,
         * including trigger travel coming back to rest, so nothing here may
         * second-guess it. The raw delta below is the fallback for a descriptor
         * that decodes to nothing (no standard usages), and it cannot express a
         * release of an analog control: a wake report captured while R2 is still
         * partly held makes that travel the baseline, so full rest (0) sits 197
         * counts away from it and reads as another press. The handshake then
         * never leaves "release all buttons fully". Latch which kind of pad this
         * is on the first report that shows a decoded control, because a report
         * at rest and a report the parser cannot read look identical. */
        if (wake_pad_reports_controls(input))
            s->wake_pad_decoded = 1;
        if (s->wake_pad_decoded)
            return gc_setup_feed_input(s, path, input, now_ms);
        const size_t n = report_size < sizeof s->wake_baseline
                             ? report_size : sizeof s->wake_baseline;
        if (!s->wake_baseline_valid || s->wake_baseline_len != report_size) {
            memcpy(s->wake_baseline, report, n);
            s->wake_baseline_len = (uint16_t)report_size;
            s->wake_baseline_valid = 1;
        }
        int changed = 0;
        const size_t m = n < (size_t)s->wake_baseline_len ? n : s->wake_baseline_len;
        /* Compare only the button/stick span. Bytes past byte 7 carry a
         * per-frame timestamp/counter (and byte 7's high six bits are the
         * report counter), which change on every packet -- diffing the whole
         * report would read as held forever and a release would never land. */
        const size_t span = m < 8 ? m : 8;
        for (size_t i = 0; i < span; ++i) {
            const uint8_t mask = (i == 7) ? 0x03 : 0xFF;
            const int delta = (int)(report[i] & mask) -
                              (int)(s->wake_baseline[i] & mask);
            if (delta > 12 || delta < -12) { changed = 1; break; }
        }
        ScePadData carrier = *input;
        carrier.buttons = changed ? SCE_PAD_BUTTON_CROSS : 0;
        carrier.analogButtons.l2 = 0;
        carrier.analogButtons.r2 = 0;
        return gc_setup_feed_input(s, path, &carrier, now_ms);
    }
    if (target_is_axis(s->target) ||
        s->target == GC_BUTTON_L2 || s->target == GC_BUTTON_R2)
        return gc_setup_feed_input(s, path, input, now_ms);
    gc_setup_tick(s, now_ms);
    s->input = *input;
    s->input_ms = now_ms;
    s->raw_learn = 1;
    if (s->raw_span_mask) {
        /* Field-derived window: the bytes that actually carry this source's
         * digital controls, with the bits of wide fields (a per-frame counter,
         * an axis byte) masked out. XOR against the armed baseline, because a
         * hat direction clears a neutral bit and a release clears a button bit;
         * with the AND-NOT form those changes produce no signal at all. */
        const uint32_t now_bits =
            gc_rawmap_window(report, report_size, s->raw_span_offset) & s->raw_span_mask;
        const uint32_t base_bits =
            (s->wake_baseline_valid && s->wake_baseline_len)
                ? gc_rawmap_window(s->wake_baseline, s->wake_baseline_len,
                                   s->raw_span_offset) & s->raw_span_mask
                : 0;
        bits = now_bits ^ base_bits;
    } else {
        bits = gc_rawmap_digital_mask(report, report_size);
        /* Only bits that differ from the armed baseline count as pressed, so the
         * neutral report releases instead of latching: without this the wizard
         * restarts its 3-press confirmation on every press (1/3 forever). */
        if (s->wake_baseline_valid && s->wake_baseline_len)
            bits &= ~gc_rawmap_digital_mask(s->wake_baseline, s->wake_baseline_len);
    }
    feed_buttons(s, bits, now_ms);
    s->neutral = !bits;
    s->raw_learn = 0;
    return suppress;
}

int gc_setup_feed(GcSetup *s, const char *path, uint32_t buttons,
                  uint64_t now_ms)
{
    ScePadData input = s ? s->input : (ScePadData){0};
    input.buttons = buttons;
    input.analogButtons.l2 = (buttons & SCE_PAD_BUTTON_L2) ? 255 : 0;
    input.analogButtons.r2 = (buttons & SCE_PAD_BUTTON_R2) ? 255 : 0;
    return gc_setup_feed_input(s, path, &input, now_ms);
}

static int hid_button_value(const GcHidValue *value)
{
    return value->source.page == 9 ||
        (value->source.page == 1 &&
         (value->source.usage == 0x39 ||
          (value->source.usage >= 0x90 && value->source.usage <= 0x93)));
}

static int hid_axis_value(const GcHidValue *value)
{
    return !value->is_null &&
        ((value->source.page == 1 && value->source.usage >= 0x30 &&
            value->source.usage <= 0x38) ||
        (value->source.page == 2 &&
         (value->source.usage == 0xba || value->source.usage == 0xbb ||
          value->source.usage == 0xc4 || value->source.usage == 0xc5 ||
          value->source.usage == 0xc8)));
}

static const GcHidValue *find_hid_value(const GcHidValue *values, size_t count,
                                        const GcHidSource *source)
{
    for (size_t index = 0; index < count; ++index)
        if (gc_hid_source_equal(&values[index].source, source))
            return &values[index];
    return NULL;
}

static int hid_axes_neutral(const GcSetup *s)
{
    if (!s->axis_reference_valid)
        return 1;
    for (size_t index = 0; index < s->hid_count; ++index) {
        const GcHidValue *value = &s->hid_values[index];
        if (!hid_axis_value(value))
            continue;
        const GcHidValue *reference = find_hid_value(
            s->hid_reference, s->hid_reference_count, &value->source);
        if (!reference)
            continue;
        int64_t delta = (int64_t)value->value - reference->value;
        if (delta < 0)
            delta = -delta;
        const int64_t range = (int64_t)value->source.maximum -
                              value->source.minimum;
        if (range > 0 && delta * 8 > range)
            return 0;
    }
    return 1;
}

static uint32_t hid_confirmation_key(const GcHidValue *value)
{
    uint32_t key = 2166136261u;
#define MIX(field) key = (key ^ (uint32_t)(field)) * 16777619u
    MIX(value->source.offset);
    MIX(value->source.page);
    MIX(value->source.usage);
    MIX(value->source.bits);
    MIX(value->source.report);
    MIX(value->value);
#undef MIX
    return key ? key : 1;
}

static void capture_hid_button(GcSetup *s, uint32_t normalized_buttons,
                               uint32_t normalized_rising, uint64_t now_ms)
{
    const GcHidValue *pressed = NULL;
    int active = 0, rising = 0;
    for (size_t index = 0; index < s->hid_count; ++index) {
        const GcHidValue *value = &s->hid_values[index];
        if (!hid_button_value(value) || !value->active) continue;
        ++active;
        const GcHidValue *old = find_hid_value(s->hid_reference,
                                               s->hid_reference_count,
                                               &value->source);
        if (old && old->active && old->value == value->value) continue;
        if (value->source.page == 1 && value->source.usage == 0x39) {
            const int64_t position = (int64_t)value->value -
                                     value->source.minimum;
            const int64_t positions = (int64_t)value->source.maximum -
                                      value->source.minimum + 1;
            const int diagonal = positions == 8 && (position & 1);
            if (diagonal) {
                rising = 2;
                pressed = NULL;
                break;
            }
        }
        pressed = value;
        ++rising;
    }
    /* Analog-only triggers still satisfy the earlier button prompt. Their
     * descriptor identity is learned later in the axis phase. */
    if (!rising && (s->target == GC_BUTTON_L2 || s->target == GC_BUTTON_R2)) {
        const uint32_t trigger = gc_button_mask((GcButton)s->target);
        if (s->guided && !(normalized_buttons & trigger)) {
            confirm_guided_pattern(s, 0, now_ms);
            return;
        }
        if (normalized_rising & trigger) {
            if (s->guided && !confirm_guided_pattern(s, trigger, now_ms))
                return;
            s->learned |= gc_button_mask((GcButton)s->target);
            clear_pending(s);
            advance(s, now_ms);
        }
        return;
    }
    if (!rising && s->guided && !active) {
        confirm_guided_pattern(s, 0, now_ms);
        return;
    }
    if (rising != 1) { if (rising > 1) s->neutral = 0; return; }
    if (s->guided &&
        !confirm_guided_pattern(s, hid_confirmation_key(pressed), now_ms))
        return;
    int displaced = -1;
    for (int other = 0; other < GC_BUTTON_INPUT15; ++other) {
        const GcHidButtonBinding *binding = &s->profile.hid_buttons[other];
        if (other != s->target && binding->present &&
            binding->active_value == pressed->value &&
            gc_hid_source_equal(&binding->source, &pressed->source)) {
            if (s->guided) {
                displaced = other;
                s->profile.hid_buttons[other].present = 0;
                s->learned &= ~gc_button_mask((GcButton)other);
                break;
            }
            ++s->revision;
            snprintf(s->message, sizeof(s->message),
                     "That button is already used. Release it, then choose another for %s.",
                     gc_setup_target_name(s->target));
            return;
        }
    }
    s->profile.hid_buttons[s->target] = (GcHidButtonBinding){
        .source = pressed->source, .active_value = pressed->value, .present = 1,
    };
    const int captured = s->target;
    s->learned |= gc_button_mask((GcButton)s->target);
    clear_pending(s);
    if (displaced >= 0)
        repair_duplicate(s, displaced, captured, now_ms);
    else
        advance(s, now_ms);
}

static void capture_hid_axis(GcSetup *s, uint64_t now_ms)
{
    const GcAxis target = (GcAxis)(s->target - GC_SETUP_AXIS_BASE);
    const GcHidValue *best = NULL, *best_old = NULL;
    int64_t best_scaled = 0;
    if (!s->axis_centered) {
        for (size_t index = 0; index < s->hid_count; ++index) {
            const GcHidValue *value = &s->hid_values[index];
            if (!hid_axis_value(value)) continue;
            const GcHidValue *old = find_hid_value(
                s->hid_reference, s->hid_reference_count, &value->source);
            if (!old) continue;
            int64_t delta = (int64_t)value->value - old->value;
            if (delta < 0) delta = -delta;
            int64_t range = (int64_t)value->source.maximum -
                            value->source.minimum;
            if (range > 0 && delta * 8 > range) return;
        }
        s->axis_centered = 1;
        return;
    }
    for (size_t index = 0; index < s->hid_count; ++index) {
        const GcHidValue *value = &s->hid_values[index];
        if (!hid_axis_value(value)) continue;
        const GcHidValue *old = find_hid_value(s->hid_reference,
                                               s->hid_reference_count,
                                               &value->source);
        if (!old) continue;
        int64_t delta = (int64_t)value->value - old->value;
        if (delta < 0) delta = -delta;
        int64_t range = (int64_t)value->source.maximum -
                        value->source.minimum;
        if (range <= 0 || delta * 4 < range) continue;
        int64_t scaled = delta * 1000000 / range;
        if (scaled > best_scaled) {
            best = value; best_old = old; best_scaled = scaled;
        }
    }
    if (!best) return;
    const int polarity = best->value > best_old->value ? 1 : -1;
    for (int other = 0; other < GC_AXIS_COUNT; ++other)
        if (other != (int)target && s->profile.hid_axes[other].present &&
            gc_hid_source_equal(&s->profile.hid_axes[other].source,
                                &best->source)) {
            const int trigger_pair =
                ((target == GC_AXIS_L2 && other == GC_AXIS_R2) ||
                 (target == GC_AXIS_R2 && other == GC_AXIS_L2)) &&
                s->profile.hid_axes[other].polarity != polarity;
            if (trigger_pair) continue;
            ++s->revision;
            snprintf(s->message, sizeof(s->message),
                     "That axis was already learned. Center it, then use a different control for %s.",
                     gc_setup_target_name(s->target));
            return;
        }
    s->profile.hid_axes[target] = (GcHidAxisBinding){
        .source = best->source, .center = best_old->value,
        .polarity = polarity, .present = 1,
    };
    s->learned_axes |= 1u << target;
    s->axis_centered = 0;
    advance(s, now_ms);
}

int gc_setup_feed_hid(GcSetup *s, const char *path, const GcHidLayout *layout,
                      const uint8_t *report, size_t report_size,
                      const ScePadData *input, uint64_t now_ms)
{
    if (!s || !layout || !input || !s->active || strcmp(path, s->path))
        return 0;
    const int suppress = s->target >= 0 || !s->neutral;
    gc_setup_tick(s, now_ms);
    s->input = *input;
    s->input_ms = now_ms;
    GcHidValue values[GC_HID_MAX_FIELDS];
    size_t count = 0;
    if (!gc_hid_values(layout, report, report_size, values,
                       GC_HID_MAX_FIELDS, &count))
        return suppress;
    merge_hid_values(s, values, count);
    s->hid_layout = *layout;
    s->hid = 1;
    int active = 0;
    for (size_t index = 0; index < s->hid_count; ++index)
        if (hid_button_value(&s->hid_values[index]))
            active |= s->hid_values[index].active;
    const uint32_t setup_buttons = gc_setup_learning_buttons(input);
    const uint32_t normalized_rising = setup_buttons & ~s->observed_buttons;
    s->observed = 1;
    s->observed_buttons = setup_buttons;
    s->neutral = !active && hid_axes_neutral(s);
    const int was_axis_target = target_is_axis(s->target);
    if (s->target >= 0) {
        if (was_axis_target) {
            if (!active && !s->axis_centered && input_centered(input))
                set_axis_reference(s, input);
            else if (!active && s->axis_centered)
                capture_hid_axis(s, now_ms);
        }
        else if (s->guided_ready_state)
            feed_buttons(s, setup_buttons, now_ms);
        else
            capture_hid_button(s, setup_buttons, normalized_rising, now_ms);
    }
    if (!was_axis_target) {
        memcpy(s->hid_reference, s->hid_values,
               s->hid_count * sizeof(s->hid_values[0]));
        s->hid_reference_count = s->hid_count;
    }
    s->neutral = !active && hid_axes_neutral(s);
    return suppress;
}

void gc_setup_skip(GcSetup *s, uint64_t now_ms)
{
    if (!s->active || s->target < 0) return;
    if (s->guided_ready_state) {
        s->guided_ready_state = 0;
        s->guided_wake_cycles = 0;
        s->pending_pattern = 0;
        s->pending_pattern_target = -1;
        s->pending_pattern_seen_ms = 0;
        s->pending_pattern_released = 0;
        s->deadline_ms = now_ms + GC_SETUP_GUIDED_TIMEOUT_MS;
        ++s->revision;
        set_prompt(s);
        return;
    }
    advance(s, now_ms);
}

void gc_setup_cancel(GcSetup *s, const char *message)
{
    s->active = 0;
    s->target = -1;
    s->completed = 0;
    s->pending_pattern = 0;
    s->pending_pattern_target = -1;
    s->guided_ready_state = 0;
    s->guided_wake_cycles = 0;
    s->guided_confirmations = 0;
    s->guided_resume_target = -1;
    s->consumed_bits = 0;
    ++s->revision;
    snprintf(s->message, sizeof(s->message), "%s", message);
}

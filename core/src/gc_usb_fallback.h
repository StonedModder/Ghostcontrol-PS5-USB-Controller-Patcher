/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#define GC_USB_FALLBACK_INTERVAL_MS 250u
#define GC_USB_INTERRUPT_RECENT_MS 1000u

/* Owned by one UsbFeed; its open/close memset resets every field. */
typedef struct {
    uint64_t interrupt_in_received, get_report_attempts;
    uint64_t get_report_successes, get_report_failures;
    uint64_t last_interrupt_ms, last_attempt_ms;
    unsigned ticks;
    int have_interrupt, have_attempt, control_decoded;
} GcUsbFallback;

static inline void gc_usb_fallback_interrupt(GcUsbFallback *state, uint64_t now)
{
    ++state->interrupt_in_received;
    state->last_interrupt_ms = now;
    state->have_interrupt = 1;
}

static inline int gc_usb_fallback_due(GcUsbFallback *state, uint64_t now)
{
    /* A decoded control sample is not proof that interrupt input resumed.
     * Keep polling a quiet pipe so later presses/releases can be delivered. */
    if (state->have_interrupt && (now < state->last_interrupt_ms ||
        now - state->last_interrupt_ms < GC_USB_INTERRUPT_RECENT_MS)) return 0;
    if (state->ticks < 6) ++state->ticks;
    if (state->ticks < 6) return 0;
    if (state->have_attempt && (now < state->last_attempt_ms ||
        now - state->last_attempt_ms < GC_USB_FALLBACK_INTERVAL_MS)) return 0;
    state->ticks = 0;
    state->last_attempt_ms = now;
    state->have_attempt = 1;
    ++state->get_report_attempts;
    return 1;
}

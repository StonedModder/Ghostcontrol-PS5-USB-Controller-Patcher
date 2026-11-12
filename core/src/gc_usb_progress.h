/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* Diagnostic state only. Never authorizes a restart, close or publication. */
typedef struct {
    uint64_t generation, opened_ms, started_ms, completed_ms, input_ms, sampled_ms;
    uint64_t starts, completions, busy_returns, other_errors, inputs, decoded, rejected;
    int have_completion, have_input, have_sample, outstanding, suspect, poll_revents;
} GcUsbProgress;

static inline uint64_t gc_usb_progress_age(uint64_t now, uint64_t then)
{
    return now >= then ? now - then : 0;
}
static inline void gc_usb_progress_start(GcUsbProgress *p, uint64_t now)
{
    ++p->starts; p->started_ms = now; p->outstanding = 1;
}
static inline void gc_usb_progress_complete(GcUsbProgress *p, uint64_t now)
{
    ++p->completions; p->completed_ms = now; p->have_completion = 1; p->outstanding = 0;
}
static inline void gc_usb_progress_input(GcUsbProgress *p, uint64_t now)
{
    ++p->inputs; p->input_ms = now; p->have_input = 1;
}
static inline int gc_usb_progress_due(GcUsbProgress *p, uint64_t now)
{
    if (p->have_sample && gc_usb_progress_age(now, p->sampled_ms) < 1000) return 0;
    p->have_sample = 1; p->sampled_ms = now; return 1;
}
static inline int gc_usb_progress_suspect(const GcUsbProgress *p, uint64_t now)
{
    return p->outstanding && gc_usb_progress_age(now, p->started_ms) >= 1000 &&
        gc_usb_progress_age(now, p->have_completion ? p->completed_ms : p->opened_ms) >= 1000;
}

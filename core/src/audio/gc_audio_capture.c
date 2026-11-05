/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-07 capture adapter. See gc_audio_capture.h for the contract this
 * implements and docs/AUDIO-07-CONTRACTS.md section 1 for the platform facts.
 *
 * Locking model. One mutex guards the slots and the stats. It is NEVER held
 * across a backend call, and never held while the adapter waits: a read
 * polls the service with the lock released, then re-takes it to publish the
 * outcome. cancel takes the lock, sets the flag and broadcasts, so a read
 * inside its bounded wait returns at once. close takes the lock, refuses if
 * the handle is being read, and does the service teardown outside it with
 * the slot parked in `closing` (so no open can pick it up meanwhile).
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* clock_gettime, mkdtemp users of this header */
#endif
#include "gc_audio_capture.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The float32 sample the service hands us, clamped into the seam's s16.
 * A NaN becomes silence; out-of-range values saturate instead of wrapping
 * (the conversion itself must stay in range, or it is undefined). */
static int16_t to_s16(float value)
{
    if (!(value == value)) return 0;
    if (value >= 1.0f) return 32767;
    if (value <= -1.0f) return -32768;
    return (int16_t)(value * 32767.0f);
}

/* The caller may pass any handle; only ours are ever used. */
static GcAudioCaptureSlot *slot_of(GcAudioCapture *capture, const void *handle)
{
    const uintptr_t base = (uintptr_t)(const void *)capture->slots;
    const uintptr_t slot_size = sizeof(capture->slots[0]);
    const uintptr_t address = (uintptr_t)handle;
    if (!handle) return NULL;
    if (address < base || address - base > slot_size * (sizeof(capture->slots) / slot_size) - slot_size) return NULL;
    if ((address - base) % slot_size) return NULL;
    return &capture->slots[(address - base) / slot_size];
}

static int backend_complete(const GcAudioCaptureBackend *backend)
{
    return backend && backend->initialize && backend->open && backend->start && backend->read &&
           backend->stop && backend->close;
}

int gc_audio_capture_init(GcAudioCapture *capture, const GcAudioCaptureBackend *backend)
{
    if (!capture) return -1;
    memset(capture, 0, sizeof(*capture));
    if (pthread_mutex_init(&capture->mutex, NULL)) return -1;
    if (pthread_cond_init(&capture->ready, NULL)) {
        pthread_mutex_destroy(&capture->mutex);
        return -1;
    }
    capture->stats.last_error = GC_AUDIO_SOURCE_UNAVAILABLE;
    if (!backend_complete(backend)) return -1; /* absent: reported, never substituted */
    capture->backend = backend;
    capture->stats.init_calls = 1;
    if (backend->initialize(backend->ctx)) {
        capture->stats.init_failures = 1;
        return -1;
    }
    capture->available = 1;
    capture->stats.available = 1;
    capture->stats.last_error = GC_AUDIO_OK;
    return 0;
}

void gc_audio_capture_destroy(GcAudioCapture *capture)
{
    if (!capture) return;
    pthread_cond_destroy(&capture->ready);
    pthread_mutex_destroy(&capture->mutex);
}

/* Rejects malformed service records before anything is copied out: a record
 * is a whole number of 8-byte stereo float32 frames and never more than the
 * record buffer. */
static int record_is_whole(size_t bytes)
{
    return bytes > 0 && bytes % GC_AUDIO_CAPTURE_FRAME_BYTES == 0 && bytes <= GC_AUDIO_CAPTURE_RECORD_BYTES;
}

/* Hands at most max_frames frames out of the staging buffer and keeps the
 * rest. Mutex held; always releases it. */
static long serve_locked(GcAudioCapture *capture, GcAudioCaptureSlot *slot, int16_t *frames, size_t max_frames,
                         uint64_t *first_sample)
{
    if (slot->cancelled) {
        ++capture->stats.cancelled_reads;
        pthread_mutex_unlock(&capture->mutex);
        return 0;
    }
    const size_t have = slot->staged / GC_AUDIO_CAPTURE_FRAME_BYTES;
    const size_t give = have < max_frames ? have : max_frames;
    for (size_t i = 0; i < give * GC_AUDIO_CAPTURE_CHANNELS; ++i) frames[i] = to_s16(slot->stage.f[i]);
    slot->staged -= give * GC_AUDIO_CAPTURE_FRAME_BYTES;
    if (slot->staged)
        memmove(slot->stage.bytes, slot->stage.bytes + give * GC_AUDIO_CAPTURE_FRAME_BYTES, slot->staged);
    if (first_sample) *first_sample = slot->next_sample;
    slot->next_sample += give;
    capture->stats.frames += give;
    pthread_mutex_unlock(&capture->mutex);
    return (long)give;
}

/* Bounded wait between no-wait polls; cancel broadcasts and cuts it short. */
static void capture_wait(GcAudioCapture *capture, GcAudioCaptureSlot *slot, unsigned usec)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) return;
    deadline.tv_nsec += (long)usec * 1000;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&capture->mutex);
    if (!slot->cancelled) pthread_cond_timedwait(&capture->ready, &capture->mutex, &deadline);
    pthread_mutex_unlock(&capture->mutex);
}

/* The service's own overrun counter, read WITHOUT our lock (a backend call),
 * and the cross-check against what this adapter saw. */
static uint64_t service_overruns(const GcAudioCapture *capture)
{
    const GcAudioCaptureBackend *backend = capture->backend;
    return backend && backend->overruns ? backend->overruns(backend->ctx) : 0;
}

static void publish_service_overruns_locked(GcAudioCapture *capture, uint64_t service)
{
    capture->stats.service_overruns = service;
    capture->stats.unseen_overruns =
        service > capture->stats.overruns ? service - capture->stats.overruns : 0;
}

/* Section 1 lifecycle: "nothing to capture yet" (the service's own status)
 * means the session has no source; tear it down and open a fresh one. No
 * lock is held here; the slot is parked between teardown and reopen. */
static int restart_session(GcAudioCapture *capture, GcAudioCaptureSlot *slot)
{
    const GcAudioCaptureBackend *backend = capture->backend;
    pthread_mutex_lock(&capture->mutex);
    if (slot->cancelled || !slot->in_use || slot->restarts >= GC_AUDIO_CAPTURE_MAX_RESTARTS) {
        pthread_mutex_unlock(&capture->mutex);
        return -1;
    }
    ++slot->restarts;
    ++capture->stats.restarts;
    slot->silent_polls = 0;
    slot->staged = 0;
    void *stale = slot->service;
    slot->service = NULL;
    slot->started = 0;
    pthread_mutex_unlock(&capture->mutex);

    if (stale) {
        backend->stop(backend->ctx, stale);
        backend->close(backend->ctx, stale);
        pthread_mutex_lock(&capture->mutex);
        ++capture->stats.stops;
        ++capture->stats.service_closes;
        pthread_mutex_unlock(&capture->mutex);
    }
    void *fresh = NULL;
    if (backend->open(backend->ctx, &fresh) || !fresh) return -1;
    if (backend->start(backend->ctx, fresh)) {
        backend->close(backend->ctx, fresh);
        return -1;
    }
    pthread_mutex_lock(&capture->mutex);
    ++capture->stats.starts;
    if (slot->cancelled || !slot->in_use) {
        pthread_mutex_unlock(&capture->mutex);
        backend->stop(backend->ctx, fresh);
        backend->close(backend->ctx, fresh);
        return -1;
    }
    slot->service = fresh;
    slot->started = 1;
    pthread_mutex_unlock(&capture->mutex);
    return 0;
}

/* ---- the seam's describe ---- */

static int capture_describe(void *ctx, GcAudioSourceInfo *out)
{
    GcAudioCapture *capture = ctx;
    if (!capture || !out) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", GC_AUDIO_CAPTURE_ID);
    out->scope = GC_AUDIO_SCOPE_AUTHORIZED_EXTERNAL;
    out->sample_rate = GC_AUDIO_CAPTURE_RATE;
    out->channels = (uint8_t)GC_AUDIO_CAPTURE_CHANNELS;
    out->consent_required = 1; /* validated, never relied on: consent is always explicit */
    if (!capture->available) {
        out->available = 0;
        /* Printable, terminated ASCII only; the module rejects anything else. */
        snprintf(out->unavailable_reason, sizeof(out->unavailable_reason),
                 "%s", capture->backend ? "capture service refused to initialize" : "no capture service");
        return 0;
    }
    out->available = 1;
    return 0;
}

static int capture_open(void *ctx, uint64_t consent_generation, void **handle)
{
    GcAudioCapture *capture = ctx;
    if (!capture || !handle) return -1;
    *handle = NULL;
    const GcAudioCaptureBackend *backend = capture->backend;

    pthread_mutex_lock(&capture->mutex);
    GcAudioCaptureSlot *slot = NULL;
    for (size_t i = 0; i < sizeof(capture->slots) / sizeof(capture->slots[0]); ++i) {
        if (!capture->slots[i].in_use && !capture->slots[i].closing && !capture->slots[i].reading) {
            slot = &capture->slots[i];
            break;
        }
    }
    if (!capture->available || !backend || !slot) {
        ++capture->stats.refused_opens;
        capture->stats.last_error = GC_AUDIO_SOURCE_UNAVAILABLE;
        pthread_mutex_unlock(&capture->mutex);
        return -1;
    }
    slot->in_use = 1; /* reserve first: a second open cannot pick the same slot */
    slot->closing = 0;
    slot->reading = 0;
    slot->cancelled = 0;
    slot->started = 0;
    slot->service = NULL;
    slot->generation = consent_generation;
    slot->next_sample = 0;
    slot->silent_polls = 0;
    slot->restarts = 0;
    slot->staged = 0;
    pthread_mutex_unlock(&capture->mutex);

    void *service = NULL;
    if (backend->open(backend->ctx, &service) || !service) {
        pthread_mutex_lock(&capture->mutex);
        slot->in_use = 0;
        ++capture->stats.failed_opens;
        capture->stats.last_error = GC_AUDIO_SOURCE_UNAVAILABLE;
        pthread_mutex_unlock(&capture->mutex);
        return -1; /* no handle handed out alongside a failure */
    }
    if (backend->start(backend->ctx, service)) {
        backend->close(backend->ctx, service);
        pthread_mutex_lock(&capture->mutex);
        slot->in_use = 0;
        ++capture->stats.failed_starts;
        capture->stats.last_error = GC_AUDIO_SOURCE_UNAVAILABLE;
        pthread_mutex_unlock(&capture->mutex);
        return -1;
    }

    pthread_mutex_lock(&capture->mutex);
    slot->service = service;
    slot->started = 1;
    ++capture->stats.starts;
    ++capture->stats.opens;
    capture->stats.last_error = GC_AUDIO_OK;
    pthread_mutex_unlock(&capture->mutex);
    *handle = slot;
    return 0;
}

static long capture_read(void *ctx, void *handle, int16_t *frames, size_t max_frames, uint64_t *first_sample)
{
    GcAudioCapture *capture = ctx;
    if (!capture || !frames || !max_frames) return -1;
    GcAudioCaptureSlot *slot = slot_of(capture, handle);
    if (!slot) return -1;
    const GcAudioCaptureBackend *backend = capture->backend;
    long result = 0;
    unsigned polls = 0;

    pthread_mutex_lock(&capture->mutex);
    if (!slot->in_use || slot->closing || !capture->available || !backend) {
        if (!slot->in_use || slot->closing) ++capture->stats.stale_reads;
        pthread_mutex_unlock(&capture->mutex);
        return -1;
    }
    if (slot->reading) { /* one read at a time: refused, not raced */
        ++capture->stats.overlapping_reads;
        pthread_mutex_unlock(&capture->mutex);
        return 0;
    }
    slot->reading = 1;
    const int already_cancelled = slot->cancelled;
    pthread_mutex_unlock(&capture->mutex);
    if (already_cancelled) goto done;

    for (;;) {
        pthread_mutex_lock(&capture->mutex);
        if (slot->staged) { /* whole frames only, staged from an earlier record */
            result = serve_locked(capture, slot, frames, max_frames, first_sample);
            goto done;
        }
        if (slot->cancelled) {
            ++capture->stats.cancelled_reads;
            pthread_mutex_unlock(&capture->mutex);
            goto done;
        }
        pthread_mutex_unlock(&capture->mutex);

        const long bytes = backend->read(backend->ctx, slot->service, slot->stage.bytes,
                                         GC_AUDIO_CAPTURE_RECORD_BYTES);
        const uint64_t service = service_overruns(capture);

        pthread_mutex_lock(&capture->mutex);
        ++capture->stats.reads;
        publish_service_overruns_locked(capture, service);
        if (bytes > 0) {
            if (!record_is_whole((size_t)bytes)) {
                ++capture->stats.rejected_records;
                capture->stats.last_error = GC_AUDIO_SOURCE_UNAVAILABLE;
                pthread_mutex_unlock(&capture->mutex);
                result = -2; /* malformed provider output: the module ends the session */
                goto done;
            }
            ++capture->stats.records;
            slot->staged = (size_t)bytes;
            slot->silent_polls = 0;
            pthread_mutex_unlock(&capture->mutex);
            continue;
        }
        if (bytes == GC_AUDIO_CAPTURE_READ_OVERRUN) {
            /* Data loss, not an error: count it and let the sample index show
             * a forward gap (one record is the least that can have been lost). */
            ++capture->stats.overruns;
            capture->stats.dropped_frames += GC_AUDIO_CAPTURE_RECORD_FRAMES;
            slot->next_sample += GC_AUDIO_CAPTURE_RECORD_FRAMES;
            slot->silent_polls = 0;
            pthread_mutex_unlock(&capture->mutex);
            if (++polls >= GC_AUDIO_CAPTURE_POLLS_PER_READ) goto done;
            continue;
        }
        if (bytes == 0) { /* nothing ready yet */
            ++slot->silent_polls;
            const int restart = slot->silent_polls >= GC_AUDIO_CAPTURE_SILENT_POLLS;
            pthread_mutex_unlock(&capture->mutex);
            if (restart) {
                if (restart_session(capture, slot)) {
                    result = -1; /* the restart path failed: the source is gone */
                    goto done;
                }
                continue;
            }
            if (++polls >= GC_AUDIO_CAPTURE_POLLS_PER_READ) goto done;
            capture_wait(capture, slot, GC_AUDIO_CAPTURE_POLL_US);
            continue;
        }
        /* The service refused, or its session is gone: end of source. */
        capture->stats.last_error = GC_AUDIO_SOURCE_UNAVAILABLE;
        pthread_mutex_unlock(&capture->mutex);
        result = -1;
        goto done;
    }
done:
    pthread_mutex_lock(&capture->mutex);
    slot->reading = 0;
    pthread_mutex_unlock(&capture->mutex);
    return result;
}

static void capture_cancel(void *ctx, void *handle)
{
    GcAudioCapture *capture = ctx;
    if (!capture) return;
    GcAudioCaptureSlot *slot = slot_of(capture, handle);
    pthread_mutex_lock(&capture->mutex);
    if (!slot || !slot->in_use || slot->closing) {
        ++capture->stats.stale_cancels;
    } else {
        slot->cancelled = 1;
        ++capture->stats.cancels;
    }
    pthread_cond_broadcast(&capture->ready); /* wakes a read inside its bounded wait */
    pthread_mutex_unlock(&capture->mutex);
}

static void capture_close(void *ctx, void *handle)
{
    GcAudioCapture *capture = ctx;
    if (!capture) return;
    GcAudioCaptureSlot *slot = slot_of(capture, handle);
    pthread_mutex_lock(&capture->mutex);
    if (!slot || !slot->in_use || slot->closing) {
        ++capture->stats.stale_closes;
        pthread_mutex_unlock(&capture->mutex);
        return;
    }
    if (slot->reading) {
        /* The seam forbids this: closing here would close the service session
         * under the reader. Refuse and make the fault visible. */
        ++capture->stats.closes_during_read;
        pthread_mutex_unlock(&capture->mutex);
        return;
    }
    if (!slot->cancelled) ++capture->stats.closes_without_cancel;
    const int started = slot->started;
    void *service = slot->service;
    capture->stats.discarded_frames += slot->staged / GC_AUDIO_CAPTURE_FRAME_BYTES;
    ++capture->stats.closes;
    slot->closing = 1;
    pthread_mutex_unlock(&capture->mutex);

    const GcAudioCaptureBackend *backend = capture->backend;
    if (service) {
        if (started) {
            backend->stop(backend->ctx, service);
            pthread_mutex_lock(&capture->mutex);
            ++capture->stats.stops;
            pthread_mutex_unlock(&capture->mutex);
        }
        backend->close(backend->ctx, service);
        pthread_mutex_lock(&capture->mutex);
        ++capture->stats.service_closes;
        pthread_mutex_unlock(&capture->mutex);
    }

    pthread_mutex_lock(&capture->mutex);
    slot->in_use = 0;
    slot->closing = 0;
    slot->reading = 0;
    slot->cancelled = 0;
    slot->started = 0;
    slot->service = NULL;
    slot->staged = 0;
    pthread_mutex_unlock(&capture->mutex);
}

GcAudioProvider gc_audio_capture_provider(GcAudioCapture *capture)
{
    return (GcAudioProvider){capture, capture_describe, capture_open, capture_read, capture_cancel, capture_close};
}

void gc_audio_capture_stats(GcAudioCapture *capture, GcAudioCaptureStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!capture) return;
    const uint64_t service = service_overruns(capture); /* outside the lock */
    pthread_mutex_lock(&capture->mutex);
    publish_service_overruns_locked(capture, service);
    capture->stats.available = capture->available;
    *out = capture->stats;
    pthread_mutex_unlock(&capture->mutex);
}

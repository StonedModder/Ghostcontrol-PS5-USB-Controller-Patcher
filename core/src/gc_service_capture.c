/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-07 wiring. See gc_service_capture.h.
 */
#include "gc_service_capture.h"

#include "audio/gc_audio_capture.h"
#if defined(__PROSPERO__)
#include "audio/gc_audio_capture_sce.h"
#endif

#include <stdlib.h>
#include <string.h>

struct GcServiceCapture {
    GcAudioCapture capture;       /* the adapter object the seam points at */
    GcAudioProvider provider;     /* the seam handed to the module */
    const GcAudioCaptureBackend *backend;
    int available;
};

GcServiceCapture *gc_service_capture_attach(const GcServiceAudioOptions *base, GcServiceAudioOptions *out)
{
    if (!out) return NULL;
    if (base) *out = *base;
    else memset(out, 0, sizeof(*out));

    GcServiceCapture *owner = calloc(1, sizeof(*owner));
    if (!owner) return NULL; /* *out stays as base: no provider, reported absent */

#if defined(__PROSPERO__)
    /* The console backend refuses (returns NULL) unless every ABI value it
     * needs is established, so a half-known platform cannot be half-used. */
    owner->backend = gc_audio_capture_sce_backend();
#endif
    /* With no backend the adapter still returns a complete seam and reports
     * itself unavailable; the module then reports source_unavailable. */
    (void)gc_audio_capture_init(&owner->capture, owner->backend);
    owner->provider = gc_audio_capture_provider(&owner->capture);
    {
        GcAudioCaptureStats stats;
        gc_audio_capture_stats(&owner->capture, &stats);
        owner->available = stats.available;
    }
    out->provider = &owner->provider;
    return owner;
}

const GcAudioProvider *gc_service_capture_provider(GcServiceCapture *capture)
{
    return capture ? &capture->provider : NULL;
}

int gc_service_capture_available(GcServiceCapture *capture)
{
    return capture ? capture->available : 0;
}

void gc_service_capture_close(GcServiceCapture *capture)
{
    if (!capture) return;
    /* The audio owner is already closed, so no seam call can be in flight. */
    gc_audio_capture_destroy(&capture->capture);
    free(capture);
}

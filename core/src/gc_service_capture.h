/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-07 wiring: the service's PCM provider is the capture adapter.
 *
 * The module (audio/gc_audio.c) takes its PCM source as an injected seam. This
 * file owns one adapter instance for the life of the service's audio owner and
 * hands that seam to gc_service_audio_open(), so the service no longer runs
 * with "no PCM source provider":
 *
 *   console (target, __PROSPERO__): the adapter's backend is
 *     gc_audio_capture_sce_backend() -- the console capture service, opened the
 *     way Remote Play opens it, source 2 (game + system sounds), raw 48 kHz
 *     stereo float32. Its ABI values are established in that file.
 *   host: there is no capture service, so the adapter is initialised with no
 *     backend and reports source_unavailable, exactly as before. Nothing is
 *     stubbed or faked into claiming a source.
 *
 * Lifecycle order matters and is enforced here: the adapter is created before
 * the audio owner and destroyed after it, because the module calls into the
 * seam only between those two points (gc_audio_shutdown waits for every call
 * inside the object, and gc_audio_capture_destroy frees the seam's lock).
 */
#ifndef GC_SERVICE_CAPTURE_H
#define GC_SERVICE_CAPTURE_H

#include "gc_service_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcServiceCapture GcServiceCapture;

/* Fills *out with `base` (or a zeroed option set when base is NULL) plus this
 * adapter's provider seam, and returns the owner to hand to
 * gc_service_capture_close() after the audio owner is closed. Never returns a
 * NULL provider: with no usable backend the seam still describes itself as
 * unavailable, which is what the module reports. Returns NULL only when the
 * owner cannot be allocated, in which case *out is left as `base`. */
GcServiceCapture *gc_service_capture_attach(const GcServiceAudioOptions *base, GcServiceAudioOptions *out);

/* The seam actually handed to the module (never NULL for a live owner). */
const GcAudioProvider *gc_service_capture_provider(GcServiceCapture *capture);

/* 1 when a real capture backend is present and initialised, 0 when the adapter
 * is reporting source_unavailable. Passive: opens no session. */
int gc_service_capture_available(GcServiceCapture *capture);

/* Call only after the audio owner has been closed. */
void gc_service_capture_close(GcServiceCapture *capture);

#ifdef __cplusplus
}
#endif

#endif

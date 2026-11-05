/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-07 capture adapter, console side: the backend table that talks to the
 * platform's own capture service (libSceAvcap2). Target only -- the host build
 * never sees any of this, and gc_audio_capture.c never references it, so the
 * adapter stays testable on a host with a faked service.
 *
 * Wiring (not done here, and deliberately not done by this file): the service
 * that owns the module passes gc_audio_capture_sce_backend() to
 * gc_audio_capture_init(). Nothing in this tree calls it yet.
 */
#ifndef GC_AUDIO_CAPTURE_SCE_H
#define GC_AUDIO_CAPTURE_SCE_H

#include "gc_audio_capture.h"

#ifdef __PROSPERO__
/* The console backend, or NULL when the platform side is not established
 * (see gc_audio_capture_sce.c: unresolved NIDs or unestablished ABI values
 * make it unavailable rather than guessed). */
const GcAudioCaptureBackend *gc_audio_capture_sce_backend(void);
#endif

#endif

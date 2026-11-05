/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GC_APP_SERVICE_AUDIO_H
#define GC_APP_SERVICE_AUDIO_H
#include "gc_service_audio.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GcAppService GcAppService;
/* External authorized adapter injection, before IPC publication. NULL is the
 * production default: no PCM, transport, inventory, encoder or capture. */
GcAppService *gc_app_service_open_audio(const char *, unsigned short, char *, size_t, const GcServiceAudioOptions *);
GcAudioError gc_app_service_audio_event(GcAppService *, uint64_t, GcAudioTransportEvent);
#ifdef __cplusplus
}
#endif
#endif

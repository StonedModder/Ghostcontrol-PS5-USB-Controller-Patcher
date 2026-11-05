/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GC_SERVICE_AUDIO_H
#define GC_SERVICE_AUDIO_H
#include "audio/gc_audio.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GC_SERVICE_AUDIO_PEERS 16
/* Independent measured profile inventory. Saved preferences are not peers. */
typedef enum { GC_AUDIO_PEER_HEADSET=1, GC_AUDIO_PEER_PAD=2, GC_AUDIO_PEER_UNKNOWN=3 } GcAudioPeerKind;
typedef struct { char address[GC_AUDIO_ADDRESS_TEXT]; GcAudioPeerKind kind; } GcAudioPeer;
typedef struct {
    int available;
    uint64_t generation;
    size_t count;
    GcAudioPeer peers[GC_SERVICE_AUDIO_PEERS];
} GcAudioInventory;
typedef struct {
    const GcAudioProvider *provider;
    const GcAudioTransport *transport;
    const GcAudioEncoder *encoder;
    void *inventory_context;
    /* Bounded passive observation only; no discovery/acquisition. */
    int (*inventory)(void *, GcAudioInventory *);
} GcServiceAudioOptions;
typedef struct GcServiceAudio GcServiceAudio;
GcServiceAudio *gc_service_audio_open(const char *root, uint64_t instance, const GcServiceAudioOptions *options);
void gc_service_audio_close(GcServiceAudio *audio);
/* NULL means a checked ordinary release is available, not yet completed.
 * External void provider/transport close callbacks cannot attest release. */
const char *gc_service_audio_release_blocker(const GcServiceAudio *audio);
int gc_service_audio_close_checked(GcServiceAudio *audio);
int gc_service_audio_diagnostic(const GcServiceAudio *audio);
int gc_service_audio_close_diagnostic(GcServiceAudio *audio);
/* 1 = not handled; otherwise bounded newline JSON. Serialized by owner. */
int gc_service_audio_command(GcServiceAudio *, const char *, const char *, char *, size_t);
/* Authorized transport adapter only. Not exposed as a wire command. */
GcAudioError gc_service_audio_transport_event(GcServiceAudio *, uint64_t, GcAudioTransportEvent);
#ifdef __cplusplus
}
#endif
#endif

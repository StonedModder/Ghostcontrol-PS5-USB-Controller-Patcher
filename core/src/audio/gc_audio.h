/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-01: the ordinary headset-audio contract.
 *
 * One owner object answers "what can audio do here, what is it doing, and
 * with whose permission". It never opens a radio, captures PCM or starts a
 * stream on its own; everything that touches the outside world comes in
 * through three injected seams, and an absent seam is reported as exactly
 * that:
 *
 *   GcAudioProvider   a PCM source (test tone, app-owned audio, or an
 *                     explicitly authorized external source). None means
 *                     source_unavailable -- there is no fallback capture.
 *   GcAudioTransport  the already-authorized Bluetooth owner's profile
 *                     channel. Absent adapter = radio_unavailable; a foreign
 *                     owner = radio_owned_elsewhere. No reset/takeover.
 *   GcAudioEncoder    whether an encoder for the negotiated codec exists.
 *
 * Rules the host tests pin:
 *   - Capability booleans separate ordinary_host_implemented, service_wired,
 *     provider_available, transport_authorized, encoder_available and
 *     target_verified. Only the first is true from this module alone; none
 *     is inferred from compilation. Microphone, LE Audio and game/system
 *     capture are explicit, permanently unavailable entries.
 *   - Querying capabilities or a snapshot never opens, reads or starts
 *     anything.
 *   - START needs a canonical headset address, the provider's own source id,
 *     the CURRENT consent generation with consent granted for that source,
 *     an available provider, an authorized transport and an encoder. The
 *     local checks (shutdown, busy, consent, generation) run first and a
 *     refusal there calls no seam at all; every refusal changes no state: no
 *     provider open, no transport call, no session or generation change.
 *   - Sessions: every start and every release bumps the session generation.
 *     Every release names the session it ends and re-checks it under the same
 *     lock that detaches the provider handle, so a late failure for an old
 *     session can never end a newer one.
 *   - Transport events are accepted only for the live session and only after
 *     open_stream has been issued for it; anything else is counted and
 *     ignored. "streaming" only after the transport reports START accepted;
 *     RESUMED is valid only from SUSPENDED.
 *   - close_stream(session) is called exactly once for every session
 *     open_stream was called for, and never before that open_stream call has
 *     returned (a release while it is in flight leaves the close to start()).
 *   - The provider handle is cancelled and closed exactly once, and never
 *     closed while a read on it is running: a release during a read cancels
 *     immediately (so the read can return) and the close happens when both
 *     the release and the read are done.
 *   - Granting consent for a different source, or revoking consent, ends a
 *     live session with consent_revoked.
 *   - gc_audio_shutdown() refuses every later call and returns only after
 *     every call already inside this object has left, so no seam is invoked
 *     after it returns and the caller may free the seams.
 *   - Preference writes go through gc_audio_settings (checked, generation-
 *     fenced, read back) and report accepted / persisted / readback_verified
 *     / durable separately; `applied` is never set by a write.
 *
 * This is the contract only (AUDIO-01). AVDTP/SBC/RTP (AUDIO-02/03), AVRCP
 * and battery (AUDIO-04), IPC/UI (AUDIO-06) and any real provider or radio
 * (AUDIO-07) plug into it later.
 */
#ifndef GC_AUDIO_H
#define GC_AUDIO_H

#include "gc_audio_settings.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GC_AUDIO_OK = 0,
    GC_AUDIO_INVALID_ARGUMENT,
    GC_AUDIO_INVALID_IDENTITY,
    GC_AUDIO_STALE_GENERATION,
    GC_AUDIO_CONSENT_REQUIRED,
    GC_AUDIO_CONSENT_REVOKED,
    GC_AUDIO_SOURCE_UNAVAILABLE,
    GC_AUDIO_UNSUPPORTED_FORMAT,
    GC_AUDIO_RADIO_UNAVAILABLE,
    GC_AUDIO_RADIO_OWNED_ELSEWHERE,
    GC_AUDIO_PROFILE_UNAVAILABLE,
    GC_AUDIO_NEGOTIATION_REJECTED,
    GC_AUDIO_TRANSPORT_LOST,
    GC_AUDIO_QUEUE_FULL,
    GC_AUDIO_PERSISTENCE_FAILED,
    GC_AUDIO_READBACK_FAILED,
    GC_AUDIO_BUSY,
    GC_AUDIO_ENCODER_UNAVAILABLE,
    GC_AUDIO_ERROR_COUNT
} GcAudioError;

/* Stable wire names ("invalid_argument", ...) for IPC/logs. */
const char *gc_audio_error_name(GcAudioError error);

typedef enum {
    GC_AUDIO_SCOPE_NONE = 0,               /* no source described / no session yet */
    GC_AUDIO_SCOPE_TEST_TONE = 1,          /* must be labelled as a tone */
    GC_AUDIO_SCOPE_APP_OWNED = 2,          /* this app's own PCM */
    GC_AUDIO_SCOPE_AUTHORIZED_EXTERNAL = 3 /* needs an independently authorized provider */
} GcAudioSourceScope;

#define GC_AUDIO_SOURCE_ID_MAX 32
#define GC_AUDIO_REASON_MAX 96

typedef struct {
    char id[GC_AUDIO_SOURCE_ID_MAX];       /* [a-z0-9_-], 1..31 chars */
    GcAudioSourceScope scope;              /* TEST_TONE..AUTHORIZED_EXTERNAL; NONE is invalid here */
    uint32_t sample_rate;                  /* 44100 or 48000 */
    uint8_t channels;                      /* 1 or 2 */
    uint8_t available;
    uint8_t consent_required;
    char unavailable_reason[GC_AUDIO_REASON_MAX];
} GcAudioSourceInfo;

/* PCM source seam.
 *   read    fills up to max_frames interleaved s16 frames and the index of
 *           the first returned frame; returns the frame count, 0 for none
 *           yet, or -1 when the source ended. Any other negative value or a
 *           count above max_frames is malformed. It runs WITHOUT this
 *           object's lock, one read at a time.
 *   cancel  may run concurrently with a read on the same handle and must
 *           make that read return promptly.
 *   close   is never called while a read on that handle is running, and
 *           always after cancel.
 *   describe may run concurrently with read.
 *   open    returns 0 and a non-NULL handle distinct from every handle of
 *           this provider that has not been closed yet; on failure it must
 *           not hand out a handle (one returned with a failure is neither
 *           kept nor closed by this module). A reissued still-open handle is
 *           detected where this module can see it (a close owed after a
 *           running read, or the live handle) and refused as
 *           source_unavailable without touching it.
 *   The source's `consent_required` flag is validated but not relied on:
 *   this module always requires explicit consent for every source.
 * Every callback must be bounded. A callback may call the non-lifecycle
 * entry points (stop, revoke, on_transport, ...) but never
 * gc_audio_shutdown()/gc_audio_destroy(), which wait for it. */
typedef struct {
    void *ctx;
    int (*describe)(void *ctx, GcAudioSourceInfo *out);
    int (*open)(void *ctx, uint64_t consent_generation, void **handle);
    long (*read)(void *ctx, void *handle, int16_t *frames, size_t max_frames, uint64_t *first_sample);
    void (*cancel)(void *ctx, void *handle);
    void (*close)(void *ctx, void *handle);
} GcAudioProvider;

typedef enum {
    GC_AUDIO_RADIO_UNAVAILABLE_STATE = 0,
    GC_AUDIO_RADIO_OWNED_ELSEWHERE_STATE = 1,
    GC_AUDIO_RADIO_AUTHORIZED = 2
} GcAudioRadioState;

/* Profile-channel seam on the single authorized Bluetooth owner. open_stream
 * is asynchronous: progress arrives via gc_audio_on_transport() carrying the
 * same session generation (it may also be delivered synchronously from
 * inside open_stream). close_stream(session) is called exactly once for
 * every session open_stream was called for -- including one whose
 * open_stream returned failure -- and only after open_stream returned. */
typedef struct {
    void *ctx;
    GcAudioRadioState (*radio)(void *ctx);
    int (*open_stream)(void *ctx, const char *address, uint64_t session,
                       uint32_t sample_rate, uint8_t channels);
    void (*close_stream)(void *ctx, uint64_t session);
} GcAudioTransport;

typedef struct {
    void *ctx;
    int (*available)(void *ctx, uint32_t sample_rate, uint8_t channels);
} GcAudioEncoder;

typedef struct {
    uint8_t ordinary_host_implemented;
    uint8_t service_wired;
    uint8_t provider_available;
    uint8_t transport_authorized;
    uint8_t encoder_available;
    uint8_t target_verified;
    uint8_t microphone;       /* always 0: needs a separate voice provider/SCO */
    uint8_t le_audio;         /* always 0: LE HID is not LE Audio/LC3 */
    uint8_t game_capture;     /* always 0: no authorized system/game provider */
    uint8_t source_valid;     /* the provider described itself well-formed */
    char source_id[GC_AUDIO_SOURCE_ID_MAX];  /* when source_valid */
    GcAudioSourceScope source_scope;         /* NONE unless source_valid */
    /* What START for the provider's own source with the current consent
     * generation would return right now, in START's own order: shut down
     * (profile_unavailable), busy, consent_required, then the seams. */
    GcAudioError blocking;
    /* The first missing/refusing seam only (source, format, radio, encoder);
     * OK when every seam is ready. Consent and busy never appear here. */
    GcAudioError seam_blocking;
    char unavailable_reason[GC_AUDIO_REASON_MAX]; /* explains `blocking` */
    char seam_reason[GC_AUDIO_REASON_MAX];        /* explains `seam_blocking` */
} GcAudioCapabilities;

typedef enum {
    GC_AUDIO_STATE_UNAVAILABLE = 0, /* shut down, or idle with a seam missing/refusing */
    GC_AUDIO_STATE_IDLE,
    GC_AUDIO_STATE_PENDING,         /* START accepted locally; transport negotiating */
    GC_AUDIO_STATE_STREAMING,       /* transport reported START accepted */
    GC_AUDIO_STATE_SUSPENDED
} GcAudioProfileState;

typedef enum {
    GC_AUDIO_EVENT_START_ACCEPTED = 0,
    GC_AUDIO_EVENT_START_REJECTED,
    GC_AUDIO_EVENT_SUSPENDED,
    GC_AUDIO_EVENT_RESUMED,
    GC_AUDIO_EVENT_LOST
} GcAudioTransportEvent;

typedef struct {
    GcAudioProfileState state;
    uint64_t session;              /* bumps on every start and every release */
    uint64_t consent_generation;
    uint8_t consent_granted;
    uint8_t shut_down;
    char consent_source[GC_AUDIO_SOURCE_ID_MAX];
    char headset[GC_AUDIO_ADDRESS_TEXT];   /* last session's, empty before the first */
    char source[GC_AUDIO_SOURCE_ID_MAX];
    GcAudioSourceScope source_scope;       /* NONE before the first session */
    uint32_t sample_rate;
    uint8_t channels;
    GcAudioError last_error;               /* why the last session ended (OK = user stop) */
    uint64_t frames_read;
    uint64_t stale_events;         /* transport events not for the live, opened session */
    uint64_t releases;             /* provider handles released (cancel+close owed once) */
    uint64_t rejected_reads;       /* malformed provider output refused */
    uint64_t discarded_reads;      /* reads completed after a suspend/stop; frames dropped */
    uint64_t rejected_opens;       /* provider reissued a handle that was still open */
} GcAudioSnapshot;

typedef struct {
    int accepted;
    int persisted;
    int readback_verified;
    int durable;                   /* file and directory fsync both succeeded */
    int applied;                   /* never set by a preference write */
    uint64_t generation;
    GcAudioError error;
    char detail[160];
} GcAudioPrefsResult;

/* One per start() between creating its session and returning: records why
 * that session ended if a release overtakes it. */
struct GcAudioOpener {
    uint64_t session;
    int ended;
    GcAudioError reason;
    struct GcAudioOpener *next;
};

/* Private state; read only through gc_audio_snapshot()/capabilities(). */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t quiet;          /* signalled when in_flight drops to 0 after shutdown */
    GcAudioProvider provider;
    GcAudioTransport transport;
    GcAudioEncoder encoder;
    int has_provider, has_transport, has_encoder;
    char settings_root[256];
    GcAudioSnapshot snap;
    void *handle;                  /* provider handle attached to the live session */
    uint64_t transport_session;    /* live session open_stream was issued for */
    int transport_phase;           /* none / opening (start owns the close) / open */
    uint64_t next_sample;
    void *reading;                 /* the handle a pump is inside provider.read on, or NULL */
    void *shared_close;            /* handle released mid-read; closed by the last of release/pump */
    int shared_refs;
    struct GcAudioOpener *openers; /* starts still opening their session (on their stacks) */
    unsigned in_flight;            /* public calls currently inside this object */
    int shut_down;
} GcAudio;

/* NULL seams are allowed and reported as unavailable. `settings_root` is the
 * private directory for per-headset settings: NULL means preferences report
 * persistence_failed; a non-NULL root must be absolute and shorter than 256
 * bytes or init fails (-1) and there is nothing to destroy. */
int gc_audio_init(GcAudio *audio, const GcAudioProvider *provider, const GcAudioTransport *transport,
                  const GcAudioEncoder *encoder, const char *settings_root);
/* Ends any live session exactly once, refuses every later call
 * (profile_unavailable, no seam calls) and waits until every call already
 * inside the object has returned. Never call it from a seam callback. */
void gc_audio_shutdown(GcAudio *audio);
/* Shutdown, then frees the lock. No call may run concurrently with it. */
void gc_audio_destroy(GcAudio *audio);

void gc_audio_capabilities(GcAudio *audio, GcAudioCapabilities *out);
void gc_audio_snapshot(GcAudio *audio, GcAudioSnapshot *out);

/* Explicit user consent for one source id. Grant and revoke both bump the
 * consent generation. Revoking, or granting a different source than the live
 * session's, ends that session with consent_revoked. Re-granting the same
 * source leaves a live session running under its original lease. */
GcAudioError gc_audio_grant_consent(GcAudio *audio, const char *source_id, uint64_t *generation);
GcAudioError gc_audio_revoke_consent(GcAudio *audio);

GcAudioError gc_audio_start(GcAudio *audio, const char *headset, const char *source_id,
                            uint64_t consent_generation);
GcAudioError gc_audio_suspend(GcAudio *audio);
GcAudioError gc_audio_stop(GcAudio *audio);

/* Transport progress for `session`. Returns GC_AUDIO_STALE_GENERATION (and
 * counts it) when the session is not the live one, open_stream has not been
 * issued for it yet, or the event is unknown. */
GcAudioError gc_audio_on_transport(GcAudio *audio, uint64_t session, GcAudioTransportEvent event);

/* Pulls PCM from the provider only while STREAMING, one reader at a time (a
 * concurrent pump returns 0). `frames` must hold max_frames * channels
 * samples. Validates the provider's frame count and that sample indices
 * never go backwards or wrap (a forward gap is the provider's dropped
 * audio); malformed output or end of source ends the session. A read that
 * completes after a suspend or release is discarded. Returns frames read, 0
 * when not streaming or nothing ready. */
long gc_audio_pump(GcAudio *audio, int16_t *frames, size_t max_frames);

GcAudioPrefsResult gc_audio_set_preferences(GcAudio *audio, const char *headset,
                                            uint64_t expected_generation,
                                            const GcAudioHeadsetSettings *settings);

#ifdef __cplusplus
}
#endif

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-01 ordinary headset-audio contract. See gc_audio.h.
 *
 * Locking model. One mutex guards every field. Seam callbacks always run
 * with the mutex released. Ownership of the two external resources -- the
 * provider handle and the owed close_stream -- moves only under the mutex:
 *
 *   - detach_locked() ends the live session (bumps the session, records the
 *     reason) and takes whatever that session owns; finish() then calls the
 *     seams outside the lock. Callers that end a session for a specific
 *     reason (transport event, malformed read, start failure) check under
 *     the same lock that the session is still theirs, so a late report for
 *     an old session can never end a newer one.
 *   - While open_stream is in flight the session is OPENING: a release does
 *     not close the stream; start() closes it after open_stream returns if
 *     its session is gone by then. Once it returns the phase is OPEN and the
 *     release owns the close.
 *   - pump() reads without the lock, recording the handle in `reading`. A
 *     release of THAT handle during the read cancels at once and parks it in
 *     `shared_close` with two references (release, read); whichever finishes
 *     last closes it, so close always follows both cancel and the read. A
 *     newer handle released meanwhile is not being read and closes at once.
 *     No new read starts while a parked close is owed.
 *   - Every public call except init/shutdown/destroy registers in
 *     `in_flight`; shutdown flips `shut_down` (refusing new calls), ends the
 *     live session and waits for in_flight to reach zero.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* strnlen */
#endif
#include "gc_audio.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { PHASE_NONE = 0, PHASE_OPENING = 1, PHASE_OPEN = 2 };

static const char *const error_names[GC_AUDIO_ERROR_COUNT] = {
    "ok", "invalid_argument", "invalid_identity", "stale_generation", "consent_required",
    "consent_revoked", "source_unavailable", "unsupported_format", "radio_unavailable",
    "radio_owned_elsewhere", "profile_unavailable", "negotiation_rejected", "transport_lost",
    "queue_full", "persistence_failed", "readback_failed", "busy", "encoder_unavailable",
};

const char *gc_audio_error_name(GcAudioError error)
{
    return (unsigned)error < GC_AUDIO_ERROR_COUNT ? error_names[error] : "invalid_argument";
}

static int valid_source_id(const char *id)
{
    if (!id) return 0;
    const size_t n = strnlen(id, GC_AUDIO_SOURCE_ID_MAX);
    if (n == 0 || n >= GC_AUDIO_SOURCE_ID_MAX) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Untrusted provider text: bounded, terminated, printable ASCII only. */
static int printable_bounded(const char *text, size_t cap)
{
    const char *end = memchr(text, '\0', cap);
    if (!end) return 0;
    for (const char *p = text; p < end; ++p)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) return 0;
    return 1;
}

static int supported_format(uint32_t rate, uint8_t channels)
{
    return (rate == 44100 || rate == 48000) && (channels == 1 || channels == 2);
}

static int live_state(GcAudioProfileState state)
{
    return state == GC_AUDIO_STATE_PENDING || state == GC_AUDIO_STATE_STREAMING ||
           state == GC_AUDIO_STATE_SUSPENDED;
}

int gc_audio_init(GcAudio *audio, const GcAudioProvider *provider, const GcAudioTransport *transport,
                  const GcAudioEncoder *encoder, const char *settings_root)
{
    if (!audio) return -1;
    memset(audio, 0, sizeof(*audio));
    if (settings_root &&
        (settings_root[0] != '/' || strnlen(settings_root, sizeof(audio->settings_root)) >= sizeof(audio->settings_root)))
        return -1; /* a configured root that cannot be used is an error, not "no root" */
    if (pthread_mutex_init(&audio->mutex, NULL)) return -1;
    if (pthread_cond_init(&audio->quiet, NULL)) {
        pthread_mutex_destroy(&audio->mutex);
        return -1;
    }
    if (provider && provider->describe && provider->open && provider->read && provider->cancel &&
        provider->close) {
        audio->provider = *provider;
        audio->has_provider = 1;
    }
    if (transport && transport->radio && transport->open_stream && transport->close_stream) {
        audio->transport = *transport;
        audio->has_transport = 1;
    }
    if (encoder && encoder->available) {
        audio->encoder = *encoder;
        audio->has_encoder = 1;
    }
    if (settings_root) snprintf(audio->settings_root, sizeof(audio->settings_root), "%s", settings_root);
    audio->snap.state = GC_AUDIO_STATE_IDLE;
    return 0;
}

/* ---- call registration ---- */

static int enter(GcAudio *audio)
{
    pthread_mutex_lock(&audio->mutex);
    const int ok = !audio->shut_down;
    if (ok) ++audio->in_flight;
    pthread_mutex_unlock(&audio->mutex);
    return ok;
}

static void leave(GcAudio *audio)
{
    pthread_mutex_lock(&audio->mutex);
    if (--audio->in_flight == 0 && audio->shut_down) pthread_cond_broadcast(&audio->quiet);
    pthread_mutex_unlock(&audio->mutex);
}

/* ---- the single release path ---- */

typedef struct {
    void *handle;       /* cancel (and close unless shared) */
    int shared;         /* a read is running on it: close by the last of release/read */
    uint64_t stream;    /* close_stream owed */
} Detached;

/* Ends the live session, if any, and takes what it owns. Mutex held. */
static Detached detach_locked(GcAudio *audio, GcAudioError reason)
{
    Detached d = {0};
    if (!live_state(audio->snap.state)) return d;
    d.handle = audio->handle;
    audio->handle = NULL;
    if (d.handle) {
        ++audio->snap.releases;
        if (audio->reading == d.handle) { /* not merely "some" read: this handle's */
            d.shared = 1;
            audio->shared_close = d.handle;
            audio->shared_refs = 2;
        }
    }
    if (audio->transport_phase == PHASE_OPEN) d.stream = audio->transport_session;
    /* PHASE_OPENING: open_stream is still running; start() owns that close. */
    audio->transport_phase = PHASE_NONE;
    audio->transport_session = 0;
    for (struct GcAudioOpener *o = audio->openers; o; o = o->next)
        if (o->session == audio->snap.session && !o->ended) {
            o->ended = 1; /* this start's own record: later releases cannot overwrite it */
            o->reason = reason;
        }
    ++audio->snap.session; /* invalidates every late callback for the old one */
    audio->snap.state = GC_AUDIO_STATE_IDLE;
    audio->snap.last_error = reason;
    return d;
}

/* Drops one reference on a handle released mid-read; returns 1 if the
 * caller must close it now. Mutex held. */
static int drop_shared_locked(GcAudio *audio, void *handle)
{
    if (audio->shared_close != handle || audio->shared_refs <= 0) return 0;
    if (--audio->shared_refs) return 0;
    audio->shared_close = NULL;
    return 1;
}

static void finish(GcAudio *audio, Detached d)
{
    if (d.handle) {
        audio->provider.cancel(audio->provider.ctx, d.handle);
        int close_now = 1;
        if (d.shared) {
            pthread_mutex_lock(&audio->mutex);
            close_now = drop_shared_locked(audio, d.handle);
            pthread_mutex_unlock(&audio->mutex);
        }
        if (close_now) audio->provider.close(audio->provider.ctx, d.handle);
    }
    if (d.stream) audio->transport.close_stream(audio->transport.ctx, d.stream);
}

/* Why the session a start() was opening ended, from that start's own record. */
static GcAudioError overtaken_reason(const struct GcAudioOpener *opener)
{
    if (opener->ended && opener->reason != GC_AUDIO_OK) return opener->reason;
    return GC_AUDIO_PROFILE_UNAVAILABLE; /* user stop / shutdown: nothing is streaming */
}

static void unregister_opener_locked(GcAudio *audio, struct GcAudioOpener *opener)
{
    for (struct GcAudioOpener **p = &audio->openers; *p; p = &(*p)->next)
        if (*p == opener) {
            *p = opener->next;
            return;
        }
}

void gc_audio_shutdown(GcAudio *audio)
{
    if (!audio) return;
    pthread_mutex_lock(&audio->mutex);
    audio->shut_down = 1;
    audio->snap.shut_down = 1;
    const Detached d = detach_locked(audio, GC_AUDIO_PROFILE_UNAVAILABLE);
    pthread_mutex_unlock(&audio->mutex);
    finish(audio, d);
    pthread_mutex_lock(&audio->mutex);
    while (audio->in_flight) pthread_cond_wait(&audio->quiet, &audio->mutex);
    pthread_mutex_unlock(&audio->mutex);
}

void gc_audio_destroy(GcAudio *audio)
{
    if (!audio) return;
    gc_audio_shutdown(audio);
    pthread_cond_destroy(&audio->quiet);
    pthread_mutex_destroy(&audio->mutex);
}

/* ---- queries ---- */

/* Reads the provider's self-description and rejects malformed answers. */
static int describe_source(GcAudio *audio, GcAudioSourceInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (!audio->has_provider || audio->provider.describe(audio->provider.ctx, info)) return 0;
    if (!printable_bounded(info->id, sizeof(info->id)) ||
        !printable_bounded(info->unavailable_reason, sizeof(info->unavailable_reason)) ||
        !valid_source_id(info->id) || info->scope < GC_AUDIO_SCOPE_TEST_TONE ||
        info->scope > GC_AUDIO_SCOPE_AUTHORIZED_EXTERNAL || info->available > 1 || info->consent_required > 1)
        return 0;
    return 1;
}

/* Any value other than AUTHORIZED / OWNED_ELSEWHERE is treated as unavailable
 * by every comparison below. */
static GcAudioRadioState query_radio(GcAudio *audio)
{
    return audio->has_transport ? audio->transport.radio(audio->transport.ctx) : GC_AUDIO_RADIO_UNAVAILABLE_STATE;
}

void gc_audio_capabilities(GcAudio *audio, GcAudioCapabilities *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!audio) return;
    out->ordinary_host_implemented = 1; /* this contract module, nothing more */
    if (!enter(audio)) {
        out->blocking = out->seam_blocking = GC_AUDIO_PROFILE_UNAVAILABLE;
        snprintf(out->unavailable_reason, sizeof(out->unavailable_reason), "audio is shut down");
        snprintf(out->seam_reason, sizeof(out->seam_reason), "audio is shut down");
        return;
    }
    GcAudioSourceInfo info;
    const int described = describe_source(audio, &info);
    if (described) {
        out->source_valid = 1;
        snprintf(out->source_id, sizeof(out->source_id), "%s", info.id);
        out->source_scope = info.scope;
    }
    out->provider_available = described && info.available;
    const GcAudioRadioState radio = query_radio(audio);
    out->transport_authorized = radio == GC_AUDIO_RADIO_AUTHORIZED;
    const int format_ok = described && supported_format(info.sample_rate, info.channels);
    out->encoder_available = audio->has_encoder && format_ok &&
                             audio->encoder.available(audio->encoder.ctx, info.sample_rate, info.channels) ? 1 : 0;

    char *const seam_reason = out->seam_reason;
    const size_t reason_cap = sizeof(out->seam_reason);
    GcAudioError seam = GC_AUDIO_OK;
    if (!audio->has_provider) {
        seam = GC_AUDIO_SOURCE_UNAVAILABLE;
        snprintf(seam_reason, reason_cap, "no PCM source provider");
    } else if (!described) {
        seam = GC_AUDIO_SOURCE_UNAVAILABLE;
        snprintf(seam_reason, reason_cap, "PCM source description invalid");
    } else if (!info.available) {
        seam = GC_AUDIO_SOURCE_UNAVAILABLE;
        snprintf(seam_reason, reason_cap, "%s",
                 info.unavailable_reason[0] ? info.unavailable_reason : "PCM source unavailable");
    } else if (!format_ok) {
        seam = GC_AUDIO_UNSUPPORTED_FORMAT;
        snprintf(seam_reason, reason_cap, "unsupported PCM format");
    } else if (radio == GC_AUDIO_RADIO_OWNED_ELSEWHERE_STATE) {
        seam = GC_AUDIO_RADIO_OWNED_ELSEWHERE;
        snprintf(seam_reason, reason_cap, "Bluetooth radio owned elsewhere");
    } else if (radio != GC_AUDIO_RADIO_AUTHORIZED) {
        seam = GC_AUDIO_RADIO_UNAVAILABLE;
        snprintf(seam_reason, reason_cap, "no authorized Bluetooth radio");
    } else if (!out->encoder_available) {
        seam = GC_AUDIO_ENCODER_UNAVAILABLE;
        snprintf(seam_reason, reason_cap, "no encoder for this format");
    }
    out->seam_blocking = seam;

    pthread_mutex_lock(&audio->mutex);
    const int live = live_state(audio->snap.state);
    const int granted = audio->snap.consent_granted;
    const int other_source = described && strcmp(audio->snap.consent_source, info.id) != 0;
    pthread_mutex_unlock(&audio->mutex);
    if (live) {
        out->blocking = GC_AUDIO_BUSY;
        snprintf(out->unavailable_reason, sizeof(out->unavailable_reason), "an audio session is live");
    } else if (!granted || other_source) {
        out->blocking = GC_AUDIO_CONSENT_REQUIRED;
        snprintf(out->unavailable_reason, sizeof(out->unavailable_reason), "%s",
                 granted ? "consent was granted for another source" : "no audio consent granted");
    } else {
        out->blocking = seam;
        snprintf(out->unavailable_reason, sizeof(out->unavailable_reason), "%s", seam_reason);
    }
    leave(audio);
}

void gc_audio_snapshot(GcAudio *audio, GcAudioSnapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!audio) return;
    GcAudioCapabilities caps;
    gc_audio_capabilities(audio, &caps);
    pthread_mutex_lock(&audio->mutex);
    *out = audio->snap;
    pthread_mutex_unlock(&audio->mutex);
    if (out->state == GC_AUDIO_STATE_IDLE && (out->shut_down || caps.seam_blocking != GC_AUDIO_OK))
        out->state = GC_AUDIO_STATE_UNAVAILABLE;
}

/* ---- consent ---- */

GcAudioError gc_audio_grant_consent(GcAudio *audio, const char *source_id, uint64_t *generation)
{
    if (!audio || !valid_source_id(source_id)) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(audio)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&audio->mutex);
    ++audio->snap.consent_generation;
    audio->snap.consent_granted = 1;
    snprintf(audio->snap.consent_source, sizeof(audio->snap.consent_source), "%s", source_id);
    Detached d = {0};
    /* Consent moved to another source: the live one no longer has it. */
    if (live_state(audio->snap.state) && strcmp(audio->snap.source, source_id))
        d = detach_locked(audio, GC_AUDIO_CONSENT_REVOKED);
    if (generation) *generation = audio->snap.consent_generation;
    pthread_mutex_unlock(&audio->mutex);
    finish(audio, d);
    leave(audio);
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_revoke_consent(GcAudio *audio)
{
    if (!audio) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(audio)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&audio->mutex);
    ++audio->snap.consent_generation;
    audio->snap.consent_granted = 0;
    audio->snap.consent_source[0] = '\0';
    const Detached d = detach_locked(audio, GC_AUDIO_CONSENT_REVOKED);
    pthread_mutex_unlock(&audio->mutex);
    finish(audio, d);
    leave(audio);
    return GC_AUDIO_OK;
}

/* ---- session lifecycle ---- */

/* The checks START can make without asking any seam. Mutex held. */
static GcAudioError local_refusal_locked(const GcAudio *audio, const char *source_id, uint64_t consent_generation)
{
    if (audio->shut_down) return GC_AUDIO_PROFILE_UNAVAILABLE;
    if (live_state(audio->snap.state)) return GC_AUDIO_BUSY;
    if (!audio->snap.consent_granted || strcmp(audio->snap.consent_source, source_id))
        return GC_AUDIO_CONSENT_REQUIRED;
    if (consent_generation != audio->snap.consent_generation) return GC_AUDIO_STALE_GENERATION;
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_start(GcAudio *audio, const char *headset, const char *source_id,
                            uint64_t consent_generation)
{
    if (!audio) return GC_AUDIO_INVALID_ARGUMENT;
    char address[GC_AUDIO_ADDRESS_TEXT];
    if (gc_audio_address_canonical(headset, address)) return GC_AUDIO_INVALID_IDENTITY;
    if (!valid_source_id(source_id)) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(audio)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    struct GcAudioOpener opener = {0}; /* registered while this start owns a session */

    /* 1. Local refusals first: no seam is asked anything. */
    pthread_mutex_lock(&audio->mutex);
    GcAudioError result = local_refusal_locked(audio, source_id, consent_generation);
    pthread_mutex_unlock(&audio->mutex);
    if (result != GC_AUDIO_OK) goto out;

    /* 2. Seam readiness (queries only). */
    GcAudioSourceInfo info;
    GcAudioRadioState radio;
    if (!describe_source(audio, &info) || strcmp(info.id, source_id) || !info.available)
        result = GC_AUDIO_SOURCE_UNAVAILABLE;
    else if (!supported_format(info.sample_rate, info.channels))
        result = GC_AUDIO_UNSUPPORTED_FORMAT;
    else if ((radio = query_radio(audio)) == GC_AUDIO_RADIO_OWNED_ELSEWHERE_STATE)
        result = GC_AUDIO_RADIO_OWNED_ELSEWHERE;
    else if (radio != GC_AUDIO_RADIO_AUTHORIZED)
        result = GC_AUDIO_RADIO_UNAVAILABLE;
    else if (!audio->has_encoder || !audio->encoder.available(audio->encoder.ctx, info.sample_rate, info.channels))
        result = GC_AUDIO_ENCODER_UNAVAILABLE;
    if (result != GC_AUDIO_OK) goto out;

    /* 3. Re-check what may have changed while the seams were asked, then
     *    create the session. */
    pthread_mutex_lock(&audio->mutex);
    result = local_refusal_locked(audio, source_id, consent_generation);
    if (result != GC_AUDIO_OK) {
        pthread_mutex_unlock(&audio->mutex);
        goto out;
    }
    const uint64_t session = ++audio->snap.session;
    opener.session = session;
    opener.next = audio->openers;
    audio->openers = &opener;
    audio->snap.state = GC_AUDIO_STATE_PENDING;
    audio->snap.last_error = GC_AUDIO_OK;
    snprintf(audio->snap.headset, sizeof(audio->snap.headset), "%s", address);
    snprintf(audio->snap.source, sizeof(audio->snap.source), "%s", source_id);
    audio->snap.source_scope = info.scope;
    audio->snap.sample_rate = info.sample_rate;
    audio->snap.channels = info.channels;
    audio->handle = NULL;
    audio->transport_phase = PHASE_NONE;
    audio->transport_session = 0;
    pthread_mutex_unlock(&audio->mutex);

    /* 4. Open the source under the consent lease. */
    void *handle = NULL;
    const int open_failed = audio->provider.open(audio->provider.ctx, consent_generation, &handle) || !handle;
    pthread_mutex_lock(&audio->mutex);
    const int ours = audio->snap.session == session;
    if (open_failed) {
        Detached d = {0};
        if (ours) d = detach_locked(audio, GC_AUDIO_SOURCE_UNAVAILABLE);
        pthread_mutex_unlock(&audio->mutex);
        finish(audio, d); /* owns nothing yet: no handle, no stream */
        /* A handle returned alongside a failure is not ours to keep or close. */
        result = GC_AUDIO_SOURCE_UNAVAILABLE;
        goto out;
    }
    if (handle == audio->shared_close || handle == audio->handle) {
        /* Provider fault: it reissued a handle that is still open (one whose
         * close is owed after a running read, or a newer session's). Using or
         * closing it would close it under its owner; touch nothing. */
        ++audio->snap.rejected_opens;
        Detached d = {0};
        if (ours) d = detach_locked(audio, GC_AUDIO_SOURCE_UNAVAILABLE);
        pthread_mutex_unlock(&audio->mutex);
        finish(audio, d); /* owns nothing yet */
        result = GC_AUDIO_SOURCE_UNAVAILABLE;
        goto out;
    }
    if (!ours) {
        /* Overtaken while opening: the handle was never attached; close it here. */
        ++audio->snap.releases;
        result = overtaken_reason(&opener);
        pthread_mutex_unlock(&audio->mutex);
        audio->provider.cancel(audio->provider.ctx, handle);
        audio->provider.close(audio->provider.ctx, handle);
        goto out;
    }
    audio->handle = handle;
    audio->next_sample = 0;
    audio->transport_session = session;
    audio->transport_phase = PHASE_OPENING; /* events for this session are valid from here */
    pthread_mutex_unlock(&audio->mutex);

    /* 5. Ask the transport. Progress may arrive before this returns. */
    const int stream_failed =
        audio->transport.open_stream(audio->transport.ctx, address, session, info.sample_rate, info.channels) != 0;
    pthread_mutex_lock(&audio->mutex);
    if (audio->snap.session == session) {
        audio->transport_phase = PHASE_OPEN; /* the release now owns close_stream */
        Detached d = {0};
        if (stream_failed) d = detach_locked(audio, GC_AUDIO_PROFILE_UNAVAILABLE);
        pthread_mutex_unlock(&audio->mutex);
        finish(audio, d);
        result = stream_failed ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_OK;
        goto out;
    }
    /* Released while open_stream ran: that release left the close to us. */
    result = overtaken_reason(&opener);
    pthread_mutex_unlock(&audio->mutex);
    audio->transport.close_stream(audio->transport.ctx, session);

out:
    if (opener.session) {
        pthread_mutex_lock(&audio->mutex);
        unregister_opener_locked(audio, &opener);
        pthread_mutex_unlock(&audio->mutex);
    }
    leave(audio);
    return result; /* OK = pending: streaming only after the transport accepts START */
}

GcAudioError gc_audio_on_transport(GcAudio *audio, uint64_t session, GcAudioTransportEvent event)
{
    if (!audio) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(audio)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&audio->mutex);
    const GcAudioProfileState state = audio->snap.state;
    /* transport_session always equals the live session once the phase is
     * not NONE, so the session and phase checks together cover it. */
    if (session == 0 || session != audio->snap.session || !live_state(state) ||
        audio->transport_phase == PHASE_NONE || (unsigned)event > GC_AUDIO_EVENT_LOST) {
        ++audio->snap.stale_events;
        pthread_mutex_unlock(&audio->mutex);
        leave(audio);
        return GC_AUDIO_STALE_GENERATION;
    }
    Detached d = {0};
    switch (event) {
    case GC_AUDIO_EVENT_START_ACCEPTED:
        if (state == GC_AUDIO_STATE_PENDING) audio->snap.state = GC_AUDIO_STATE_STREAMING;
        break;
    case GC_AUDIO_EVENT_RESUMED:
        if (state == GC_AUDIO_STATE_SUSPENDED) audio->snap.state = GC_AUDIO_STATE_STREAMING;
        break;
    case GC_AUDIO_EVENT_SUSPENDED:
        if (state == GC_AUDIO_STATE_STREAMING) audio->snap.state = GC_AUDIO_STATE_SUSPENDED;
        break;
    case GC_AUDIO_EVENT_START_REJECTED:
        d = detach_locked(audio, GC_AUDIO_NEGOTIATION_REJECTED);
        break;
    case GC_AUDIO_EVENT_LOST:
        d = detach_locked(audio, GC_AUDIO_TRANSPORT_LOST);
        break;
    }
    pthread_mutex_unlock(&audio->mutex);
    finish(audio, d);
    leave(audio);
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_suspend(GcAudio *audio)
{
    if (!audio) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(audio)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&audio->mutex);
    GcAudioError result = GC_AUDIO_PROFILE_UNAVAILABLE;
    if (audio->snap.state == GC_AUDIO_STATE_STREAMING) {
        audio->snap.state = GC_AUDIO_STATE_SUSPENDED;
        result = GC_AUDIO_OK;
    }
    pthread_mutex_unlock(&audio->mutex);
    leave(audio);
    return result;
}

GcAudioError gc_audio_stop(GcAudio *audio)
{
    if (!audio) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(audio)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&audio->mutex);
    const Detached d = detach_locked(audio, GC_AUDIO_OK);
    pthread_mutex_unlock(&audio->mutex);
    finish(audio, d);
    leave(audio);
    return GC_AUDIO_OK;
}

long gc_audio_pump(GcAudio *audio, int16_t *frames, size_t max_frames)
{
    if (!audio || !frames || !max_frames) return 0;
    if (max_frames > (size_t)(LONG_MAX / 2)) max_frames = (size_t)(LONG_MAX / 2);
    if (!enter(audio)) return 0;
    pthread_mutex_lock(&audio->mutex);
    /* One reader; none while an earlier mid-read release still owes a close. */
    if (audio->snap.state != GC_AUDIO_STATE_STREAMING || !audio->handle || audio->reading ||
        audio->shared_close) {
        pthread_mutex_unlock(&audio->mutex);
        leave(audio);
        return 0;
    }
    void *const handle = audio->handle;
    const uint64_t session = audio->snap.session;
    audio->reading = handle;
    pthread_mutex_unlock(&audio->mutex);

    uint64_t first = 0;
    const long n = audio->provider.read(audio->provider.ctx, handle, frames, max_frames, &first);

    long got = 0;
    int close_now = 0;
    Detached d = {0};
    pthread_mutex_lock(&audio->mutex);
    audio->reading = NULL;
    if (audio->snap.session != session || audio->handle != handle) {
        /* Released during the read: the handle was cancelled; drop our
         * reference and close it if the release already finished. */
        ++audio->snap.discarded_reads;
        close_now = drop_shared_locked(audio, handle);
    } else if (n == -1) {
        d = detach_locked(audio, GC_AUDIO_SOURCE_UNAVAILABLE); /* end of source */
    } else if (n < -1 || (size_t)n > max_frames ||
               (n > 0 && (first < audio->next_sample || first > UINT64_MAX - (uint64_t)n))) {
        ++audio->snap.rejected_reads;
        d = detach_locked(audio, GC_AUDIO_SOURCE_UNAVAILABLE);
    } else if (n > 0) {
        audio->next_sample = first + (uint64_t)n;
        if (audio->snap.state == GC_AUDIO_STATE_STREAMING) {
            audio->snap.frames_read += (uint64_t)n;
            got = n;
        } else {
            ++audio->snap.discarded_reads; /* suspended while reading */
        }
    }
    pthread_mutex_unlock(&audio->mutex);
    if (close_now) audio->provider.close(audio->provider.ctx, handle);
    finish(audio, d);
    leave(audio);
    return got;
}

GcAudioPrefsResult gc_audio_set_preferences(GcAudio *audio, const char *headset,
                                            uint64_t expected_generation,
                                            const GcAudioHeadsetSettings *settings)
{
    GcAudioPrefsResult result;
    memset(&result, 0, sizeof(result));
    char address[GC_AUDIO_ADDRESS_TEXT];
    if (!audio || !settings || gc_audio_settings_valid(settings)) {
        result.error = GC_AUDIO_INVALID_ARGUMENT;
        return result;
    }
    if (gc_audio_address_canonical(headset, address)) {
        result.error = GC_AUDIO_INVALID_IDENTITY;
        return result;
    }
    if (!enter(audio)) {
        result.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        snprintf(result.detail, sizeof(result.detail), "audio is shut down");
        return result;
    }
    result.accepted = 1;
    if (!audio->settings_root[0]) {
        result.error = GC_AUDIO_PERSISTENCE_FAILED;
        snprintf(result.detail, sizeof(result.detail), "no private settings directory");
        leave(audio);
        return result;
    }
    const GcAudioSettingsWrite write =
        gc_audio_settings_save(audio->settings_root, address, settings, expected_generation);
    result.generation = write.generation;
    result.persisted = write.saved;
    result.readback_verified = write.verified;
    result.durable = write.durable;
    snprintf(result.detail, sizeof(result.detail), "%s", write.error);
    if (write.conflict) result.error = GC_AUDIO_STALE_GENERATION;
    else if (!write.saved) result.error = GC_AUDIO_PERSISTENCE_FAILED;
    else if (!write.verified) result.error = GC_AUDIO_READBACK_FAILED;
    leave(audio);
    return result;
}

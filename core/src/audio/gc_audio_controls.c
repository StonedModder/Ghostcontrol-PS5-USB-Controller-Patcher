/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-04 profile controls and headset telemetry. See gc_audio_controls.h.
 * No HearBridge, SBC, GIP or USB-family source is included. The AVRCP
 * vendor-dependent frame builder/parser below is reworked, not imported:
 * layout and the fallback behaviour are cited to the sibling tree's
 * src/a2dp/avrcp.c, and the strictness (no clamping, exact length match, no
 * reassembly) is this tree's own rule. Radio, RFCOMM and PCM work are not
 * implemented here and are not simulated.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "gc_audio_controls.h"

#include <stdio.h>
#include <string.h>

static int valid_source_id(const char *id)
{
    if (!id) return 0;
    size_t n = 0;
    while (id[n] && n < GC_AUDIO_SOURCE_ID_MAX) ++n;
    if (n == 0 || n >= GC_AUDIO_SOURCE_ID_MAX || id[n]) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int valid_scope(GcAudioSourceScope scope)
{
    return scope == GC_AUDIO_SCOPE_TEST_TONE || scope == GC_AUDIO_SCOPE_APP_OWNED ||
           scope == GC_AUDIO_SCOPE_AUTHORIZED_EXTERNAL;
}

static int one_key(uint32_t key_bit)
{
    return key_bit && (key_bit & (key_bit - 1u)) == 0 &&
           (key_bit & (GC_AUDIO_KEY_PLAY | GC_AUDIO_KEY_PAUSE | GC_AUDIO_KEY_STOP |
                       GC_AUDIO_KEY_NEXT | GC_AUDIO_KEY_PREVIOUS)) == key_bit;
}

static void token(char *dst, size_t cap, const char *text)
{
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", text ? text : "");
}

static int enter(GcAudioControls *c)
{
    pthread_mutex_lock(&c->mutex);
    const int ok = !c->shut_down;
    if (ok) ++c->in_flight;
    pthread_mutex_unlock(&c->mutex);
    return ok;
}

static void leave(GcAudioControls *c)
{
    pthread_mutex_lock(&c->mutex);
    if (--c->in_flight == 0 && c->shut_down) pthread_cond_broadcast(&c->quiet);
    pthread_mutex_unlock(&c->mutex);
}

static void clear_outstanding(GcAudioControls *c)
{
    c->outstanding_id = 0;
    c->outstanding_epoch = 0;
}

static void retire_registration(GcAudioControls *c)
{
    c->registration_active = 0;
    c->registered_mask = 0;
    c->interim_seen = 0;
    c->consumed_mask = 0;
}

static int same_address(const GcAudioControls *c, const char *canonical)
{
    return c->connected && strcmp(c->address, canonical) == 0;
}

static void battery_clear_values(GcAudioControls *c)
{
    c->battery_known = 0;
    c->battery_source = GC_AUDIO_BATT_SOURCE_UNKNOWN;
    c->battery_granularity = GC_AUDIO_BATT_GRANULARITY_UNKNOWN;
    c->battery_generation = 0;
    c->battery_freshness = GC_AUDIO_BATT_FRESHNESS_UNKNOWN;
    c->coarse = -1;
    c->percent = -1;
    c->step = -1;
    c->step_count = 0;
    c->battery_address[0] = '\0';
}

static void battery_accept(GcAudioControls *c, const char *canonical, GcAudioBatterySource source,
                           GcAudioBatteryGranularity granularity)
{
    c->battery_known = 1;
    c->battery_source = source;
    c->battery_granularity = granularity;
    if (c->battery_generation == UINT64_MAX) c->battery_generation = 0;
    ++c->battery_generation;
    c->battery_freshness = GC_AUDIO_BATT_FRESH;
    if (c->battery_freshness_generation < UINT64_MAX) ++c->battery_freshness_generation;
    c->coarse = -1;
    c->percent = -1;
    c->step = -1;
    c->step_count = 0;
    token(c->battery_address, sizeof(c->battery_address), canonical);
}

static void mark_stale_locked(GcAudioControls *c)
{
    if (c->battery_known && c->battery_freshness != GC_AUDIO_BATT_STALE) {
        c->battery_freshness = GC_AUDIO_BATT_STALE;
        if (c->battery_freshness_generation < UINT64_MAX) ++c->battery_freshness_generation;
    }
    if (c->remote_known) c->remote_freshness = GC_AUDIO_BATT_STALE;
    c->headset_acknowledged = 0;
    c->volume_negotiated = 0;
    if (c->volume_path == GC_AUDIO_VOLUME_PATH_PENDING) {
        c->volume_path = GC_AUDIO_VOLUME_PATH_SOFTWARE_FALLBACK;
        token(c->fallback_reason, sizeof(c->fallback_reason), "disconnected");
    }
    c->position_ms = -1;
    c->position_have_sample = 0;
    c->playback_status_known = 0;
    c->hfp_noted = 0;
    c->hfp_address[0] = '\0';
}

static void force_permanent(GcAudioControlsSnapshot *out)
{
    out->ordinary_host_implemented = 1;
    out->service_wired = 0;
    out->target_verified = 0;
    out->radio_opened = 0;
    out->pcm_capture = 0;
    out->microphone = 0;
    out->call_audio = 0;
    out->hfp_audio = 0;
    out->game_playback = 0;
    out->system_soundtrack = 0;
    out->le_audio = 0;
}

int gc_audio_controls_init(GcAudioControls *c, const GcAudioControlChannel *channel)
{
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    if (pthread_mutex_init(&c->mutex, NULL)) return -1;
    if (pthread_cond_init(&c->quiet, NULL)) {
        pthread_mutex_destroy(&c->mutex);
        return -1;
    }
    if (channel && channel->channel && channel->request_absolute_volume &&
        channel->register_notifications && channel->unregister_notifications) {
        c->channel = *channel;
        c->has_channel = 1;
    }
    c->remote_freshness = GC_AUDIO_BATT_FRESHNESS_UNKNOWN;
    c->position_ms = -1;
    c->length_ms = -1;
    c->coarse = -1;
    c->percent = -1;
    c->step = -1;
    c->next_request_id = 1;
    c->next_registration = 1;
    return 0;
}

void gc_audio_controls_shutdown(GcAudioControls *c)
{
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    c->shut_down = 1;
    c->connected = 0;
    clear_outstanding(c);
    retire_registration(c);
    mark_stale_locked(c);
    pthread_mutex_unlock(&c->mutex);
    pthread_mutex_lock(&c->mutex);
    while (c->in_flight) pthread_cond_wait(&c->quiet, &c->mutex);
    pthread_mutex_unlock(&c->mutex);
}

void gc_audio_controls_destroy(GcAudioControls *c)
{
    if (!c) return;
    gc_audio_controls_shutdown(c);
    pthread_cond_destroy(&c->quiet);
    pthread_mutex_destroy(&c->mutex);
}

GcAudioError gc_audio_controls_on_connect(GcAudioControls *c, const char *address)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (gc_audio_address_canonical(address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    GcAudioError error = GC_AUDIO_OK;
    if (c->connected && strcmp(c->address, canonical) != 0) {
        error = GC_AUDIO_BUSY;
    } else if (!c->connected) {
        if (c->battery_known && strcmp(c->battery_address, canonical) != 0) battery_clear_values(c);
        if (c->remote_known && strcmp(c->remote_address, canonical) != 0) {
            c->remote_known = 0;
            c->remote_freshness = GC_AUDIO_BATT_FRESHNESS_UNKNOWN;
            c->remote_address[0] = '\0';
        }
        c->connected = 1;
        token(c->address, sizeof(c->address), canonical);
        if (c->link_epoch < UINT64_MAX) ++c->link_epoch;
        c->absolute_volume = GC_AUDIO_ABS_VOL_UNKNOWN;
        c->volume_negotiated = 0;
        c->headset_acknowledged = 0;
        c->hfp_noted = 0;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

GcAudioError gc_audio_controls_on_disconnect(GcAudioControls *c, const char *address)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (gc_audio_address_canonical(address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    if (!c->connected || strcmp(c->address, canonical) != 0) {
        ++c->stale_rejected;
        c->last_error = GC_AUDIO_STALE_GENERATION;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_STALE_GENERATION;
    }
    const int drop = c->registration_active && c->has_channel;
    const uint64_t registration = c->registration;
    char drop_address[GC_AUDIO_ADDRESS_TEXT];
    token(drop_address, sizeof(drop_address), c->address);
    GcAudioControlChannel channel = c->channel;
    c->connected = 0;
    if (c->link_epoch < UINT64_MAX) ++c->link_epoch;
    clear_outstanding(c);
    retire_registration(c);
    mark_stale_locked(c);
    c->last_error = GC_AUDIO_OK;
    pthread_mutex_unlock(&c->mutex);
    if (drop) channel.unregister_notifications(channel.ctx, drop_address, registration);
    leave(c);
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_controls_on_volume_capability(GcAudioControls *c, const char *address, int supported)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!c || (supported != 0 && supported != 1)) return GC_AUDIO_INVALID_ARGUMENT;
    if (gc_audio_address_canonical(address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    GcAudioError error = GC_AUDIO_OK;
    if (!same_address(c, canonical)) {
        ++c->stale_rejected;
        error = GC_AUDIO_STALE_GENERATION;
    } else if (!supported) {
        c->absolute_volume = GC_AUDIO_ABS_VOL_UNSUPPORTED;
        c->volume_negotiated = 0;
        c->headset_acknowledged = 0;
        if (c->link_epoch < UINT64_MAX) ++c->link_epoch;
        clear_outstanding(c);
    } else {
        c->absolute_volume = GC_AUDIO_ABS_VOL_SUPPORTED;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

static void record_requested(GcAudioControls *c, int volume, GcAudioVolumeResult *result)
{
    c->requested_known = 1;
    c->requested = (uint8_t)volume;
    result->requested_recorded = 1;
}

static void apply_fallback(GcAudioControls *c, GcAudioVolumeResult *result, GcAudioError error,
                           const char *reason, int seam_called)
{
    c->headset_acknowledged = 0;
    c->volume_path = GC_AUDIO_VOLUME_PATH_SOFTWARE_FALLBACK;
    token(c->fallback_reason, sizeof(c->fallback_reason), reason);
    result->error = error;
    result->software_fallback = 1;
    result->headset_acknowledged = 0;
    result->seam_called = seam_called;
    token(result->fallback_reason, sizeof(result->fallback_reason), reason);
    c->last_error = error;
}

GcAudioVolumeResult gc_audio_controls_set_requested_volume(GcAudioControls *c, int volume)
{
    GcAudioVolumeResult result;
    memset(&result, 0, sizeof(result));
    if (!c || volume < 0 || volume > GC_AUDIO_VOLUME_MAX) {
        result.error = GC_AUDIO_INVALID_ARGUMENT;
        return result;
    }
    if (!enter(c)) {
        result.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return result;
    }
    pthread_mutex_lock(&c->mutex);
    record_requested(c, volume, &result);
    if (!c->connected) {
        apply_fallback(c, &result, GC_AUDIO_TRANSPORT_LOST, "disconnected", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    if (!c->has_channel) {
        apply_fallback(c, &result, GC_AUDIO_RADIO_UNAVAILABLE, "channel_absent", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    if (c->absolute_volume == GC_AUDIO_ABS_VOL_UNKNOWN) {
        apply_fallback(c, &result, GC_AUDIO_NEGOTIATION_REJECTED, "absolute_volume_unnegotiated", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    if (c->absolute_volume != GC_AUDIO_ABS_VOL_SUPPORTED) {
        apply_fallback(c, &result, GC_AUDIO_NEGOTIATION_REJECTED, "absolute_volume_unsupported", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    GcAudioControlChannel channel = c->channel;
    char address[GC_AUDIO_ADDRESS_TEXT];
    token(address, sizeof(address), c->address);
    pthread_mutex_unlock(&c->mutex);

    const GcAudioControlChannelState state = channel.channel(channel.ctx);
    pthread_mutex_lock(&c->mutex);
    if (!c->connected || strcmp(c->address, address) != 0 || c->absolute_volume != GC_AUDIO_ABS_VOL_SUPPORTED) {
        apply_fallback(c, &result, GC_AUDIO_TRANSPORT_LOST, "disconnected", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    if (state == GC_AUDIO_CTRL_OWNED_ELSEWHERE) {
        apply_fallback(c, &result, GC_AUDIO_RADIO_OWNED_ELSEWHERE, "channel_owned_elsewhere", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    if (state != GC_AUDIO_CTRL_READY) {
        apply_fallback(c, &result, GC_AUDIO_RADIO_UNAVAILABLE, "channel_absent", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    if (c->next_request_id == 0 || c->next_request_id == UINT64_MAX) {
        apply_fallback(c, &result, GC_AUDIO_PROFILE_UNAVAILABLE, "request_refused", 0);
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    const uint64_t request_id = c->next_request_id++;
    const uint64_t epoch = c->link_epoch;
    const uint8_t remote_known = c->remote_known;
    const uint8_t remote_volume = c->remote_volume;
    const GcAudioBatteryFreshness remote_freshness = c->remote_freshness;
    char remote_address[GC_AUDIO_ADDRESS_TEXT];
    token(remote_address, sizeof(remote_address), c->remote_address);
    const uint8_t acked = c->headset_acknowledged;
    const uint8_t negotiated = c->volume_negotiated;
    const GcAudioVolumePath path = c->volume_path;
    char reason[48];
    token(reason, sizeof(reason), c->fallback_reason);
    c->outstanding_id = request_id;
    c->outstanding_epoch = epoch;
    c->outstanding_volume = (uint8_t)volume;
    c->response_accepted_id = 0;
    result.request_id = request_id;
    pthread_mutex_unlock(&c->mutex);

    const int seam_rc = channel.request_absolute_volume(channel.ctx, address, (uint8_t)volume, request_id);

    pthread_mutex_lock(&c->mutex);
    const int still = c->connected && strcmp(c->address, address) == 0 && c->link_epoch == epoch &&
                      c->absolute_volume == GC_AUDIO_ABS_VOL_SUPPORTED;
    if (!still) {
        if (c->outstanding_id == request_id) clear_outstanding(c);
        result.seam_called = 1;
        result.request_id = request_id;
        result.error = GC_AUDIO_TRANSPORT_LOST;
        result.software_fallback = 1;
        result.headset_acknowledged = 0;
        token(result.fallback_reason, sizeof(result.fallback_reason), "disconnected");
        c->last_error = GC_AUDIO_TRANSPORT_LOST;
    } else if (seam_rc != 0) {
        c->remote_known = remote_known;
        c->remote_volume = remote_volume;
        c->remote_freshness = remote_freshness;
        token(c->remote_address, sizeof(c->remote_address), remote_address);
        c->volume_negotiated = negotiated;
        c->volume_path = path;
        token(c->fallback_reason, sizeof(c->fallback_reason), reason);
        c->headset_acknowledged = acked;
        if (c->outstanding_id == request_id) clear_outstanding(c);
        if (c->response_accepted_id == request_id) c->response_accepted_id = 0;
        apply_fallback(c, &result, GC_AUDIO_NEGOTIATION_REJECTED, "request_refused", 1);
        result.request_id = request_id;
    } else if (c->response_accepted_id == request_id && c->outstanding_id == 0) {
        result.error = GC_AUDIO_OK;
        result.seam_called = 1;
        result.request_id = request_id;
        result.headset_acknowledged = c->headset_acknowledged;
        c->last_error = GC_AUDIO_OK;
    } else {
        c->volume_path = GC_AUDIO_VOLUME_PATH_PENDING;
        c->fallback_reason[0] = '\0';
        c->headset_acknowledged = 0;
        result.error = GC_AUDIO_OK;
        result.seam_called = 1;
        result.request_id = request_id;
        result.headset_acknowledged = 0;
        c->last_error = GC_AUDIO_OK;
    }
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return result;
}

GcAudioError gc_audio_controls_set_software_gain(GcAudioControls *c, int gain_tenths)
{
    if (!c || gain_tenths < GC_AUDIO_GAIN_MIN_TENTHS || gain_tenths > GC_AUDIO_GAIN_MAX_TENTHS)
        return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    c->software_gain_known = 1;
    c->software_gain_tenths = (int16_t)gain_tenths;
    c->last_error = GC_AUDIO_OK;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_controls_note_saved_volume(GcAudioControls *c, const GcAudioHeadsetSettings *settings)
{
    if (!c || !settings || gc_audio_settings_valid(settings)) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    c->requested_known = 1;
    c->requested = settings->volume;
    c->last_error = GC_AUDIO_OK;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_controls_on_volume_response(GcAudioControls *c, const char *address,
                                                  uint64_t request_id, int volume, int origin)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!c || !request_id) return GC_AUDIO_INVALID_ARGUMENT;
    if (gc_audio_address_canonical(address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (origin != GC_AUDIO_ORIGIN_REMOTE && origin != GC_AUDIO_ORIGIN_LOCAL_ECHO)
        return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    GcAudioError error = GC_AUDIO_STALE_GENERATION;
    if (origin == GC_AUDIO_ORIGIN_LOCAL_ECHO) {
        ++c->echoes_rejected;
    } else if (!same_address(c, canonical)) {
        ++c->stale_rejected;
    } else if (request_id == c->last_completed_id) {
        ++c->echoes_rejected;
    } else if (request_id != c->outstanding_id || c->outstanding_epoch != c->link_epoch) {
        ++c->stale_rejected;
    } else if (volume < 0 || volume > GC_AUDIO_VOLUME_MAX) {
        error = GC_AUDIO_INVALID_ARGUMENT;
    } else {
        c->remote_known = 1;
        c->remote_volume = (uint8_t)volume;
        c->remote_freshness = GC_AUDIO_BATT_FRESH;
        token(c->remote_address, sizeof(c->remote_address), canonical);
        c->headset_acknowledged = 1;
        c->volume_negotiated = c->absolute_volume == GC_AUDIO_ABS_VOL_SUPPORTED;
        c->volume_path = GC_AUDIO_VOLUME_PATH_REMOTE;
        c->fallback_reason[0] = '\0';
        c->last_completed_id = request_id;
        c->response_accepted_id = request_id;
        clear_outstanding(c);
        error = GC_AUDIO_OK;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

GcAudioError gc_audio_controls_register_notifications(GcAudioControls *c, uint32_t mask)
{
    if (!c || !mask || (mask & ~GC_AUDIO_NOTIFY_ALL)) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    if (!c->connected) {
        c->last_error = GC_AUDIO_TRANSPORT_LOST;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_TRANSPORT_LOST;
    }
    if (!c->has_channel) {
        c->last_error = GC_AUDIO_RADIO_UNAVAILABLE;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_RADIO_UNAVAILABLE;
    }
    GcAudioControlChannel channel = c->channel;
    char address[GC_AUDIO_ADDRESS_TEXT];
    token(address, sizeof(address), c->address);
    const uint64_t epoch = c->link_epoch;
    pthread_mutex_unlock(&c->mutex);
    const GcAudioControlChannelState state = channel.channel(channel.ctx);
    pthread_mutex_lock(&c->mutex);
    if (!c->connected || strcmp(c->address, address) != 0 || c->link_epoch != epoch) {
        c->last_error = GC_AUDIO_TRANSPORT_LOST;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_TRANSPORT_LOST;
    }
    if (state == GC_AUDIO_CTRL_OWNED_ELSEWHERE) {
        c->last_error = GC_AUDIO_RADIO_OWNED_ELSEWHERE;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_RADIO_OWNED_ELSEWHERE;
    }
    if (state != GC_AUDIO_CTRL_READY) {
        c->last_error = GC_AUDIO_RADIO_UNAVAILABLE;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_RADIO_UNAVAILABLE;
    }
    if (c->next_registration == 0 || c->next_registration == UINT64_MAX) {
        c->last_error = GC_AUDIO_PROFILE_UNAVAILABLE;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    const uint64_t proposed = c->next_registration;
    const int was_active = c->registration_active;
    pthread_mutex_unlock(&c->mutex);
    const int seam_rc = channel.register_notifications(channel.ctx, address, proposed, mask);
    pthread_mutex_lock(&c->mutex);
    GcAudioError error = GC_AUDIO_NEGOTIATION_REJECTED;
    if (!c->connected || strcmp(c->address, address) != 0 || c->link_epoch != epoch) {
        error = GC_AUDIO_TRANSPORT_LOST;
    } else if (seam_rc == 0) {
        c->registration = proposed;
        c->next_registration = proposed + 1;
        c->registration_active = 1;
        c->registered_mask = mask;
        c->interim_seen = 0;
        c->consumed_mask = 0;
        if (was_active) ++c->reregistrations;
        else ++c->registrations;
        error = GC_AUDIO_OK;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

GcAudioError gc_audio_controls_unregister_notifications(GcAudioControls *c)
{
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    if (!c->registration_active) {
        c->last_error = GC_AUDIO_PROFILE_UNAVAILABLE;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    if (!c->has_channel) {
        retire_registration(c);
        c->last_error = GC_AUDIO_OK;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return GC_AUDIO_OK;
    }
    GcAudioControlChannel channel = c->channel;
    char address[GC_AUDIO_ADDRESS_TEXT];
    token(address, sizeof(address), c->address);
    const uint64_t registration = c->registration;
    const uint64_t epoch = c->link_epoch;
    pthread_mutex_unlock(&c->mutex);
    const int seam_rc = channel.unregister_notifications(channel.ctx, address, registration);
    pthread_mutex_lock(&c->mutex);
    GcAudioError error = GC_AUDIO_NEGOTIATION_REJECTED;
    if (c->link_epoch != epoch) {
        error = GC_AUDIO_TRANSPORT_LOST;
    } else if (seam_rc == 0 && c->registration == registration) {
        retire_registration(c);
        error = GC_AUDIO_OK;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

static GcAudioError begin_notification(GcAudioControls *c, const GcAudioControlMeta *meta, uint32_t bit,
                                       char canonical[GC_AUDIO_ADDRESS_TEXT])
{
    if (!meta || !meta->address) return GC_AUDIO_INVALID_ARGUMENT;
    if (gc_audio_address_canonical(meta->address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (meta->origin != GC_AUDIO_ORIGIN_REMOTE && meta->origin != GC_AUDIO_ORIGIN_LOCAL_ECHO)
        return GC_AUDIO_INVALID_ARGUMENT;
    if (meta->origin == GC_AUDIO_ORIGIN_LOCAL_ECHO) {
        ++c->echoes_rejected;
        return GC_AUDIO_STALE_GENERATION;
    }
    if (!same_address(c, canonical) || !c->registration_active || meta->registration != c->registration ||
        (c->registered_mask & bit) == 0) {
        ++c->stale_rejected;
        return GC_AUDIO_STALE_GENERATION;
    }
    if (meta->kind != GC_AUDIO_NOTIFY_INTERIM && meta->kind != GC_AUDIO_NOTIFY_CHANGED) {
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    if (meta->request_id && meta->request_id == c->outstanding_id) {
        ++c->echoes_rejected;
        return GC_AUDIO_STALE_GENERATION;
    }
    if (meta->kind == GC_AUDIO_NOTIFY_CHANGED && (c->consumed_mask & bit)) {
        ++c->stale_rejected;
        return GC_AUDIO_STALE_GENERATION;
    }
    if (meta->kind == GC_AUDIO_NOTIFY_INTERIM && (c->interim_seen & bit)) {
        ++c->echoes_rejected;
        return GC_AUDIO_STALE_GENERATION;
    }
    return GC_AUDIO_OK;
}

static void consume_notification(GcAudioControls *c, uint32_t bit, int kind)
{
    if (kind == GC_AUDIO_NOTIFY_INTERIM) c->interim_seen |= bit;
    else c->consumed_mask |= bit;
}

GcAudioError gc_audio_controls_on_volume_notification(GcAudioControls *c, const GcAudioControlMeta *meta,
                                                      int volume)
{
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    GcAudioError error = begin_notification(c, meta, GC_AUDIO_NOTIFY_VOLUME, canonical);
    if (!error && (volume < 0 || volume > GC_AUDIO_VOLUME_MAX)) error = GC_AUDIO_INVALID_ARGUMENT;
    if (!error) {
        c->remote_known = 1;
        c->remote_volume = (uint8_t)volume;
        c->remote_freshness = GC_AUDIO_BATT_FRESH;
        token(c->remote_address, sizeof(c->remote_address), canonical);
        c->volume_path = GC_AUDIO_VOLUME_PATH_REMOTE;
        c->fallback_reason[0] = '\0';
        consume_notification(c, GC_AUDIO_NOTIFY_VOLUME, meta->kind);
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

GcAudioError gc_audio_controls_on_playback(GcAudioControls *c, const GcAudioControlMeta *meta,
                                           const GcAudioPlaybackInfo *info)
{
    if (!c || !info) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    GcAudioError error = begin_notification(c, meta, GC_AUDIO_NOTIFY_PLAYBACK, canonical);
    if (!error && info->status > GC_AUDIO_PLAYBACK_STOPPED) error = GC_AUDIO_INVALID_ARGUMENT;
    if (!error) {
        if (info->status >= 0) {
            if (info->status == GC_AUDIO_PLAYBACK_UNKNOWN) c->playback_status_known = 0;
            else {
                c->playback_status_known = 1;
                c->playback_status = (GcAudioPlaybackStatus)info->status;
            }
        }
        if (info->length_present) c->length_ms = info->length_ms > 0 ? (int64_t)info->length_ms : -1;
        if (info->position_present) {
            if (!c->position_have_sample || c->position_sample == info->position_ms) {
                c->position_have_sample = 1;
                c->position_sample = info->position_ms;
                c->position_ms = -1;
            } else {
                c->position_sample = info->position_ms;
                c->position_ms = (int64_t)info->position_ms;
            }
        }
        consume_notification(c, GC_AUDIO_NOTIFY_PLAYBACK, meta->kind);
        (void)canonical;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

GcAudioError gc_audio_controls_authorize_source(GcAudioControls *c, const GcAudioPlaybackSource *source)
{
    if (!c || !source || !valid_source_id(source->id) || !valid_scope(source->scope))
        return GC_AUDIO_INVALID_ARGUMENT;
    const uint32_t allowed = GC_AUDIO_KEY_PLAY | GC_AUDIO_KEY_PAUSE | GC_AUDIO_KEY_STOP |
                             GC_AUDIO_KEY_NEXT | GC_AUDIO_KEY_PREVIOUS;
    if (source->implemented & ~allowed) return GC_AUDIO_INVALID_ARGUMENT;
    if (source->implemented && !source->on_key) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    c->source = *source;
    c->has_source = 1;
    if (c->source_generation < UINT64_MAX) ++c->source_generation;
    c->source_key_applied = 0;
    c->last_key_source[0] = '\0';
    c->last_error = GC_AUDIO_OK;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_controls_revoke_source(GcAudioControls *c)
{
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    memset(&c->source, 0, sizeof(c->source));
    c->has_source = 0;
    if (c->source_generation < UINT64_MAX) ++c->source_generation;
    c->last_error = GC_AUDIO_OK;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return GC_AUDIO_OK;
}

GcAudioKeyResult gc_audio_controls_on_playback_key(GcAudioControls *c, uint32_t key_bit)
{
    GcAudioKeyResult result;
    memset(&result, 0, sizeof(result));
    if (!c || !one_key(key_bit)) {
        result.error = GC_AUDIO_INVALID_ARGUMENT;
        return result;
    }
    if (!enter(c)) {
        result.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return result;
    }
    pthread_mutex_lock(&c->mutex);
    if (!c->has_source) {
        result.error = GC_AUDIO_SOURCE_UNAVAILABLE;
        c->last_error = result.error;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    if ((c->source.implemented & key_bit) == 0 || !c->source.on_key) {
        result.error = GC_AUDIO_OK;
        c->last_error = GC_AUDIO_OK;
        pthread_mutex_unlock(&c->mutex);
        leave(c);
        return result;
    }
    GcAudioPlaybackSource source = c->source;
    const uint64_t generation = c->source_generation;
    pthread_mutex_unlock(&c->mutex);
    const int applied = source.on_key(source.ctx, key_bit, generation) == 0;
    pthread_mutex_lock(&c->mutex);
    result.offered = 1;
    token(result.affects, sizeof(result.affects), source.id);
    if (c->has_source && c->source_generation == generation && applied) {
        result.consumed = 1;
        c->source_key_applied = 1;
        token(c->last_key_source, sizeof(c->last_key_source), source.id);
    }
    result.error = GC_AUDIO_OK;
    c->last_error = GC_AUDIO_OK;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return result;
}

GcAudioError gc_audio_controls_note_hfp_battery_only(GcAudioControls *c, const char *address)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (gc_audio_address_canonical(address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    GcAudioError error = GC_AUDIO_OK;
    if (!same_address(c, canonical)) {
        ++c->stale_rejected;
        error = GC_AUDIO_STALE_GENERATION;
    } else {
        c->hfp_noted = 1;
        token(c->hfp_address, sizeof(c->hfp_address), canonical);
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

GcAudioError gc_audio_controls_on_avrcp_battery(GcAudioControls *c, const GcAudioControlMeta *meta, int coarse)
{
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    GcAudioError error = begin_notification(c, meta, GC_AUDIO_NOTIFY_BATTERY, canonical);
    if (!error && (coarse < GC_AUDIO_BATT_COARSE_NORMAL || coarse > GC_AUDIO_BATT_COARSE_FULL))
        error = GC_AUDIO_INVALID_ARGUMENT;
    if (!error) {
        battery_accept(c, canonical, GC_AUDIO_BATT_SOURCE_AVRCP, GC_AUDIO_BATT_GRANULARITY_COARSE);
        c->coarse = coarse;
        consume_notification(c, GC_AUDIO_NOTIFY_BATTERY, meta->kind);
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

static GcAudioError begin_hfp(GcAudioControls *c, const char *address, int origin, char canonical[GC_AUDIO_ADDRESS_TEXT])
{
    if (gc_audio_address_canonical(address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (origin != GC_AUDIO_ORIGIN_REMOTE && origin != GC_AUDIO_ORIGIN_LOCAL_ECHO)
        return GC_AUDIO_INVALID_ARGUMENT;
    if (origin == GC_AUDIO_ORIGIN_LOCAL_ECHO) {
        ++c->echoes_rejected;
        return GC_AUDIO_STALE_GENERATION;
    }
    if (!same_address(c, canonical) || !c->hfp_noted || strcmp(c->hfp_address, canonical) != 0) {
        ++c->stale_rejected;
        return GC_AUDIO_STALE_GENERATION;
    }
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_controls_on_hfp_percent(GcAudioControls *c, const char *address, int percent, int origin)
{
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    GcAudioError error = begin_hfp(c, address, origin, canonical);
    if (!error && (percent < 0 || percent > 100)) error = GC_AUDIO_INVALID_ARGUMENT;
    if (!error) {
        battery_accept(c, canonical, GC_AUDIO_BATT_SOURCE_HFP, GC_AUDIO_BATT_GRANULARITY_PERCENT);
        c->percent = percent;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

GcAudioError gc_audio_controls_on_hfp_stepped(GcAudioControls *c, const char *address, int step,
                                             int step_count, int origin)
{
    if (!c) return GC_AUDIO_INVALID_ARGUMENT;
    if (!enter(c)) return GC_AUDIO_PROFILE_UNAVAILABLE;
    pthread_mutex_lock(&c->mutex);
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    GcAudioError error = begin_hfp(c, address, origin, canonical);
    if (!error && (step_count < 2 || step_count > 10 || step < 0 || step >= step_count))
        error = GC_AUDIO_INVALID_ARGUMENT;
    if (!error) {
        battery_accept(c, canonical, GC_AUDIO_BATT_SOURCE_HFP, GC_AUDIO_BATT_GRANULARITY_STEPPED);
        c->step = step;
        c->step_count = step_count;
    }
    c->last_error = error;
    pthread_mutex_unlock(&c->mutex);
    leave(c);
    return error;
}

static void fill_snapshot(const GcAudioControls *c, GcAudioControlsSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    force_permanent(out);
    out->shut_down = (uint8_t)c->shut_down;
    out->connected = (uint8_t)c->connected;
    if (c->connected) token(out->address, sizeof(out->address), c->address);
    out->requested_known = c->requested_known;
    out->requested = c->requested;
    out->remote_known = c->remote_known;
    out->remote_volume = c->remote_volume;
    out->remote_freshness = c->remote_known ? c->remote_freshness : GC_AUDIO_BATT_FRESHNESS_UNKNOWN;
    out->software_gain_known = c->software_gain_known;
    out->software_gain_tenths = c->software_gain_tenths;
    out->headset_acknowledged = c->headset_acknowledged;
    out->volume_path = c->volume_path;
    token(out->fallback_reason, sizeof(out->fallback_reason), c->fallback_reason);
    out->volume_negotiated = c->volume_negotiated;
    out->absolute_volume = c->absolute_volume;
    out->outstanding_request = c->outstanding_id;
    out->notifications_registered = (uint8_t)c->registration_active;
    out->registration = c->registration_active ? c->registration : 0;
    out->registered_mask = c->registered_mask;
    out->reregister_owed = c->consumed_mask;
    out->registrations = c->registrations;
    out->reregistrations = c->reregistrations;
    out->playback_status_known = c->playback_status_known;
    out->playback_status = c->playback_status_known ? c->playback_status : GC_AUDIO_PLAYBACK_UNKNOWN;
    out->position_ms = c->position_ms;
    out->length_ms = c->length_ms;
    if (c->has_source) {
        token(out->playback_source, sizeof(out->playback_source), c->source.id);
        out->playback_implemented = c->source.implemented;
    }
    out->source_key_applied = c->source_key_applied;
    token(out->last_key_source, sizeof(out->last_key_source), c->last_key_source);
    out->battery_known = c->battery_known;
    out->battery_source = c->battery_known ? c->battery_source : GC_AUDIO_BATT_SOURCE_UNKNOWN;
    out->battery_granularity = c->battery_known ? c->battery_granularity : GC_AUDIO_BATT_GRANULARITY_UNKNOWN;
    out->battery_generation = c->battery_known ? c->battery_generation : 0;
    out->battery_freshness_generation = c->battery_known ? c->battery_freshness_generation : 0;
    out->battery_freshness = c->battery_known ? c->battery_freshness : GC_AUDIO_BATT_FRESHNESS_UNKNOWN;
    out->coarse = c->battery_known ? c->coarse : -1;
    out->percent = c->battery_known ? c->percent : -1;
    out->step = c->battery_known ? c->step : -1;
    out->step_count = c->battery_known ? c->step_count : 0;
    out->hfp_battery_only = (uint8_t)c->hfp_noted;
    if (c->battery_known) token(out->battery_address, sizeof(out->battery_address), c->battery_address);
    out->echoes_rejected = c->echoes_rejected;
    out->stale_rejected = c->stale_rejected;
    out->last_error = c->last_error;
}

void gc_audio_controls_snapshot(const GcAudioControls *c, GcAudioControlsSnapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    force_permanent(out);
    if (!c) return;
    pthread_mutex_lock((pthread_mutex_t *)&c->mutex);
    fill_snapshot(c, out);
    pthread_mutex_unlock((pthread_mutex_t *)&c->mutex);
}

void gc_audio_controls_capabilities(const GcAudioControls *c, GcAudioControlsCapabilities *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ordinary_host_implemented = 1;
    if (!c) return;
    pthread_mutex_lock((pthread_mutex_t *)&c->mutex);
    out->channel_configured = (uint8_t)c->has_channel;
    out->hfp_battery_only = (uint8_t)c->hfp_noted;
    out->notifications_registered = (uint8_t)c->registration_active;
    out->absolute_volume = c->absolute_volume;
    out->shut_down = (uint8_t)c->shut_down;
    pthread_mutex_unlock((pthread_mutex_t *)&c->mutex);
}

int gc_audio_controls_microphone_available(const GcAudioControls *c)
{
    (void)c;
    return 0;
}

int gc_audio_controls_call_audio_available(const GcAudioControls *c)
{
    (void)c;
    return 0;
}

int gc_audio_controls_hfp_audio_available(const GcAudioControls *c)
{
    (void)c;
    return 0;
}

int gc_audio_controls_game_playback_available(const GcAudioControls *c)
{
    (void)c;
    return 0;
}

const char *gc_audio_controls_volume_path_name(GcAudioVolumePath path)
{
    switch (path) {
    case GC_AUDIO_VOLUME_PATH_NONE: return "none";
    case GC_AUDIO_VOLUME_PATH_SOFTWARE_FALLBACK: return "software_fallback";
    case GC_AUDIO_VOLUME_PATH_PENDING: return "pending";
    case GC_AUDIO_VOLUME_PATH_REMOTE: return "remote";
    }
    return "none";
}

const char *gc_audio_controls_battery_source_name(GcAudioBatterySource source)
{
    switch (source) {
    case GC_AUDIO_BATT_SOURCE_UNKNOWN: return "unknown";
    case GC_AUDIO_BATT_SOURCE_AVRCP: return "avrcp";
    case GC_AUDIO_BATT_SOURCE_HFP: return "hfp";
    }
    return "unknown";
}

const char *gc_audio_controls_granularity_name(GcAudioBatteryGranularity granularity)
{
    switch (granularity) {
    case GC_AUDIO_BATT_GRANULARITY_UNKNOWN: return "unknown";
    case GC_AUDIO_BATT_GRANULARITY_COARSE: return "coarse";
    case GC_AUDIO_BATT_GRANULARITY_PERCENT: return "percent";
    case GC_AUDIO_BATT_GRANULARITY_STEPPED: return "stepped";
    }
    return "unknown";
}

const char *gc_audio_controls_freshness_name(GcAudioBatteryFreshness freshness)
{
    switch (freshness) {
    case GC_AUDIO_BATT_FRESHNESS_UNKNOWN: return "unknown";
    case GC_AUDIO_BATT_FRESH: return "fresh";
    case GC_AUDIO_BATT_STALE: return "stale";
    }
    return "unknown";
}

const char *gc_audio_controls_coarse_name(int coarse)
{
    switch (coarse) {
    case GC_AUDIO_BATT_COARSE_NORMAL: return "normal";
    case GC_AUDIO_BATT_COARSE_WARNING: return "warning";
    case GC_AUDIO_BATT_COARSE_CRITICAL: return "critical";
    case GC_AUDIO_BATT_COARSE_EXTERNAL: return "external";
    case GC_AUDIO_BATT_COARSE_FULL: return "full_charge";
    default: return "unknown";
    }
}

/* --- AVRCP vendor-dependent frame -----------------------------------------
 * Layout reworked from the sibling implementation's avrcp.c (HB
 * src/a2dp/avrcp.c:129-154 builds the AVCTP + AV/C + vendor header,
 * :167-204 the three commands, :206-269 the response dispatch). This version
 * refuses instead of clamping and checks every length, and it owns no clock:
 * the caller keeps the request ids, the notifications and the battery state.
 */

static void avrcp_header(uint8_t *out, uint8_t label, int response, uint8_t ctype)
{
    out[0] = (uint8_t)((uint8_t)((label & 0x0Fu) << 4) | (response ? 0x02u : 0x00u));
    out[1] = (uint8_t)(GC_AUDIO_AVCTP_PID >> 8);
    out[2] = (uint8_t)(GC_AUDIO_AVCTP_PID & 0xFFu);
    /* AV/C ctype is a byte whose high nibble is reserved zero: a CONTROL
     * command is 0x00, NOTIFY 0x03, CHANGED 0x0D. Recording that wrong (in the
     * high nibble) is what the builder vectors caught during this rework. */
    out[3] = (uint8_t)(ctype & 0x0Fu);
    out[4] = GC_AUDIO_AVRCP_SUBUNIT_PANEL;
    out[5] = GC_AUDIO_AVRCP_OPCODE_VENDOR;
    out[6] = 0x00u;
    out[7] = 0x19u;
    out[8] = 0x58u;
}

static GcAudioError avrcp_frame(uint8_t *out, size_t cap, uint8_t label, int response,
                                uint8_t ctype, uint8_t pdu, const uint8_t *params, size_t param_len,
                                size_t *out_len)
{
    if (!out || !out_len) return GC_AUDIO_INVALID_ARGUMENT;
    *out_len = 0;
    if (param_len > 0xFFFFu) return GC_AUDIO_INVALID_ARGUMENT;
    if (cap < GC_AUDIO_AVRCP_HEADER || cap - GC_AUDIO_AVRCP_HEADER < param_len) {
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    avrcp_header(out, label, response, ctype);
    out[9] = pdu;
    out[10] = 0x00u; /* packet type: single */
    out[11] = (uint8_t)(param_len >> 8);
    out[12] = (uint8_t)(param_len & 0xFFu);
    if (param_len) memcpy(out + GC_AUDIO_AVRCP_HEADER, params, param_len);
    *out_len = GC_AUDIO_AVRCP_HEADER + param_len;
    return GC_AUDIO_OK;
}

static int avrcp_label_ok(uint8_t label)
{
    return label <= 15u;
}

static int avrcp_event_reported(int event)
{
    return event == GC_AUDIO_AVRCP_EVENT_VOLUME_CHANGED || event == GC_AUDIO_AVRCP_EVENT_BATT_STATUS;
}

GcAudioError gc_audio_controls_build_notification_registration(uint8_t *out, size_t cap,
                                                              uint8_t label, int event,
                                                              size_t *out_len)
{
    if (!out || !out_len) return GC_AUDIO_INVALID_ARGUMENT;
    *out_len = 0;
    if (!avrcp_label_ok(label) || !avrcp_event_reported(event)) return GC_AUDIO_INVALID_ARGUMENT;
    const uint8_t params[5] = {(uint8_t)event, 0x00u, 0x00u, 0x00u, 0x00u};
    return avrcp_frame(out, cap, label, 0, GC_AUDIO_AVRCP_CTYPE_NOTIFY,
                       GC_AUDIO_AVRCP_PDU_REGISTER_NOTIFICATION, params, sizeof params, out_len);
}

GcAudioError gc_audio_controls_build_absolute_volume(uint8_t *out, size_t cap, uint8_t label,
                                                     int volume, size_t *out_len)
{
    if (!out || !out_len) return GC_AUDIO_INVALID_ARGUMENT;
    *out_len = 0;
    if (!avrcp_label_ok(label) || volume < 0 || volume > 127) return GC_AUDIO_INVALID_ARGUMENT;
    const uint8_t params[1] = {(uint8_t)volume};
    return avrcp_frame(out, cap, label, 0, GC_AUDIO_AVRCP_CTYPE_CONTROL,
                       GC_AUDIO_AVRCP_PDU_SET_ABSOLUTE_VOLUME, params, 1, out_len);
}

GcAudioError gc_audio_controls_build_volume_changed(uint8_t *out, size_t cap, uint8_t label,
                                                    int volume, size_t *out_len)
{
    if (!out || !out_len) return GC_AUDIO_INVALID_ARGUMENT;
    *out_len = 0;
    if (!avrcp_label_ok(label) || volume < 0 || volume > 127) return GC_AUDIO_INVALID_ARGUMENT;
    const uint8_t params[2] = {GC_AUDIO_AVRCP_EVENT_VOLUME_CHANGED, (uint8_t)volume};
    return avrcp_frame(out, cap, label, 1, GC_AUDIO_AVRCP_CTYPE_CHANGED,
                       GC_AUDIO_AVRCP_PDU_REGISTER_NOTIFICATION, params, sizeof params, out_len);
}

GcAudioError gc_audio_controls_parse_avctp(const uint8_t *in, size_t len, GcAudioAvrcpFrame *out)
{
    if (!out) return GC_AUDIO_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->event = -1;
    out->value = -1;
    out->volume = -1;
    if (!in) {
        out->error = GC_AUDIO_INVALID_ARGUMENT;
        return out->error;
    }
    out->error = GC_AUDIO_NEGOTIATION_REJECTED;
    if (len < GC_AUDIO_AVRCP_HEADER) return out->error;
    if (((in[0] >> 2) & 0x03u) != 0) return out->error; /* fragmented AVCTP frame */
    if ((((uint16_t)in[1] << 8) | in[2]) != GC_AUDIO_AVCTP_PID) return out->error;
    if ((in[3] & 0xF0u) != 0) return out->error;        /* reserved nibble */
    if ((in[4] & 0xF8u) != GC_AUDIO_AVRCP_SUBUNIT_PANEL) return out->error;
    if (in[5] != GC_AUDIO_AVRCP_OPCODE_VENDOR) return out->error;
    if (in[6] != 0x00u || in[7] != 0x19u || in[8] != 0x58u) return out->error;
    if (in[10] != 0x00u) return out->error;             /* fragmented payload */
    const size_t param_len = ((size_t)in[11] << 8) | in[12];
    if (param_len != len - GC_AUDIO_AVRCP_HEADER) return out->error;

    out->label = (uint8_t)((in[0] >> 4) & 0x0Fu);
    out->response = (in[0] >> 1) & 1;
    out->ipid = in[0] & 1;
    out->ctype = (uint8_t)(in[3] & 0x0Fu);
    out->pdu = in[9];
    out->param_len = param_len;

    if (out->pdu == GC_AUDIO_AVRCP_PDU_REGISTER_NOTIFICATION) {
        /* A registration carries event + a 4-byte value; the answer to one
         * carries event + the value. Anything else is a frame this box will
         * not guess about. */
        if (!out->response && param_len != 5) return out->error;
        if (out->response && param_len != 2) return out->error;
        out->event = in[13];
        out->value = in[14];
        if (out->event == GC_AUDIO_AVRCP_EVENT_VOLUME_CHANGED) {
            if (out->value < 0 || out->value > 127) return out->error;
            out->volume = out->value;
        }
    } else if (out->pdu == GC_AUDIO_AVRCP_PDU_SET_ABSOLUTE_VOLUME) {
        if (param_len != 1) return out->error;
        if (in[13] > 127u) return out->error;
        out->value = in[13];
        out->volume = in[13];
    }
    out->error = GC_AUDIO_OK;
    return GC_AUDIO_OK;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-04: profile controls and headset telemetry.
 *
 * Unwired ordinary HOST module. It plugs into the AUDIO-01 contract
 * (GcAudioError, canonical headset address, 0..127 volume, source-id
 * rules, source scopes) and reads AUDIO-05's saved volume as a request.
 * It does not replace either module, and it does not open a radio,
 * capture PCM, encode AVRCP/HFP PDUs, start a stream, or claim game or
 * system playback.
 *
 * What this module owns is the meaning of already-decoded owner reports
 * and of requests handed to an injected control seam:
 *
 *   - Requested volume, remote-reported volume and applied software gain
 *     are three fields. None is derived from another. A missing channel,
 *     an unnegotiated or refused absolute-volume capability, or a seam
 *     refusal is a labeled software-volume fallback. The request call
 *     itself is never a headset acknowledgement. Only a later command
 *     response for that request id can acknowledge, and a seam failure
 *     rolls a synchronous response back.
 *   - Notifications register and re-register through the seam. A
 *     generation is committed only when the seam accepts. Echoes and
 *     stale reports change no value. An omitted, first, or repeated
 *     playback position stays unknown.
 *   - A playback key is offered only to an authorized source that
 *     implements that key. game playback and the system soundtrack are
 *     not targets and are not advertised.
 *   - Battery carries source, granularity, generation and freshness.
 *     AVRCP coarse status is not a percentage. HFP percent is not coarse.
 *     A stepped report is not converted to a percentage. Unknown stays
 *     unknown. Disconnect keeps a retained value and marks it stale.
 *     HFP here is battery-only: microphone, call audio and HFP audio
 *     stay unavailable.
 *
 * Outside this box (not faked): L2CAP, SDP, feature-exchange packets, HFP
 * RFCOMM/AT framing, radio open, ACL credits, PCM, SBC/RTP, applying gain to
 * a live stream, IPC and UI.
 *
 * Inside this box since the AUDIO-02/03/04 rework: the AVRCP vendor-dependent
 * frame itself, byte-in/byte-out (below). The rework took the frame layout
 * from the sibling implementation's avrcp.c (HB src/a2dp/avrcp.c:129-204 for
 * the builder, :206-269 for the response dispatch) and reworked it to this
 * tree's rules: a parameter out of range is refused and never clamped (HB
 * clamps a volume to 0..127 at avrcp.c:184-186), every length field must
 * match the bytes actually present, a fragmented frame is refused instead of
 * reassembled, and nothing here owns a timer or a state machine - the
 * semantic fields above stay authoritative. What it does not take from HB:
 * the wall-clock echo/hold windows (AVRCP_ECHO_MS, AVRCP_HOLD_MS,
 * AVRCP_KEY_SEND_MS), because this module already tells an echo from a
 * headset change by request id and origin, which does not depend on how
 * promptly a headset answers.
 *
 * Without an owner seam that has actually negotiated absolute volume,
 * every volume request stays a labeled software fallback.
 */
#ifndef GC_AUDIO_CONTROLS_H
#define GC_AUDIO_CONTROLS_H

#include "gc_audio.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_AUDIO_NOTIFY_VOLUME   (1u << 0)
#define GC_AUDIO_NOTIFY_PLAYBACK (1u << 1)
#define GC_AUDIO_NOTIFY_BATTERY  (1u << 2)
#define GC_AUDIO_NOTIFY_ALL \
    (GC_AUDIO_NOTIFY_VOLUME | GC_AUDIO_NOTIFY_PLAYBACK | GC_AUDIO_NOTIFY_BATTERY)

#define GC_AUDIO_KEY_PLAY     (1u << 0)
#define GC_AUDIO_KEY_PAUSE    (1u << 1)
#define GC_AUDIO_KEY_STOP     (1u << 2)
#define GC_AUDIO_KEY_NEXT     (1u << 3)
#define GC_AUDIO_KEY_PREVIOUS (1u << 4)

typedef enum {
    GC_AUDIO_CTRL_ABSENT = 0,
    GC_AUDIO_CTRL_OWNED_ELSEWHERE = 1,
    GC_AUDIO_CTRL_READY = 2
} GcAudioControlChannelState;

/* Injected profile-control seam on the already-authorized owner.
 * channel() must not open a radio. A seam missing any callback is absent.
 * request_absolute_volume returning 0 means the owner accepted the request
 * as pending. It is not a headset acknowledgement. register returns 0 only
 * when the owner accepted that registration generation. Reports for a
 * generation that this module has not committed are stale, including a
 * report delivered synchronously from inside register. A command response
 * may be delivered synchronously from inside request_absolute_volume. */
typedef struct {
    void *ctx;
    GcAudioControlChannelState (*channel)(void *ctx);
    int (*request_absolute_volume)(void *ctx, const char *address, uint8_t volume,
                                   uint64_t request_id);
    int (*register_notifications)(void *ctx, const char *address, uint64_t registration,
                                  uint32_t mask);
    int (*unregister_notifications)(void *ctx, const char *address, uint64_t registration);
} GcAudioControlChannel;

typedef enum {
    GC_AUDIO_ABS_VOL_UNKNOWN = 0,
    GC_AUDIO_ABS_VOL_SUPPORTED = 1,
    GC_AUDIO_ABS_VOL_UNSUPPORTED = 2
} GcAudioAbsoluteVolume;

typedef enum {
    GC_AUDIO_VOLUME_PATH_NONE = 0,
    GC_AUDIO_VOLUME_PATH_SOFTWARE_FALLBACK = 1,
    GC_AUDIO_VOLUME_PATH_PENDING = 2,
    GC_AUDIO_VOLUME_PATH_REMOTE = 3
} GcAudioVolumePath;

typedef enum {
    GC_AUDIO_ORIGIN_REMOTE = 1,
    GC_AUDIO_ORIGIN_LOCAL_ECHO = 2
} GcAudioReportOrigin;

typedef enum {
    GC_AUDIO_NOTIFY_INTERIM = 1, /* current value; does not consume the registration */
    GC_AUDIO_NOTIFY_CHANGED = 2  /* one-shot; owes re-register before another changed */
} GcAudioNotifyKind;

typedef enum {
    GC_AUDIO_PLAYBACK_UNKNOWN = 0,
    GC_AUDIO_PLAYBACK_PLAYING = 1,
    GC_AUDIO_PLAYBACK_PAUSED = 2,
    GC_AUDIO_PLAYBACK_STOPPED = 3
} GcAudioPlaybackStatus;

typedef enum {
    GC_AUDIO_BATT_SOURCE_UNKNOWN = 0,
    GC_AUDIO_BATT_SOURCE_AVRCP = 1,
    GC_AUDIO_BATT_SOURCE_HFP = 2
} GcAudioBatterySource;

typedef enum {
    GC_AUDIO_BATT_GRANULARITY_UNKNOWN = 0,
    GC_AUDIO_BATT_GRANULARITY_COARSE = 1,  /* AVRCP status, not a percent */
    GC_AUDIO_BATT_GRANULARITY_PERCENT = 2, /* HFP integer percent */
    GC_AUDIO_BATT_GRANULARITY_STEPPED = 3  /* stepped report, not converted */
} GcAudioBatteryGranularity;

typedef enum {
    GC_AUDIO_BATT_FRESHNESS_UNKNOWN = 0,
    GC_AUDIO_BATT_FRESH = 1,
    GC_AUDIO_BATT_STALE = 2
} GcAudioBatteryFreshness;

/* AVRCP Battery Status 0..4. There is no conversion of these to a percent. */
#define GC_AUDIO_BATT_COARSE_NORMAL 0
#define GC_AUDIO_BATT_COARSE_WARNING 1
#define GC_AUDIO_BATT_COARSE_CRITICAL 2
#define GC_AUDIO_BATT_COARSE_EXTERNAL 3
#define GC_AUDIO_BATT_COARSE_FULL 4

typedef struct {
    const char *address;
    uint64_t registration;
    uint64_t request_id; /* 0 if the report is not tied to one of our requests */
    int origin;          /* GcAudioReportOrigin; anything else is invalid */
    int kind;            /* GcAudioNotifyKind; required for notifications */
} GcAudioControlMeta;

typedef struct {
    GcAudioError error;
    int requested_recorded;
    int seam_called;           /* request_absolute_volume, not channel() */
    int headset_acknowledged;  /* 1 only if a command response was accepted */
    int software_fallback;
    char fallback_reason[48];  /* stable token; empty unless software_fallback */
    uint64_t request_id;       /* 0 if no request was issued */
} GcAudioVolumeResult;

typedef struct {
    int status; /* <0 omit; otherwise GcAudioPlaybackStatus */
    int position_present;
    uint32_t position_ms;
    int length_present;
    uint32_t length_ms;
} GcAudioPlaybackInfo;

typedef struct {
    char id[GC_AUDIO_SOURCE_ID_MAX];
    GcAudioSourceScope scope; /* test_tone, app_owned, or authorized_external */
    uint32_t implemented;     /* GC_AUDIO_KEY_* bits this source actually handles */
    /* 0 = applied, nonzero = refused. NULL is allowed only when implemented is 0. */
    int (*on_key)(void *ctx, uint32_t key_bit, uint64_t source_generation);
    void *ctx;
} GcAudioPlaybackSource;

typedef struct {
    GcAudioError error;
    int offered;   /* 1 only if an authorized implementing source was called */
    int consumed;  /* 1 only if that source returned applied */
    char affects[GC_AUDIO_SOURCE_ID_MAX]; /* that source id, else empty */
    uint8_t game_playback;       /* always 0 */
    uint8_t system_soundtrack;   /* always 0 */
} GcAudioKeyResult;

typedef struct {
    uint8_t ordinary_host_implemented; /* always 1 */
    uint8_t service_wired;             /* always 0 */
    uint8_t target_verified;           /* always 0 */
    uint8_t radio_opened;              /* always 0: this module never opens one */
    uint8_t pcm_capture;               /* always 0 */
    uint8_t microphone;                /* always 0 */
    uint8_t call_audio;                /* always 0 */
    uint8_t hfp_audio;                 /* always 0 */
    uint8_t game_playback;             /* always 0 */
    uint8_t system_soundtrack;         /* always 0 */
    uint8_t le_audio;                  /* always 0 */
    uint8_t shut_down;
    uint8_t connected;
    char address[GC_AUDIO_ADDRESS_TEXT];

    uint8_t requested_known;
    uint8_t requested;                 /* meaningful only if requested_known */
    uint8_t remote_known;
    uint8_t remote_volume;
    GcAudioBatteryFreshness remote_freshness; /* unknown if !remote_known */
    uint8_t software_gain_known;
    int16_t software_gain_tenths;      /* meaningful only if software_gain_known */
    uint8_t headset_acknowledged;
    GcAudioVolumePath volume_path;
    char fallback_reason[48];
    uint8_t volume_negotiated;         /* capability supported AND a command response */
    GcAudioAbsoluteVolume absolute_volume;
    uint64_t outstanding_request;

    uint8_t notifications_registered;
    uint64_t registration;
    uint32_t registered_mask;
    uint32_t reregister_owed;
    uint64_t registrations;
    uint64_t reregistrations;

    uint8_t playback_status_known;
    GcAudioPlaybackStatus playback_status; /* headset-reported; not a game claim */
    int64_t position_ms;               /* -1 unknown, including static */
    int64_t length_ms;                 /* -1 unknown; 0 is not a known length */
    char playback_source[GC_AUDIO_SOURCE_ID_MAX];
    uint32_t playback_implemented;
    uint8_t source_key_applied;
    char last_key_source[GC_AUDIO_SOURCE_ID_MAX];

    uint8_t battery_known;
    GcAudioBatterySource battery_source;
    GcAudioBatteryGranularity battery_granularity;
    uint64_t battery_generation;           /* 0 = no accepted report */
    uint64_t battery_freshness_generation; /* bumps on fresh and on stale */
    GcAudioBatteryFreshness battery_freshness;
    int coarse;                        /* -1 unknown; never a percent */
    int percent;                       /* -1 unknown; 0..100 only when known */
    int step;                          /* -1 unknown */
    int step_count;                    /* 0 unknown */
    uint8_t hfp_battery_only;          /* link noted; still not mic/call audio */
    char battery_address[GC_AUDIO_ADDRESS_TEXT];

    uint64_t echoes_rejected;
    uint64_t stale_rejected;
    GcAudioError last_error;
} GcAudioControlsSnapshot;

typedef struct {
    uint8_t ordinary_host_implemented;
    uint8_t service_wired;
    uint8_t target_verified;
    uint8_t radio_opened;
    uint8_t pcm_capture;
    uint8_t microphone;
    uint8_t call_audio;
    uint8_t hfp_audio;
    uint8_t game_playback;
    uint8_t system_soundtrack;
    uint8_t le_audio;
    uint8_t channel_configured; /* seam was complete at init; not authorization */
    uint8_t hfp_battery_only;
    uint8_t notifications_registered;
    GcAudioAbsoluteVolume absolute_volume;
    uint8_t shut_down;
} GcAudioControlsCapabilities;

/* Private. Read it through snapshot() / capabilities(). */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t quiet;
    unsigned in_flight;
    int shut_down;
    GcAudioControlChannel channel;
    int has_channel;
    int connected;
    char address[GC_AUDIO_ADDRESS_TEXT];
    uint64_t link_epoch;

    uint8_t requested_known;
    uint8_t requested;
    uint8_t remote_known;
    uint8_t remote_volume;
    GcAudioBatteryFreshness remote_freshness;
    char remote_address[GC_AUDIO_ADDRESS_TEXT];
    uint8_t software_gain_known;
    int16_t software_gain_tenths;
    uint8_t headset_acknowledged;
    GcAudioVolumePath volume_path;
    char fallback_reason[48];
    uint8_t volume_negotiated;
    GcAudioAbsoluteVolume absolute_volume;
    uint64_t next_request_id;
    uint64_t outstanding_id;
    uint64_t outstanding_epoch;
    uint8_t outstanding_volume;
    uint64_t last_completed_id;
    uint64_t response_accepted_id;

    uint64_t next_registration;
    uint64_t registration;
    int registration_active;
    uint32_t registered_mask;
    uint32_t interim_seen;
    uint32_t consumed_mask;
    uint64_t registrations;
    uint64_t reregistrations;

    uint8_t playback_status_known;
    GcAudioPlaybackStatus playback_status;
    uint8_t position_have_sample;
    uint32_t position_sample;
    int64_t position_ms;
    int64_t length_ms;
    GcAudioPlaybackSource source;
    int has_source;
    uint64_t source_generation;
    uint8_t source_key_applied;
    char last_key_source[GC_AUDIO_SOURCE_ID_MAX];

    uint8_t battery_known;
    GcAudioBatterySource battery_source;
    GcAudioBatteryGranularity battery_granularity;
    uint64_t battery_generation;
    uint64_t battery_freshness_generation;
    GcAudioBatteryFreshness battery_freshness;
    int coarse;
    int percent;
    int step;
    int step_count;
    char battery_address[GC_AUDIO_ADDRESS_TEXT];
    int hfp_noted;
    char hfp_address[GC_AUDIO_ADDRESS_TEXT];

    uint64_t echoes_rejected;
    uint64_t stale_rejected;
    GcAudioError last_error;
} GcAudioControls;

int gc_audio_controls_init(GcAudioControls *controls, const GcAudioControlChannel *channel);
void gc_audio_controls_shutdown(GcAudioControls *controls);
void gc_audio_controls_destroy(GcAudioControls *controls);

/* Connection is an owner report. It does not open a radio and does not
 * refresh a stale battery or remote volume. A second address is busy
 * until disconnect. Disconnect retires registration and any outstanding
 * volume request; a retained battery or remote volume becomes stale. */
GcAudioError gc_audio_controls_on_connect(GcAudioControls *controls, const char *address);
GcAudioError gc_audio_controls_on_disconnect(GcAudioControls *controls, const char *address);

/* Owner report after a feature exchange this module did not perform.
 * Unknown stays unknown until this is called. Switching to unsupported
 * retires an outstanding request so a late response cannot acknowledge. */
GcAudioError gc_audio_controls_on_volume_capability(GcAudioControls *controls, const char *address,
                                                    int supported);

GcAudioVolumeResult gc_audio_controls_set_requested_volume(GcAudioControls *controls, int volume);
/* Local applied gain only. Does not change requested or remote volume and
 * does not call the seam. */
GcAudioError gc_audio_controls_set_software_gain(GcAudioControls *controls, int gain_tenths);
/* Copies settings->volume into the requested field only. Does not send,
 * does not copy gain, and does not acknowledge a headset. */
GcAudioError gc_audio_controls_note_saved_volume(GcAudioControls *controls,
                                                 const GcAudioHeadsetSettings *settings);

/* Headset command response for one of our request ids. A local echo, a
 * second response, a mismatched id, or a response after disconnect is
 * rejected and changes no volume field. */
GcAudioError gc_audio_controls_on_volume_response(GcAudioControls *controls, const char *address,
                                                  uint64_t request_id, int volume, int origin);

GcAudioError gc_audio_controls_register_notifications(GcAudioControls *controls, uint32_t mask);
GcAudioError gc_audio_controls_unregister_notifications(GcAudioControls *controls);

GcAudioError gc_audio_controls_on_volume_notification(GcAudioControls *controls,
                                                      const GcAudioControlMeta *meta, int volume);
GcAudioError gc_audio_controls_on_playback(GcAudioControls *controls, const GcAudioControlMeta *meta,
                                           const GcAudioPlaybackInfo *info);

GcAudioError gc_audio_controls_authorize_source(GcAudioControls *controls,
                                                const GcAudioPlaybackSource *source);
GcAudioError gc_audio_controls_revoke_source(GcAudioControls *controls);
GcAudioKeyResult gc_audio_controls_on_playback_key(GcAudioControls *controls, uint32_t key_bit);

/* Notes an HFP link that is battery-only. Does not create a battery value
 * and does not advertise microphone or call audio. */
GcAudioError gc_audio_controls_note_hfp_battery_only(GcAudioControls *controls, const char *address);
GcAudioError gc_audio_controls_on_avrcp_battery(GcAudioControls *controls,
                                                const GcAudioControlMeta *meta, int coarse);
GcAudioError gc_audio_controls_on_hfp_percent(GcAudioControls *controls, const char *address,
                                              int percent, int origin);
GcAudioError gc_audio_controls_on_hfp_stepped(GcAudioControls *controls, const char *address,
                                              int step, int step_count, int origin);

void gc_audio_controls_snapshot(const GcAudioControls *controls, GcAudioControlsSnapshot *out);
void gc_audio_controls_capabilities(const GcAudioControls *controls, GcAudioControlsCapabilities *out);

/* --- AVRCP vendor-dependent frame, byte in / byte out ----------------------
 *
 * Wire layout (AVCTP 1.4 header, then an AV/C frame carrying a vendor-dependent
 * PDU), 13 bytes of header before the parameters:
 *
 *   0        label[7:4] | packet_type[3:2] | C/R[1] | IPID[0]
 *   1..2     profile id, 0x110E (AVRCP)
 *   3        ctype[3:0] | 0 (the high nibble is reserved zero: CONTROL 0x00,
 *            NOTIFY 0x03, CHANGED 0x0D, INTERIM 0x0F)
 *   4        subunit_type[7:3] | subunit_id[2:0]; PANEL = 0x48
 *   5        opcode, 0x00 = vendor dependent
 *   6..8     company id, 0x001958 (Bluetooth SIG)
 *   9        AVRCP PDU id
 *   10       packet type, 0 = single
 *   11..12   parameter length, big endian
 *   13..     parameters
 *
 * The builders write exactly one unfragmented frame and refuse a label outside
 * 0..15, an event this module does not report, or a volume outside 0..127 with
 * invalid_argument; nothing is clamped and a short buffer is written to not at
 * all. `out_len` is required.
 */
#define GC_AUDIO_AVCTP_PID 0x110Eu
#define GC_AUDIO_AVRCP_HEADER 13u
#define GC_AUDIO_AVRCP_SUBUNIT_PANEL 0x48u
#define GC_AUDIO_AVRCP_OPCODE_VENDOR 0x00u
#define GC_AUDIO_AVRCP_COMPANY_LO 0x1958u /* bytes 00 19 58 */
#define GC_AUDIO_AVRCP_CTYPE_CONTROL 0x00u
#define GC_AUDIO_AVRCP_CTYPE_NOTIFY 0x03u
#define GC_AUDIO_AVRCP_CTYPE_ACCEPTED 0x09u
#define GC_AUDIO_AVRCP_CTYPE_REJECTED 0x0Au
#define GC_AUDIO_AVRCP_CTYPE_STABLE 0x0Cu
#define GC_AUDIO_AVRCP_CTYPE_CHANGED 0x0Du
#define GC_AUDIO_AVRCP_CTYPE_INTERIM 0x0Fu
#define GC_AUDIO_AVRCP_PDU_REGISTER_NOTIFICATION 0x31u
#define GC_AUDIO_AVRCP_PDU_SET_ABSOLUTE_VOLUME 0x50u
#define GC_AUDIO_AVRCP_EVENT_BATT_STATUS 0x06u
#define GC_AUDIO_AVRCP_EVENT_VOLUME_CHANGED 0x0Du
#define GC_AUDIO_AVRCP_FRAME_MAX 24u

/* REGISTER_NOTIFICATION for one event: event octet then a 4-byte value that
 * is 0 for a registration. 18 bytes. */
GcAudioError gc_audio_controls_build_notification_registration(uint8_t *out, size_t cap,
                                                              uint8_t label, int event,
                                                              size_t *out_len);
/* SET_ABSOLUTE_VOLUME, CONTROL ctype, one parameter. 14 bytes. */
GcAudioError gc_audio_controls_build_absolute_volume(uint8_t *out, size_t cap, uint8_t label,
                                                     int volume, size_t *out_len);
/* The CHANGED answer to the headset's pending registration. 15 bytes. */
GcAudioError gc_audio_controls_build_volume_changed(uint8_t *out, size_t cap, uint8_t label,
                                                    int volume, size_t *out_len);

typedef struct {
    GcAudioError error;  /* ok; negotiation_rejected for a frame this box will not read */
    uint8_t label;
    int response;        /* 1 when the C/R bit marks a response */
    int ipid;
    uint8_t ctype;       /* AV/C ctype */
    uint8_t pdu;         /* AVRCP PDU id */
    int event;           /* REGISTER_NOTIFICATION event, -1 when the PDU carries none */
    int value;           /* that event's parameter, -1 when absent */
    int volume;          /* 0..127 when this frame reports a volume, else -1 */
    size_t param_len;    /* parameters present, already matched to the frame */
} GcAudioAvrcpFrame;

/* One complete AVCTP frame. Refuses (negotiation_rejected) a fragmented frame
 * (packet type other than single - this box does not reassemble), a foreign
 * profile id, a reserved bit set, a non-vendor opcode, a foreign company id, a
 * packet-type octet that is not single, and a parameter length that does not
 * equal the bytes present. A well-formed frame with a PDU this module does not
 * interpret is still ok: `pdu` is reported and `event`/`value`/`volume` stay
 * -1, so a caller never mistakes silence for support. */
GcAudioError gc_audio_controls_parse_avctp(const uint8_t *in, size_t len, GcAudioAvrcpFrame *out);

/* Always 0. There is no setter. HFP battery-only is not these. */
int gc_audio_controls_microphone_available(const GcAudioControls *controls);
int gc_audio_controls_call_audio_available(const GcAudioControls *controls);
int gc_audio_controls_hfp_audio_available(const GcAudioControls *controls);
int gc_audio_controls_game_playback_available(const GcAudioControls *controls);

const char *gc_audio_controls_volume_path_name(GcAudioVolumePath path);
const char *gc_audio_controls_battery_source_name(GcAudioBatterySource source);
const char *gc_audio_controls_granularity_name(GcAudioBatteryGranularity granularity);
const char *gc_audio_controls_freshness_name(GcAudioBatteryFreshness freshness);
const char *gc_audio_controls_coarse_name(int coarse);

#ifdef __cplusplus
}
#endif

#endif

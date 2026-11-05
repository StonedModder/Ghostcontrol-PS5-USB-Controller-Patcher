/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-02: AVDTP signaling gates and RTP/SBC media framing.
 *
 * Unwired ordinary HOST module. It decides which Source/Sink capability
 * intersection is legal, which AVDTP signal may be sent in which state, and
 * how already-encoded SBC frames become one unfragmented RTP packet. It does
 * not open a radio, an ACL link, or an L2CAP channel. It does not encode or
 * decode SBC (AUDIO-03), capture PCM, or talk to IPC/UI.
 *
 * The owner delivers one complete signal SDU and reports media-channel
 * readiness. This module does not reassemble AVDTP fragments and does not
 * invent a transport. A comparison note named the lenient behaviours this
 * box refuses: streaming without an accepted START, acknowledging suspend
 * without leaving streaming, synthesizing a codec bit the peer did not
 * advertise, swapping reversed bitpool bounds, clamping a bad frame count,
 * and treating an MTU that cannot hold one frame as success. No submitted
 * implementation was copied.
 *
 * Streaming is not a separate flag. It is exactly the STREAMING state, and
 * that state is entered only when a START accept is applied — ours, after
 * the remote accept PDU, or a remote START command that this module accepts.
 * A reject, a general reject, a timeout, or a malformed accept leaves the
 * pre-start state and does not packetize.
 */
#ifndef GC_AUDIO_AVDTP_H
#define GC_AUDIO_AVDTP_H

#include "gc_audio.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AVDTP 1.3 signal identifiers. */
#define GC_AVDTP_DISCOVER              0x01u
#define GC_AVDTP_GET_CAPABILITIES      0x02u
#define GC_AVDTP_SET_CONFIGURATION     0x03u
#define GC_AVDTP_GET_CONFIGURATION     0x04u
#define GC_AVDTP_RECONFIGURE           0x05u
#define GC_AVDTP_OPEN                  0x06u
#define GC_AVDTP_START                 0x07u
#define GC_AVDTP_CLOSE                 0x08u
#define GC_AVDTP_SUSPEND               0x09u
#define GC_AVDTP_ABORT                 0x0Au
#define GC_AVDTP_SECURITY_CONTROL      0x0Bu
#define GC_AVDTP_GET_ALL_CAPABILITIES  0x0Cu
#define GC_AVDTP_DELAYREPORT           0x0Du

#define GC_AVDTP_MSG_COMMAND           0u
#define GC_AVDTP_MSG_GENERAL_REJECT    1u
#define GC_AVDTP_MSG_ACCEPT            2u
#define GC_AVDTP_MSG_REJECT            3u

#define GC_AVDTP_ERR_BAD_HEADER_FORMAT 0x01u
#define GC_AVDTP_ERR_BAD_LENGTH        0x11u
#define GC_AVDTP_ERR_BAD_ACP_SEID      0x12u
#define GC_AVDTP_ERR_BAD_STATE         0x31u
#define GC_AVDTP_ERR_NOT_SUPPORTED     0x19u

#define GC_AVDTP_CAT_MEDIA_TRANSPORT    0x01u
#define GC_AVDTP_CAT_REPORTING          0x02u
#define GC_AVDTP_CAT_RECOVERY           0x03u
#define GC_AVDTP_CAT_CONTENT_PROTECTION 0x04u
#define GC_AVDTP_CAT_HEADER_COMPRESSION 0x05u
#define GC_AVDTP_CAT_MULTIPLEXING       0x06u
#define GC_AVDTP_CAT_MEDIA_CODEC        0x07u
#define GC_AVDTP_CAT_DELAY_REPORTING    0x08u

/* A2DP SBC capability bits (codec information element). */
#define GC_AVDTP_SBC_FREQ_16K  0x80u
#define GC_AVDTP_SBC_FREQ_32K  0x40u
#define GC_AVDTP_SBC_FREQ_44K  0x20u
#define GC_AVDTP_SBC_FREQ_48K  0x10u
#define GC_AVDTP_SBC_CH_MONO   0x08u
#define GC_AVDTP_SBC_CH_DUAL   0x04u
#define GC_AVDTP_SBC_CH_STEREO 0x02u
#define GC_AVDTP_SBC_CH_JOINT  0x01u
#define GC_AVDTP_SBC_BLK_16    0x80u
#define GC_AVDTP_SBC_BLK_12    0x40u
#define GC_AVDTP_SBC_BLK_8     0x20u
#define GC_AVDTP_SBC_BLK_4     0x10u
#define GC_AVDTP_SBC_SB_8      0x08u
#define GC_AVDTP_SBC_SB_4      0x04u
#define GC_AVDTP_SBC_ALLOC_LOUDNESS 0x02u
#define GC_AVDTP_SBC_ALLOC_SNR      0x01u

#define GC_AVDTP_RTP_HEADER       12u
#define GC_AVDTP_SBC_MEDIA_HEADER 1u
#define GC_AVDTP_MEDIA_OVERHEAD   13u
#define GC_AVDTP_L2CAP_BASIC      4u
#define GC_AVDTP_RTP_PAYLOAD_TYPE 96u

/* Frame-header channel mode. Matches the SBC frame, not the capability bit. */
#define GC_AVDTP_MODE_MONO  0u
#define GC_AVDTP_MODE_DUAL  1u
#define GC_AVDTP_MODE_STEREO 2u
#define GC_AVDTP_MODE_JOINT 3u
#define GC_AVDTP_ALLOC_LOUDNESS 0u
#define GC_AVDTP_ALLOC_SNR      1u

typedef enum {
    GC_AVDTP_IDLE = 0,
    GC_AVDTP_DISCOVERING,
    GC_AVDTP_DISCOVERED,
    GC_AVDTP_AWAITING_CAPS,
    GC_AVDTP_CAPABILITIES,
    GC_AVDTP_CONFIGURING,
    GC_AVDTP_CONFIGURED,
    GC_AVDTP_OPENING,
    GC_AVDTP_OPENED,
    GC_AVDTP_MEDIA_READY,
    GC_AVDTP_STARTING,
    GC_AVDTP_STREAMING,
    GC_AVDTP_SUSPENDING,
    GC_AVDTP_SUSPENDED,
    GC_AVDTP_CLOSING,
    GC_AVDTP_ABORTING
} GcAvdtpState;

/* One side's SBC capability. Masks use the A2DP bit positions above.
 * An empty mask, a bit outside the field, or min > max is malformed.
 * This module does not swap reversed bounds or fill in a missing bit. */
typedef struct {
    uint8_t frequency_mask;
    uint8_t channel_mask;
    uint8_t block_mask;
    uint8_t subband_mask;
    uint8_t allocation_mask;
    uint8_t bitpool_min;
    uint8_t bitpool_max;
} GcAvdtpSbcCaps;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;       /* 1 or 2 */
    uint8_t channel_mode;   /* GC_AVDTP_MODE_* */
    uint8_t blocks;         /* 4, 8, 12, 16 */
    uint8_t subbands;       /* 4 or 8 */
    uint8_t allocation;     /* GC_AVDTP_ALLOC_* */
    uint8_t bitpool_min;
    uint8_t bitpool_max;
    uint8_t frequency_bit;
    uint8_t channel_bit;
    uint8_t block_bit;
    uint8_t subband_bit;
    uint8_t allocation_bit;
} GcAvdtpSbcConfig;

typedef struct {
    GcAudioError error;
    size_t length;          /* 0 unless error is GC_AUDIO_OK */
    uint8_t label;
    uint8_t signal;
} GcAvdtpCommand;

typedef struct {
    GcAudioError error;
    size_t reply_len;
    int accepted;           /* remote command accepted */
    int stale;              /* response ignored; outstanding left alone */
    int applied;            /* our outstanding response was consumed */
} GcAvdtpFeed;

typedef struct {
    GcAudioError error;
    size_t length;
    uint16_t sequence;      /* sequence written, if accepted */
    uint32_t timestamp;     /* timestamp written, if accepted */
    uint32_t timestamp_delta; /* samples per channel, not milliseconds */
    uint8_t frame_count;
} GcAvdtpPacket;

typedef struct {
    int ordinary_host_implemented; /* always 1 */
    int service_wired;             /* always 0 */
    int encoder_available;         /* always 0: AUDIO-03 is not this module */
    int radio_opened;              /* always 0 */
    int target_verified;           /* always 0 */
    int microphone;                /* always 0 */
    int le_audio;                  /* always 0 */
    int game_capture;              /* always 0 */
    int shut_down;
    int bound;
    int streaming;                 /* 1 only in GC_AVDTP_STREAMING */
    int media_ready;
    int selection_valid;
    int caps_fallback;             /* GET_ALL was unsupported; next query is GET_CAPABILITIES */
    int late_hold;                 /* owner must drop_late before the next command */
    int suspend_unconfirmed;
    int close_unconfirmed;
    int sink_delay_reported;
    int sink_delay_tenths;         /* -1 if the sink has not reported */
    int queue_delay_ms;            /* always -1: this module has no queue */
    int estimate_ms;               /* always -1: no fixed capture/headset estimate */
    GcAvdtpState state;
    uint64_t session_generation;
    uint8_t outstanding;
    uint8_t outstanding_label;
    uint8_t outstanding_signal;
    uint8_t acp_seid;
    uint8_t int_seid;
    uint8_t labels_retired;        /* count of labels that will not be reused yet */
    uint16_t l2cap_mtu;
    uint16_t acl_payload_max;
    uint16_t sequence_next;
    uint32_t timestamp_next;
    uint32_t stale_responses;
    uint32_t rejected_packets;
    uint32_t accepted_packets;
    uint32_t unsent_dropped_samples;
    uint32_t accepted_then_dropped_packets;
    uint32_t malformed_pdus;
    char address[GC_AUDIO_ADDRESS_TEXT];
    char reason[48];
    GcAvdtpSbcConfig selected;
    /* Fixed-bitpool retry after a refused range (see
     * gc_audio_avdtp_configure_fixed). */
    int config_refused;     /* the last SET_CONFIGURATION was refused */
    int fixed_bitpool_used; /* 0 or the fixed value the peer accepted */
    uint32_t configure_retries;
} GcAudioAvdtpSnapshot;

typedef struct GcAudioAvdtp {
    int shut_down;
    int bound;
    int late_hold;
    int outstanding;
    int selection_valid;
    int caps_fallback;
    int caps_query_all;
    int media_ready_flag;
    int suspend_unconfirmed;
    int close_unconfirmed;
    int sink_delay_reported;
    int sink_delay_tenths;
    GcAvdtpState state;
    GcAvdtpState resume_state;
    uint64_t session_generation;
    uint64_t epoch;
    uint64_t out_epoch;
    uint16_t retired;
    uint16_t l2cap_mtu;
    uint16_t acl_payload_max;
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    uint32_t stale_responses;
    uint32_t rejected_packets;
    uint32_t accepted_packets;
    uint32_t unsent_dropped_samples;
    uint32_t accepted_then_dropped_packets;
    uint32_t malformed_pdus;
    uint8_t next_label;
    uint8_t out_label;
    uint8_t out_signal;
    uint8_t acp_seid;
    uint8_t int_seid;
    char address[GC_AUDIO_ADDRESS_TEXT];
    char reason[48];
    GcAvdtpSbcCaps local;
    GcAvdtpSbcConfig selected;
    int config_refused;
    int config_fixed_used;
    int fixed_pending;   /* the fixed bitpool this retry asked for, 0 = none */
    int fixed_bitpool;   /* the fixed bitpool the peer accepted, 0 = none */
    uint32_t configure_retries;
} GcAudioAvdtp;

/* Caller-owned. No heap, no lock: this module never calls out. Not re-entrant. */
void gc_audio_avdtp_init(GcAudioAvdtp *session);
void gc_audio_avdtp_shutdown(GcAudioAvdtp *session);

/* local_seid is our Source SEID, 0x01..0x3F. ssrc/sequence/timestamp are the
 * RTP session clock the owner wants to start from. A bad address is
 * invalid_identity and does not bind. */
GcAudioError gc_audio_avdtp_bind(GcAudioAvdtp *session, const char *address,
                                 uint8_t local_seid, const GcAvdtpSbcCaps *local,
                                 uint32_t ssrc, uint16_t sequence, uint32_t timestamp);

const char *gc_audio_avdtp_state_name(GcAvdtpState state);

/* 0 valid. -1 malformed (empty field, stray bit, bitpool out of 2..250 or
 * min > max). Does not rewrite *caps. */
int gc_audio_avdtp_caps_valid(const GcAvdtpSbcCaps *caps, char *reason, size_t reason_cap);

/* 0 and *out set. -1 malformed. -2 both sides valid but a field or the
 * bitpool range does not overlap. Preference among overlapping bits is
 * 48k, 44.1k, 32k, 16k; joint, stereo, dual, mono; 16/12/8/4 blocks;
 * 8 then 4 subbands; loudness then SNR. A bit neither side set is never
 * added. */
int gc_audio_avdtp_intersect(const GcAvdtpSbcCaps *local, const GcAvdtpSbcCaps *remote,
                             GcAvdtpSbcConfig *out, char *reason, size_t reason_cap);

/* Parse a GET_CAPABILITIES / GET_ALL_CAPABILITIES accept body (no signal
 * header). 0 ok, -1 malformed, -2 non-SBC codec, -3 category this Source
 * will not pretend to support (content protection, recovery, multiplexing,
 * header compression). Unknown categories with a fitting length are skipped.
 * Missing media transport or media codec is malformed. */
int gc_audio_avdtp_parse_caps(const uint8_t *bytes, size_t len, GcAvdtpSbcCaps *out,
                              int *delay_reporting, char *reason, size_t reason_cap);

/* On-wire SBC frame size in bytes, including the padding byte. This is the
 * ceiling of the bit packing, not the truncating division written in the
 * A2DP bit-budget formula. -1 on a bad parameter. Not an encoder. */
int gc_audio_avdtp_sbc_frame_bytes(uint8_t channel_mode, uint8_t blocks, uint8_t subbands,
                                   uint8_t bitpool, uint32_t *out);

/* Each command writes one single-packet AVDTP command or nothing.
 * Failure leaves the buffer untouched and sets no outstanding label.
 * A label consumed by success, reject, or timeout is not reused until
 * gc_audio_avdtp_drop_late. After abort, close, disconnect, or a hard
 * negotiation failure, late_hold refuses new commands until drop_late. */
GcAvdtpCommand gc_audio_avdtp_discover(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
GcAvdtpCommand gc_audio_avdtp_query_capabilities(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
GcAvdtpCommand gc_audio_avdtp_configure(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
/* Retry SET_CONFIGURATION with one fixed bitpool (bitpool_min == bitpool_max ==
 * bitpool) after the peer refused the range form. Some sinks accept only a
 * single bitpool value; the sibling A2DP implementation retries exactly once
 * for that reason (HB src/a2dp/avdtp.c:657-664) and this is that behaviour in
 * this tree's shapes: the caller drives it, only after a refusal, at most once
 * per negotiation, and the narrowed window becomes the session's window only
 * when the peer accepts. `bitpool` must lie inside the negotiated window
 * (otherwise negotiation_rejected), the refusal must be the one this session
 * saw (otherwise profile_unavailable), and a retry already used is
 * profile_unavailable with `fixed_bitpool_used`. */
GcAvdtpCommand gc_audio_avdtp_configure_fixed(GcAudioAvdtp *session, uint8_t *pdu, size_t cap,
                                             uint8_t bitpool);
GcAvdtpCommand gc_audio_avdtp_open(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
/* Owner observation that the media L2CAP channel is up. Does not open one.
 * Refuses unless both the peer L2CAP MTU and the controller ACL payload
 * (which must also hold a 4-byte L2CAP basic header) can carry one minimum
 * SBC frame plus the 13-byte RTP/SBC overhead. */
GcAudioError gc_audio_avdtp_media_up(GcAudioAvdtp *session, uint16_t l2cap_mtu,
                                     uint16_t acl_payload_max);
GcAudioError gc_audio_avdtp_media_lost(GcAudioAvdtp *session);
GcAvdtpCommand gc_audio_avdtp_start(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
GcAvdtpCommand gc_audio_avdtp_suspend(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
GcAvdtpCommand gc_audio_avdtp_close(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
GcAvdtpCommand gc_audio_avdtp_abort(GcAudioAvdtp *session, uint8_t *pdu, size_t cap);
/* Owner saw the link drop. No PDU. Leaves streaming, bumps the generation. */
GcAudioError gc_audio_avdtp_disconnect(GcAudioAvdtp *session);
/* Owner has discarded any late response for retired labels. Refuses while a
 * command is still outstanding. */
GcAudioError gc_audio_avdtp_drop_late(GcAudioAvdtp *session);
/* Retire the matching outstanding label. START timeout does not stream.
 * A later accept for that label is stale. */
GcAudioError gc_audio_avdtp_timeout(GcAudioAvdtp *session, uint8_t label);

/* One complete signal SDU from the owner. Remote commands get a reply in
 * `reply` when the buffer can hold it; a short buffer changes nothing.
 * A response is applied only when its label and signal match the outstanding
 * command. Anything else is stale and does not move the state. */
GcAvdtpFeed gc_audio_avdtp_feed(GcAudioAvdtp *session, const uint8_t *pdu, size_t len,
                                uint8_t *reply, size_t reply_cap);

/* Writes one RTP packet (12-byte header, one SBC count byte, then the frames)
 * only in STREAMING. Rejects count outside 1..15, a length that is not the
 * sum of that many complete frames, a frame whose header or bitpool disagrees
 * with the negotiated config, an MTU that cannot hold the packet, and an ACL
 * envelope that cannot hold the L2CAP header plus that packet. Does not clamp
 * the count. Failure writes nothing and does not move sequence or timestamp.
 * Timestamp advances by frame_count * blocks * subbands (samples per channel). */
GcAvdtpPacket gc_audio_avdtp_packetize(GcAudioAvdtp *session, const uint8_t *frames,
                                       size_t frames_len, uint8_t frame_count,
                                       uint8_t *out, size_t out_cap);

/* Pre-packet drop: advances the sample clock, not the sequence. samples must
 * be a positive multiple of one frame. Refuses unless streaming. */
GcAudioError gc_audio_avdtp_note_unsent_drop(GcAudioAvdtp *session, uint32_t samples);

/* Accepted-then-dropped accounting. Does not move sequence or timestamp again
 * and cannot exceed packets this session has actually accepted. */
GcAudioError gc_audio_avdtp_note_accepted_drop(GcAudioAvdtp *session, uint16_t packets,
                                               uint32_t samples);

void gc_audio_avdtp_snapshot(const GcAudioAvdtp *session, GcAudioAvdtpSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif

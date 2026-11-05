/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-03: SBC encoder, sample-clock pacer, and one credit owner.
 *
 * Unwired ordinary HOST module. It encodes PCM into one SBC frame for a
 * configuration the AUDIO-02 framing module can select, paces media by an
 * exact sample count, and schedules signaling, HID, and media against one
 * injected credit budget. It does not open a radio, an ACL link, or an
 * L2CAP channel, and it does not capture PCM or talk to IPC/UI.
 *
 * The encoder follows Bluetooth A2DP v1.4 Appendix B (analysis Figure 8.5,
 * scale factors B.7.2, joint stereo B.7.3, bit allocation B.6.3,
 * quantization B.7.5, syntax B.4). No submitted encoder was imported.
 * Frame length is the ceiling returned by gc_audio_avdtp_sbc_frame_bytes.
 * A bitpool outside the negotiated window is rejected, not clamped.
 *
 * Credits and latency are reports. This module does not estimate either.
 * An unknown estimate stays -1. The sample clock moves only by a positive
 * sample count; a millisecond delta is not accepted and is not accumulated.
 * Queue overflow drops media before signaling. A signaling slot is never
 * the victim.
 */
#ifndef GC_AUDIO_CODEC_H
#define GC_AUDIO_CODEC_H

#include "gc_audio_avdtp.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_AUDIO_CODEC_SIG_MAX 4
#define GC_AUDIO_CODEC_HID_MAX 4
#define GC_AUDIO_CODEC_MEDIA_MAX 4
#define GC_AUDIO_CODEC_SIG_SLOT 32
#define GC_AUDIO_CODEC_HID_SLOT 64
#define GC_AUDIO_CODEC_FRAME_MAX 544

typedef enum {
    GC_AUDIO_KIND_NONE = -1,
    GC_AUDIO_KIND_SIGNALING = 0,
    GC_AUDIO_KIND_HID = 1,
    GC_AUDIO_KIND_MEDIA = 2
} GcAudioQueueKind;

typedef struct {
    GcAudioError error;
    int dropped_media;
    uint32_t dropped_samples;
} GcAudioCodecEnqueue;

typedef struct {
    GcAudioError error;
    size_t length;
    int kind; /* GC_AUDIO_KIND_* ; NONE when nothing was sent */
    uint32_t samples;
} GcAudioCodecPull;

typedef struct {
    int ordinary_host_implemented; /* always 1 */
    int service_wired;             /* always 0 */
    int radio_opened;              /* always 0: this module does not open one */
    int encoder_available;         /* 1 only after a successful configure */
    int target_verified;           /* always 0 */
    int microphone;                /* always 0 */
    int le_audio;                  /* always 0 */
    int game_capture;              /* always 0 */
    int shut_down;
    int configured;
    int credits_known;             /* 1 only after an owner report */
    int reported_credits;          /* -1 until reported; a report, not an estimate */
    int available_credits;         /* -1 until reported; never an estimate */
    int estimate_credits;          /* always -1 */
    int inflight;
    int reported_latency_samples;  /* -1 until reported */
    int estimate_ms;               /* always -1: unknown stays unknown */
    int reported_delay_tenths;     /* -1 until reported; not a sample-clock input */
    int signaling_queued;
    int hid_queued;
    int media_queued;
    int signaling_cap;
    int hid_cap;
    int media_cap;
    int shared_cap;
    uint8_t bitpool;
    uint8_t bitpool_min;
    uint8_t bitpool_max;
    uint32_t frame_samples;        /* blocks * subbands, 0 if unconfigured */
    uint64_t release_samples;      /* sample clock not yet consumed by a sent frame */
    uint64_t queued_media_samples; /* occupancy, not a latency report */
    uint32_t media_dropped;
    uint32_t media_dropped_samples;
    uint32_t signaling_refused;
    uint32_t hid_refused;
    uint32_t sent_signaling;
    uint32_t sent_hid;
    uint32_t sent_media;
    uint32_t over_reported_completions;
    /* Adaptive bitpool: 0 until armed, then the band actually used (the
     * negotiated window intersected with the requested floor/ceiling). */
    int rate_active;
    int rate_lo;
    int rate_hi;
    int rate_downs;
    int rate_ups;
    int rate_refused;      /* a band value the strict window refused */
    int rate_held;         /* 1 while a dip holds the bitpool at the floor */
    int credits_stale;     /* link gone: reports are not evidence for a pull */
    int link_downs;
    char reason[48];
} GcAudioCodecSnapshot;

typedef struct {
    uint16_t len;
    uint8_t bytes[GC_AUDIO_CODEC_SIG_SLOT];
} GcAudioSigSlot;

typedef struct {
    uint16_t len;
    uint8_t bytes[GC_AUDIO_CODEC_HID_SLOT];
} GcAudioHidSlot;

typedef struct {
    uint16_t len;
    uint32_t samples;
    uint8_t bytes[GC_AUDIO_CODEC_FRAME_MAX];
} GcAudioMediaSlot;

typedef struct GcAudioCodec {
    int shut_down;
    int configured;
    int credits_known;
    uint16_t reported_credits;
    uint16_t inflight;
    int reported_latency_samples;
    int reported_delay_tenths;
    int sig_cap, hid_cap, media_cap, shared_cap;
    int sig_head, sig_count;
    int hid_head, hid_count;
    int media_head, media_count;
    uint8_t bitpool;
    uint32_t frame_samples;
    uint64_t release_samples;
    uint32_t media_dropped;
    uint32_t media_dropped_samples;
    uint32_t signaling_refused;
    uint32_t hid_refused;
    uint32_t sent_signaling;
    uint32_t sent_hid;
    uint32_t sent_media;
    uint32_t over_reported_completions;
    /* Private: adaptive bitpool clock and bands. Timed by the owner's
     * millisecond clock, which is never a sample-clock input. */
    int rate_active;
    int rate_lo, rate_hi, rate_cur, rate_cap;
    int rate_downs, rate_ups, rate_refused;
    int rate_hold;
    int64_t rate_t_down, rate_t_up, rate_t_calm, rate_t_cap, rate_t_late, rate_hold_until;
    uint32_t rate_last_drops;
    int credits_stale;
    int link_downs;
    char reason[48];
    GcAvdtpSbcConfig selected;
    double history[2][80];
    GcAudioSigSlot signaling[GC_AUDIO_CODEC_SIG_MAX];
    GcAudioHidSlot hid[GC_AUDIO_CODEC_HID_MAX];
    GcAudioMediaSlot media[GC_AUDIO_CODEC_MEDIA_MAX];
} GcAudioCodec;

/* Caller-owned. No heap, no lock, no radio. Not re-entrant. */
void gc_audio_codec_init(GcAudioCodec *codec);
void gc_audio_codec_shutdown(GcAudioCodec *codec);

/* `selected` is an AUDIO-02 intersection. `bitpool` must lie inside its
 * window and inside the appendix bit cap (16*subbands per mono/dual channel,
 * 32*subbands for stereo/joint). Outside the window is negotiation_rejected
 * and does not clamp. A window the framing module would not select, or a
 * bitpool that cannot be packed into the appendix frame, is unsupported_format.
 * Does not rewrite *selected. Drops queued media because those frames belong
 * to the previous configuration. Signaling and HID stay. Credits stay. */
GcAudioError gc_audio_codec_configure(GcAudioCodec *codec, const GcAvdtpSbcConfig *selected,
                                      uint8_t bitpool);

/* Same rejection rules. Failure leaves the previous bitpool in place. */
GcAudioError gc_audio_codec_set_bitpool(GcAudioCodec *codec, uint8_t bitpool);

/* Caps are 1..the compile-time maximum. shared_cap is 1..sig+hid+media.
 * A change that would have to discard signaling or HID is refused and
 * changes nothing. Media may be dropped to meet a tighter media or shared cap. */
GcAudioError gc_audio_codec_set_bounds(GcAudioCodec *codec, int signaling_cap, int hid_cap,
                                       int media_cap, int shared_cap);

/* One SBC frame. pcm is blocks*subbands frames per channel, interleaved s16.
 * Failure writes nothing and does not advance the analysis history. */
GcAudioError gc_audio_sbc_encode(GcAudioCodec *codec, const int16_t *pcm, uint8_t *out,
                                 size_t cap, size_t *out_len);

/* How many whole frames of the current bitpool fit in one unfragmented RTP
 * packet under the AUDIO-02 overhead (13-byte RTP/SBC header, 4-byte L2CAP
 * basic header, count at most 15). Does not change bitpool. Zero frames is
 * negotiation_rejected. */
GcAudioError gc_audio_codec_packet_room(const GcAudioCodec *codec, uint16_t l2cap_mtu,
                                        uint16_t acl_payload_max, uint32_t *frame_bytes,
                                        uint8_t *frames);

/* Owner report of the single credit budget. Not an estimate, and not a
 * request to open a radio. A later report replaces the budget. Completions
 * cannot raise available above the last report. */
GcAudioError gc_audio_codec_report_credits(GcAudioCodec *codec, uint16_t credits);
/* Restores inflight by the reported completion count. Excess is counted and
 * does not invent credits. Refuses when no budget has been reported. */
GcAudioError gc_audio_codec_report_completed(GcAudioCodec *codec, uint16_t packets);

/* The link is gone (owner report, e.g. a Disconnection Complete). The
 * controller frees every buffer that handle held, so every inflight packet
 * is retired in one step and the retired count is written to *retired when
 * it is non-NULL. The single reported budget is NOT replaced and NOT
 * refilled, and after this a pull is refused as radio_unavailable with
 * `credits_stale` until the owner reports the next link's budget: a dead
 * link's report is not evidence that buffers are free. Completions that
 * arrive after this cannot resurrect a credit either. Reworked from the
 * sibling A2DP implementation's acl_pool_disconnected() (HB
 * src/a2dp/acl_pool.c:69-76), which returns the outstanding count for the
 * same reason. */
GcAudioError gc_audio_codec_report_link_down(GcAudioCodec *codec, uint16_t *retired);

/* --- Adaptive bitpool -----------------------------------------------------
 *
 * Rejects, never clamps, and never leaves the negotiated window. The
 * controller only picks values inside
 *   [max(window.bitpool_min, floor), min(window.bitpool_max, ceiling, cap)]
 * where `cap` is the Appendix B.5.1 mode cap the strict setter already
 * enforces, and every pick goes through gc_audio_codec_set_bitpool. A value
 * the strict window refuses is counted in rate_refused and the previous
 * bitpool stays in force; the bands are then narrowed so it cannot recur.
 *
 * Step rules, reworked from the sibling implementation's rate controller
 * (HB src/a2dp/rate.h:39-49, rate.c:74-132), in this tree's terms:
 *   - down: a media drop since the last observation steps 4 down; media
 *     waiting more than GC_AUDIO_CODEC_RATE_SLACK packets, with at least half
 *     the media room late for 300 ms, steps 3 down and remembers `cur - 1` as
 *     a ceiling for 30 s; the same backlog lasting 600 ms steps 1 down.
 *     At most one step down per 300 ms.
 *   - up: after 4 s with no drop, no step down and no late packet, +2 per
 *     2 s while 8 or more below the top, then +1, never above the band top.
 *   - calm is restarted by a drop, a step down, a late packet or a hold.
 *
 * The clock is the owner's free-running millisecond count. It times the
 * adaptation only: it is not a sample clock, it does not advance
 * release_samples and it cannot make media sendable. */
#define GC_AUDIO_CODEC_RATE_FLOOR       22
#define GC_AUDIO_CODEC_RATE_SLACK        4
#define GC_AUDIO_CODEC_RATE_DOWN_MS    300
#define GC_AUDIO_CODEC_RATE_UP_CALM_MS 4000
#define GC_AUDIO_CODEC_RATE_UP_GAP_MS  2000
#define GC_AUDIO_CODEC_RATE_CAP_HOLD_MS 30000
#define GC_AUDIO_CODEC_RATE_LATE_CALM_MS 200
#define GC_AUDIO_CODEC_RATE_LATE_MS     600
#define GC_AUDIO_CODEC_RATE_LATE_BIG_MS 300

typedef struct {
    GcAudioError error;
    uint8_t bitpool;  /* the bitpool in force after this observation */
    int step;         /* signed steps applied by this call; 0 = unchanged */
    int refused;      /* 1 if this call's pick was refused by the window */
} GcAudioCodecRateTick;

/* Arms the controller for the current configuration. floor/ceiling are the
 * band this tree wants inside the negotiated window (2..250); a band that
 * does not overlap the window collapses to the window's own edge, it never
 * widens it. Refuses encoder_unavailable before a configure and
 * invalid_argument for floor > ceiling or a value outside 2..250. Arming
 * brings the bitpool into the band (counted as downs/ups), which is legal
 * because the band is inside the window. */
GcAudioError gc_audio_codec_rate_init(GcAudioCodec *codec, uint8_t floor, uint8_t ceiling,
                                      int64_t now_ms);

/* One observation. queue = media packets waiting, qmax = the media cap in
 * force, drops = the monotonic total media drop count (snapshot
 * .media_dropped). Refuses profile_unavailable when not armed or shut down,
 * invalid_argument for a negative or non-monotonic counter. */
GcAudioCodecRateTick gc_audio_codec_rate_update(GcAudioCodec *codec, int64_t now_ms, int queue,
                                                int qmax, uint32_t drops);

/* A dip: the floor immediately and until `until_ms`, no step up, and the
 * calm period starts when the hold ends. A hold in the past is a no-op. */
GcAudioError gc_audio_codec_rate_hold_floor(GcAudioCodec *codec, int64_t now_ms, int64_t until_ms);

/* Disarms the controller and leaves the bitpool where it is. */
GcAudioError gc_audio_codec_rate_stop(GcAudioCodec *codec);

/* A measured latency in samples. Does not write estimate_ms and does not
 * move the sample clock. -1 withdraws the report (unknown again). */
GcAudioError gc_audio_codec_report_latency_samples(GcAudioCodec *codec, int samples);
/* Sink delay in tenths of a millisecond, stored as a report. Not converted
 * into samples and not copied into estimate_ms. -1 withdraws it. */
GcAudioError gc_audio_codec_report_delay_tenths(GcAudioCodec *codec, int tenths);

/* Moves the release clock by an exact sample count. There is no millisecond
 * entry. Zero is rejected so a caller cannot treat a time tick as progress. */
GcAudioError gc_audio_codec_advance_samples(GcAudioCodec *codec, uint32_t samples);

GcAudioCodecEnqueue gc_audio_codec_enqueue_signaling(GcAudioCodec *codec, const uint8_t *bytes,
                                                     size_t len);
GcAudioCodecEnqueue gc_audio_codec_enqueue_hid(GcAudioCodec *codec, const uint8_t *bytes, size_t len);
/* Encodes one frame and queues it. Overflow drops oldest media, never signaling. */
GcAudioCodecEnqueue gc_audio_codec_enqueue_pcm(GcAudioCodec *codec, const int16_t *pcm);

/* Sends one packet if a reported credit is available. Order is signaling,
 * then HID, then media whose sample count the release clock already covers.
 * Does not estimate a credit. Does not send media early. */
GcAudioCodecPull gc_audio_codec_pull(GcAudioCodec *codec, uint8_t *out, size_t cap);

void gc_audio_codec_snapshot(const GcAudioCodec *codec, GcAudioCodecSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif

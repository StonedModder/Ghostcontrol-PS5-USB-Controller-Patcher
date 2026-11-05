/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-02 AVDTP gates and RTP/SBC packetizer. See gc_audio_avdtp.h.
 * No radio, no L2CAP open, no SBC encoder.
 */
#include "gc_audio_avdtp.h"

#include <stdio.h>
#include <string.h>

static void copy_reason(char *reason, size_t cap, const char *text)
{
    if (!reason || !cap) return;
    snprintf(reason, cap, "%s", text ? text : "");
}

static void set_reason(GcAudioAvdtp *session, const char *text)
{
    copy_reason(session->reason, sizeof session->reason, text);
}

static int field_ok(uint8_t mask, uint8_t legal)
{
    return mask != 0 && (mask & (uint8_t)~legal) == 0 && (mask & legal) != 0;
}

int gc_audio_avdtp_caps_valid(const GcAvdtpSbcCaps *caps, char *reason, size_t reason_cap)
{
    if (!caps) {
        copy_reason(reason, reason_cap, "malformed_capability");
        return -1;
    }
    if (!field_ok(caps->frequency_mask, 0xF0u) || !field_ok(caps->channel_mask, 0x0Fu) ||
        !field_ok(caps->block_mask, 0xF0u) || !field_ok(caps->subband_mask, 0x0Cu) ||
        !field_ok(caps->allocation_mask, 0x03u) || caps->bitpool_min < 2 ||
        caps->bitpool_max > 250 || caps->bitpool_min > caps->bitpool_max) {
        copy_reason(reason, reason_cap, "malformed_capability");
        return -1;
    }
    copy_reason(reason, reason_cap, "");
    return 0;
}

static int pick_bit(uint8_t mask, const uint8_t *prefer, int count, uint8_t *bit)
{
    for (int i = 0; i < count; ++i) {
        if (mask & prefer[i]) {
            *bit = prefer[i];
            return 0;
        }
    }
    return -1;
}

int gc_audio_avdtp_intersect(const GcAvdtpSbcCaps *local, const GcAvdtpSbcCaps *remote,
                             GcAvdtpSbcConfig *out, char *reason, size_t reason_cap)
{
    if (!out) {
        copy_reason(reason, reason_cap, "malformed_capability");
        return -1;
    }
    if (gc_audio_avdtp_caps_valid(local, reason, reason_cap) ||
        gc_audio_avdtp_caps_valid(remote, reason, reason_cap)) {
        return -1;
    }
    const uint8_t freq = (uint8_t)(local->frequency_mask & remote->frequency_mask);
    const uint8_t chan = (uint8_t)(local->channel_mask & remote->channel_mask);
    const uint8_t blocks = (uint8_t)(local->block_mask & remote->block_mask);
    const uint8_t sub = (uint8_t)(local->subband_mask & remote->subband_mask);
    const uint8_t alloc = (uint8_t)(local->allocation_mask & remote->allocation_mask);
    const uint8_t pool_lo = local->bitpool_min > remote->bitpool_min ? local->bitpool_min
                                                                      : remote->bitpool_min;
    const uint8_t pool_hi = local->bitpool_max < remote->bitpool_max ? local->bitpool_max
                                                                      : remote->bitpool_max;
    if (!freq || !chan || !blocks || !sub || !alloc || pool_lo > pool_hi) {
        copy_reason(reason, reason_cap, "empty_intersection");
        return -2;
    }
    static const uint8_t freq_prefer[] = {
        GC_AVDTP_SBC_FREQ_48K, GC_AVDTP_SBC_FREQ_44K, GC_AVDTP_SBC_FREQ_32K, GC_AVDTP_SBC_FREQ_16K};
    static const uint8_t chan_prefer[] = {
        GC_AVDTP_SBC_CH_JOINT, GC_AVDTP_SBC_CH_STEREO, GC_AVDTP_SBC_CH_DUAL, GC_AVDTP_SBC_CH_MONO};
    static const uint8_t block_prefer[] = {
        GC_AVDTP_SBC_BLK_16, GC_AVDTP_SBC_BLK_12, GC_AVDTP_SBC_BLK_8, GC_AVDTP_SBC_BLK_4};
    static const uint8_t sub_prefer[] = {GC_AVDTP_SBC_SB_8, GC_AVDTP_SBC_SB_4};
    static const uint8_t alloc_prefer[] = {GC_AVDTP_SBC_ALLOC_LOUDNESS, GC_AVDTP_SBC_ALLOC_SNR};
    GcAvdtpSbcConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    if (pick_bit(freq, freq_prefer, 4, &cfg.frequency_bit) ||
        pick_bit(chan, chan_prefer, 4, &cfg.channel_bit) ||
        pick_bit(blocks, block_prefer, 4, &cfg.block_bit) ||
        pick_bit(sub, sub_prefer, 2, &cfg.subband_bit) ||
        pick_bit(alloc, alloc_prefer, 2, &cfg.allocation_bit)) {
        copy_reason(reason, reason_cap, "empty_intersection");
        return -2;
    }
    if (cfg.frequency_bit == GC_AVDTP_SBC_FREQ_48K) cfg.sample_rate = 48000;
    else if (cfg.frequency_bit == GC_AVDTP_SBC_FREQ_44K) cfg.sample_rate = 44100;
    else if (cfg.frequency_bit == GC_AVDTP_SBC_FREQ_32K) cfg.sample_rate = 32000;
    else cfg.sample_rate = 16000;
    if (cfg.channel_bit == GC_AVDTP_SBC_CH_MONO) {
        cfg.channel_mode = GC_AVDTP_MODE_MONO;
        cfg.channels = 1;
    } else if (cfg.channel_bit == GC_AVDTP_SBC_CH_DUAL) {
        cfg.channel_mode = GC_AVDTP_MODE_DUAL;
        cfg.channels = 2;
    } else if (cfg.channel_bit == GC_AVDTP_SBC_CH_STEREO) {
        cfg.channel_mode = GC_AVDTP_MODE_STEREO;
        cfg.channels = 2;
    } else {
        cfg.channel_mode = GC_AVDTP_MODE_JOINT;
        cfg.channels = 2;
    }
    if (cfg.block_bit == GC_AVDTP_SBC_BLK_16) cfg.blocks = 16;
    else if (cfg.block_bit == GC_AVDTP_SBC_BLK_12) cfg.blocks = 12;
    else if (cfg.block_bit == GC_AVDTP_SBC_BLK_8) cfg.blocks = 8;
    else cfg.blocks = 4;
    cfg.subbands = cfg.subband_bit == GC_AVDTP_SBC_SB_8 ? 8 : 4;
    cfg.allocation = cfg.allocation_bit == GC_AVDTP_SBC_ALLOC_SNR ? GC_AVDTP_ALLOC_SNR
                                                                   : GC_AVDTP_ALLOC_LOUDNESS;
    cfg.bitpool_min = pool_lo;
    cfg.bitpool_max = pool_hi;
    *out = cfg;
    copy_reason(reason, reason_cap, "");
    return 0;
}

int gc_audio_avdtp_parse_caps(const uint8_t *bytes, size_t len, GcAvdtpSbcCaps *out,
                              int *delay_reporting, char *reason, size_t reason_cap)
{
    if (delay_reporting) *delay_reporting = 0;
    if (!bytes || !out || len == 0) {
        copy_reason(reason, reason_cap, "malformed_capability");
        return -1;
    }
    uint8_t seen[32];
    memset(seen, 0, sizeof seen);
    int saw_transport = 0;
    int saw_codec = 0;
    int delay = 0;
    GcAvdtpSbcCaps caps;
    memset(&caps, 0, sizeof caps);
    size_t i = 0;
    while (i < len) {
        if (len - i < 2) {
            copy_reason(reason, reason_cap, "malformed_capability");
            return -1;
        }
        const uint8_t cat = bytes[i];
        const uint8_t losc = bytes[i + 1];
        if (len - i - 2 < losc) {
            copy_reason(reason, reason_cap, "malformed_capability");
            return -1;
        }
        if (seen[cat >> 3] & (uint8_t)(1u << (cat & 7))) {
            copy_reason(reason, reason_cap, "malformed_capability");
            return -1;
        }
        seen[cat >> 3] |= (uint8_t)(1u << (cat & 7));
        const uint8_t *data = bytes + i + 2;
        if (cat == 0) {
            copy_reason(reason, reason_cap, "malformed_capability");
            return -1;
        }
        if (cat == GC_AVDTP_CAT_MEDIA_TRANSPORT) {
            if (losc != 0) {
                copy_reason(reason, reason_cap, "malformed_capability");
                return -1;
            }
            saw_transport = 1;
        } else if (cat == GC_AVDTP_CAT_REPORTING) {
            /* Optional. A fitting length is ignored, not treated as support. */
        } else if (cat == GC_AVDTP_CAT_RECOVERY || cat == GC_AVDTP_CAT_CONTENT_PROTECTION ||
                   cat == GC_AVDTP_CAT_HEADER_COMPRESSION || cat == GC_AVDTP_CAT_MULTIPLEXING) {
            copy_reason(reason, reason_cap, "unsupported_category");
            return -3;
        } else if (cat == GC_AVDTP_CAT_MEDIA_CODEC) {
            if (losc != 6 || (data[0] & 0x0Fu) != 0 || (data[0] >> 4) != 0) {
                copy_reason(reason, reason_cap, "malformed_capability");
                return -1;
            }
            if (data[1] != 0) {
                copy_reason(reason, reason_cap, "unsupported_codec");
                return -2;
            }
            caps.frequency_mask = (uint8_t)(data[2] & 0xF0u);
            caps.channel_mask = (uint8_t)(data[2] & 0x0Fu);
            caps.block_mask = (uint8_t)(data[3] & 0xF0u);
            caps.subband_mask = (uint8_t)(data[3] & 0x0Cu);
            caps.allocation_mask = (uint8_t)(data[3] & 0x03u);
            caps.bitpool_min = data[4];
            caps.bitpool_max = data[5];
            saw_codec = 1;
        } else if (cat == GC_AVDTP_CAT_DELAY_REPORTING) {
            if (losc != 0) {
                copy_reason(reason, reason_cap, "malformed_capability");
                return -1;
            }
            delay = 1;
        }
        i += 2u + losc;
    }
    if (!saw_transport || !saw_codec || gc_audio_avdtp_caps_valid(&caps, reason, reason_cap)) {
        copy_reason(reason, reason_cap, "malformed_capability");
        return -1;
    }
    *out = caps;
    if (delay_reporting) *delay_reporting = delay;
    copy_reason(reason, reason_cap, "");
    return 0;
}

int gc_audio_avdtp_sbc_frame_bytes(uint8_t channel_mode, uint8_t blocks, uint8_t subbands,
                                   uint8_t bitpool, uint32_t *out)
{
    if (!out || bitpool < 2 || bitpool > 250) return -1;
    if (channel_mode > GC_AVDTP_MODE_JOINT) return -1;
    if (blocks != 4 && blocks != 8 && blocks != 12 && blocks != 16) return -1;
    if (subbands != 4 && subbands != 8) return -1;
    const uint32_t channels = channel_mode == GC_AVDTP_MODE_MONO ? 1u : 2u;
    const uint32_t scale = (4u * subbands * channels) / 8u;
    uint32_t bits;
    if (channel_mode == GC_AVDTP_MODE_MONO || channel_mode == GC_AVDTP_MODE_DUAL) {
        bits = (uint32_t)blocks * channels * bitpool;
    } else {
        const uint32_t joint = channel_mode == GC_AVDTP_MODE_JOINT ? subbands : 0u;
        bits = joint + (uint32_t)blocks * bitpool;
    }
    *out = 4u + scale + (bits + 7u) / 8u;
    return 0;
}

static const char *const kStateNames[] = {
    "idle", "discovering", "discovered", "awaiting_caps", "capabilities", "configuring",
    "configured", "opening", "open", "media_ready", "starting", "streaming", "suspending",
    "suspended", "closing", "aborting"};

const char *gc_audio_avdtp_state_name(GcAvdtpState state)
{
    if ((unsigned)state >= sizeof kStateNames / sizeof kStateNames[0]) return "unknown";
    return kStateNames[state];
}

void gc_audio_avdtp_init(GcAudioAvdtp *session)
{
    if (!session) return;
    memset(session, 0, sizeof *session);
    session->sink_delay_tenths = -1;
    session->state = GC_AVDTP_IDLE;
}

void gc_audio_avdtp_shutdown(GcAudioAvdtp *session)
{
    if (!session) return;
    session->shut_down = 1;
    session->outstanding = 0;
    session->state = GC_AVDTP_IDLE;
    session->selection_valid = 0;
    session->media_ready_flag = 0;
    session->late_hold = 1;
    session->epoch++;
    session->session_generation++;
    set_reason(session, "shut_down");
}

static GcAudioError ready(const GcAudioAvdtp *session)
{
    if (!session) return GC_AUDIO_INVALID_ARGUMENT;
    if (session->shut_down || !session->bound) return GC_AUDIO_PROFILE_UNAVAILABLE;
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_avdtp_bind(GcAudioAvdtp *session, const char *address, uint8_t local_seid,
                                 const GcAvdtpSbcCaps *local, uint32_t ssrc, uint16_t sequence,
                                 uint32_t timestamp)
{
    if (!session || !local) return GC_AUDIO_INVALID_ARGUMENT;
    if (session->shut_down) return GC_AUDIO_PROFILE_UNAVAILABLE;
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (gc_audio_address_canonical(address, canonical)) return GC_AUDIO_INVALID_IDENTITY;
    if (local_seid == 0) return GC_AUDIO_INVALID_ARGUMENT;
    if (gc_audio_avdtp_caps_valid(local, session->reason, sizeof session->reason)) {
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if (session->bound && session->late_hold) return GC_AUDIO_STALE_GENERATION;
    if (session->bound && (session->state != GC_AVDTP_IDLE || session->outstanding)) {
        return GC_AUDIO_BUSY;
    }
    session->bound = 1;
    memcpy(session->address, canonical, GC_AUDIO_ADDRESS_TEXT);
    session->int_seid = local_seid;
    session->local = *local;
    session->ssrc = ssrc;
    session->sequence = sequence;
    session->timestamp = timestamp;
    session->state = GC_AVDTP_IDLE;
    set_reason(session, "");
    return GC_AUDIO_OK;
}

static int find_label(const GcAudioAvdtp *session, uint8_t *label)
{
    if (session->retired == 0xFFFFu) return -1;
    for (int i = 0; i < 16; ++i) {
        const uint8_t candidate = (uint8_t)((session->next_label + i) & 15);
        if ((session->retired & (uint16_t)(1u << candidate)) == 0) {
            *label = candidate;
            return 0;
        }
    }
    return -1;
}

static void commit_tx(GcAudioAvdtp *session, uint8_t label, uint8_t signal, GcAvdtpState next)
{
    session->retired = (uint16_t)(session->retired | (uint16_t)(1u << label));
    session->next_label = (uint8_t)((label + 1u) & 15u);
    session->outstanding = 1;
    session->out_label = label;
    session->out_signal = signal;
    session->out_epoch = session->epoch;
    session->resume_state = session->state;
    session->state = next;
}

static GcAvdtpCommand fail_cmd(GcAudioError error)
{
    GcAvdtpCommand cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.error = error;
    return cmd;
}

static GcAudioError can_issue(const GcAudioAvdtp *session)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    if (session->late_hold) return GC_AUDIO_STALE_GENERATION;
    if (session->outstanding) return GC_AUDIO_BUSY;
    return GC_AUDIO_OK;
}

static GcAvdtpCommand issue(GcAudioAvdtp *session, uint8_t signal, GcAvdtpState next, int with_seid,
                            uint8_t *pdu, size_t cap)
{
    const GcAudioError error = can_issue(session);
    if (error) return fail_cmd(error);
    const size_t need = with_seid ? 3u : 2u;
    if (!pdu || cap < need) return fail_cmd(GC_AUDIO_INVALID_ARGUMENT);
    uint8_t label = 0;
    if (find_label(session, &label)) return fail_cmd(GC_AUDIO_BUSY);
    pdu[0] = (uint8_t)((label << 4) | GC_AVDTP_MSG_COMMAND);
    pdu[1] = signal;
    if (with_seid) pdu[2] = (uint8_t)(session->acp_seid << 2);
    commit_tx(session, label, signal, next);
    GcAvdtpCommand cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.error = GC_AUDIO_OK;
    cmd.length = need;
    cmd.label = label;
    cmd.signal = signal;
    return cmd;
}

GcAvdtpCommand gc_audio_avdtp_discover(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    const GcAudioError error = can_issue(session);
    if (error) return fail_cmd(error);
    if (session->state != GC_AVDTP_IDLE) return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    return issue(session, GC_AVDTP_DISCOVER, GC_AVDTP_DISCOVERING, 0, pdu, cap);
}

GcAvdtpCommand gc_audio_avdtp_query_capabilities(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state != GC_AVDTP_DISCOVERED || session->acp_seid == 0) {
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    const uint8_t signal = session->caps_fallback ? GC_AVDTP_GET_CAPABILITIES
                                                   : GC_AVDTP_GET_ALL_CAPABILITIES;
    GcAvdtpCommand cmd = issue(session, signal, GC_AVDTP_AWAITING_CAPS, 1, pdu, cap);
    if (!cmd.error) session->caps_query_all = signal == GC_AVDTP_GET_ALL_CAPABILITIES;
    return cmd;
}

static GcAvdtpCommand configure_pdu(GcAudioAvdtp *session, uint8_t *pdu, size_t cap,
                                    uint8_t bitpool_min, uint8_t bitpool_max)
{
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state != GC_AVDTP_CAPABILITIES || !session->selection_valid) {
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    if (!pdu || cap < 14) return fail_cmd(GC_AUDIO_INVALID_ARGUMENT);
    uint8_t label = 0;
    if (find_label(session, &label)) return fail_cmd(GC_AUDIO_BUSY);
    const GcAvdtpSbcConfig *cfg = &session->selected;
    pdu[0] = (uint8_t)((label << 4) | GC_AVDTP_MSG_COMMAND);
    pdu[1] = GC_AVDTP_SET_CONFIGURATION;
    pdu[2] = (uint8_t)(session->acp_seid << 2);
    pdu[3] = (uint8_t)(session->int_seid << 2);
    pdu[4] = GC_AVDTP_CAT_MEDIA_TRANSPORT;
    pdu[5] = 0;
    pdu[6] = GC_AVDTP_CAT_MEDIA_CODEC;
    pdu[7] = 6;
    pdu[8] = 0;
    pdu[9] = 0;
    pdu[10] = (uint8_t)(cfg->frequency_bit | cfg->channel_bit);
    pdu[11] = (uint8_t)(cfg->block_bit | cfg->subband_bit | cfg->allocation_bit);
    pdu[12] = bitpool_min;
    pdu[13] = bitpool_max;
    commit_tx(session, label, GC_AVDTP_SET_CONFIGURATION, GC_AVDTP_CONFIGURING);
    GcAvdtpCommand cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.error = GC_AUDIO_OK;
    cmd.length = 14;
    cmd.label = label;
    cmd.signal = GC_AVDTP_SET_CONFIGURATION;
    return cmd;
}

GcAvdtpCommand gc_audio_avdtp_configure(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    if (!session) return fail_cmd(GC_AUDIO_INVALID_ARGUMENT);
    return configure_pdu(session, pdu, cap, session->selected.bitpool_min,
                         session->selected.bitpool_max);
}

GcAvdtpCommand gc_audio_avdtp_configure_fixed(GcAudioAvdtp *session, uint8_t *pdu, size_t cap,
                                             uint8_t bitpool)
{
    if (!session) return fail_cmd(GC_AUDIO_INVALID_ARGUMENT);
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state != GC_AVDTP_CAPABILITIES || !session->selection_valid) {
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    if (!session->config_refused) {
        set_reason(session, "no_config_refusal");
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    if (session->config_fixed_used) {
        set_reason(session, "fixed_bitpool_used");
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    if (bitpool < session->selected.bitpool_min || bitpool > session->selected.bitpool_max) {
        set_reason(session, "bitpool_window");
        return fail_cmd(GC_AUDIO_NEGOTIATION_REJECTED);
    }
    GcAvdtpCommand cmd = configure_pdu(session, pdu, cap, bitpool, bitpool);
    if (cmd.error) return cmd;
    session->config_fixed_used = 1;
    session->fixed_pending = bitpool;
    session->configure_retries++;
    set_reason(session, "config_retry_fixed");
    return cmd;
}

GcAvdtpCommand gc_audio_avdtp_open(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state != GC_AVDTP_CONFIGURED) return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    return issue(session, GC_AVDTP_OPEN, GC_AVDTP_OPENING, 1, pdu, cap);
}

static uint32_t min_frame_bytes(const GcAvdtpSbcConfig *cfg)
{
    uint32_t len = 0;
    if (gc_audio_avdtp_sbc_frame_bytes(cfg->channel_mode, cfg->blocks, cfg->subbands,
                                       cfg->bitpool_min, &len)) {
        return 0;
    }
    return len;
}

GcAudioError gc_audio_avdtp_media_up(GcAudioAvdtp *session, uint16_t l2cap_mtu,
                                     uint16_t acl_payload_max)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    if (session->state != GC_AVDTP_OPENED || !session->selection_valid) {
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    const uint32_t frame = min_frame_bytes(&session->selected);
    if (frame == 0 || frame > 65535u - GC_AVDTP_MEDIA_OVERHEAD) {
        set_reason(session, "mtu_too_small");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    const uint32_t sdu = GC_AVDTP_MEDIA_OVERHEAD + frame;
    const uint32_t acl_need = GC_AVDTP_L2CAP_BASIC + sdu;
    if (l2cap_mtu < sdu) {
        set_reason(session, "mtu_too_small");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if (acl_payload_max < acl_need) {
        set_reason(session, "acl_too_small");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    session->l2cap_mtu = l2cap_mtu;
    session->acl_payload_max = acl_payload_max;
    session->media_ready_flag = 1;
    session->state = GC_AVDTP_MEDIA_READY;
    set_reason(session, "");
    return GC_AUDIO_OK;
}

static void to_idle(GcAudioAvdtp *session, const char *reason)
{
    session->state = GC_AVDTP_IDLE;
    session->outstanding = 0;
    session->selection_valid = 0;
    session->media_ready_flag = 0;
    session->caps_fallback = 0;
    session->caps_query_all = 0;
    session->acp_seid = 0;
    session->l2cap_mtu = 0;
    session->acl_payload_max = 0;
    session->suspend_unconfirmed = 0;
    session->config_refused = 0;
    session->config_fixed_used = 0;
    session->fixed_pending = 0;
    session->fixed_bitpool = 0;
    session->epoch++;
    session->session_generation++;
    session->late_hold = 1;
    set_reason(session, reason);
}

GcAudioError gc_audio_avdtp_media_lost(GcAudioAvdtp *session)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    if (session->state != GC_AVDTP_MEDIA_READY && session->state != GC_AVDTP_STARTING &&
        session->state != GC_AVDTP_STREAMING && session->state != GC_AVDTP_SUSPENDING &&
        session->state != GC_AVDTP_SUSPENDED) {
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    session->outstanding = 0;
    session->media_ready_flag = 0;
    session->l2cap_mtu = 0;
    session->acl_payload_max = 0;
    session->state = GC_AVDTP_OPENED;
    session->epoch++;
    set_reason(session, "media_lost");
    return GC_AUDIO_TRANSPORT_LOST;
}

GcAvdtpCommand gc_audio_avdtp_start(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state != GC_AVDTP_MEDIA_READY && session->state != GC_AVDTP_SUSPENDED) {
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    return issue(session, GC_AVDTP_START, GC_AVDTP_STARTING, 1, pdu, cap);
}

GcAvdtpCommand gc_audio_avdtp_suspend(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state != GC_AVDTP_STREAMING) return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    return issue(session, GC_AVDTP_SUSPEND, GC_AVDTP_SUSPENDING, 1, pdu, cap);
}

GcAvdtpCommand gc_audio_avdtp_close(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state != GC_AVDTP_OPENED && session->state != GC_AVDTP_MEDIA_READY &&
        session->state != GC_AVDTP_STREAMING && session->state != GC_AVDTP_SUSPENDED) {
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    GcAvdtpCommand cmd = issue(session, GC_AVDTP_CLOSE, GC_AVDTP_CLOSING, 1, pdu, cap);
    return cmd;
}

GcAvdtpCommand gc_audio_avdtp_abort(GcAudioAvdtp *session, uint8_t *pdu, size_t cap)
{
    if (can_issue(session)) return fail_cmd(can_issue(session));
    if (session->state == GC_AVDTP_IDLE || session->acp_seid == 0) {
        return fail_cmd(GC_AUDIO_PROFILE_UNAVAILABLE);
    }
    return issue(session, GC_AVDTP_ABORT, GC_AVDTP_ABORTING, 1, pdu, cap);
}

GcAudioError gc_audio_avdtp_disconnect(GcAudioAvdtp *session)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    to_idle(session, "transport_lost");
    return GC_AUDIO_TRANSPORT_LOST;
}

GcAudioError gc_audio_avdtp_drop_late(GcAudioAvdtp *session)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    if (session->outstanding) return GC_AUDIO_BUSY;
    session->retired = 0;
    session->next_label = 0;
    session->late_hold = 0;
    return GC_AUDIO_OK;
}

static void consume(GcAudioAvdtp *session)
{
    session->outstanding = 0;
}

static int start_refused(GcAudioAvdtp *session, const char *reason)
{
    session->state = session->resume_state;
    set_reason(session, reason);
    return 1;
}

GcAudioError gc_audio_avdtp_timeout(GcAudioAvdtp *session, uint8_t label)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    if (!session->outstanding || label != session->out_label) {
        session->stale_responses++;
        return GC_AUDIO_STALE_GENERATION;
    }
    const uint8_t signal = session->out_signal;
    consume(session);
    if (signal == GC_AVDTP_START) {
        start_refused(session, "start_timeout");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if (signal == GC_AVDTP_SUSPEND) {
        session->state = GC_AVDTP_SUSPENDED;
        session->suspend_unconfirmed = 1;
        set_reason(session, "suspend_unconfirmed");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if (signal == GC_AVDTP_CLOSE) {
        session->close_unconfirmed = 1;
        to_idle(session, "close_unconfirmed");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if (signal == GC_AVDTP_ABORT) {
        to_idle(session, "abort_timeout");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if (signal == GC_AVDTP_SET_CONFIGURATION) {
        session->state = GC_AVDTP_CAPABILITIES;
        session->fixed_pending = 0;
        set_reason(session, "config_timeout");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if (signal == GC_AVDTP_OPEN) {
        session->state = GC_AVDTP_CONFIGURED;
        set_reason(session, "open_timeout");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    to_idle(session, "timeout");
    return GC_AUDIO_NEGOTIATION_REJECTED;
}

static int seid_ok(const GcAudioAvdtp *session, uint8_t byte)
{
    return session->acp_seid != 0 && (byte & 0x03u) == 0 && (byte >> 2) == session->acp_seid;
}

static GcAvdtpFeed feed_fail(GcAudioError error)
{
    GcAvdtpFeed feed;
    memset(&feed, 0, sizeof feed);
    feed.error = error;
    return feed;
}

static int discover_seps(const uint8_t *body, size_t len, uint8_t *seid)
{
    if (len < 2 || (len % 2u) != 0) return -1;
    int found = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        const uint8_t b0 = body[i];
        const uint8_t b1 = body[i + 1];
        if ((b0 & 0x01u) != 0 || (b1 & 0x07u) != 0) return -1;
        const uint8_t id = (uint8_t)(b0 >> 2);
        const int in_use = (b0 >> 1) & 1;
        const uint8_t media = (uint8_t)(b1 >> 4);
        const int snk = (b1 >> 3) & 1;
        if (id == 0) return -1;
        if (!found && !in_use && media == 0 && snk) {
            found = 1;
            *seid = id;
        }
    }
    return found ? 0 : 1;
}

static void apply_response(GcAudioAvdtp *session, uint8_t msg, const uint8_t *body, size_t body_len,
                           GcAvdtpFeed *feed)
{
    const uint8_t signal = session->out_signal;
    consume(session);
    feed->applied = 1;
    const int accept = msg == GC_AVDTP_MSG_ACCEPT;
    const int general = msg == GC_AVDTP_MSG_GENERAL_REJECT;
    if (!accept && !general && msg != GC_AVDTP_MSG_REJECT) {
        feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
        return;
    }
    if (signal == GC_AVDTP_DISCOVER) {
        uint8_t seid = 0;
        const int parsed = accept ? discover_seps(body, body_len, &seid) : 1;
        if (!accept || parsed != 0) {
            to_idle(session, parsed < 0 ? "malformed_capability" : "no_sink");
            feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        session->acp_seid = seid;
        session->state = GC_AVDTP_DISCOVERED;
        session->caps_fallback = 0;
        set_reason(session, "");
        feed->error = GC_AUDIO_OK;
        return;
    }
    if (signal == GC_AVDTP_GET_ALL_CAPABILITIES || signal == GC_AVDTP_GET_CAPABILITIES) {
        if (!accept) {
            uint8_t code = 0;
            if (!general && body_len >= 1) code = body[0];
            if (session->caps_query_all && (general || code == GC_AVDTP_ERR_NOT_SUPPORTED)) {
                session->state = GC_AVDTP_DISCOVERED;
                session->caps_fallback = 1;
                set_reason(session, "caps_fallback");
                feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
                return;
            }
            to_idle(session, "caps_rejected");
            feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        GcAvdtpSbcCaps remote;
        char why[48];
        const int parsed = gc_audio_avdtp_parse_caps(body, body_len, &remote, NULL, why, sizeof why);
        GcAvdtpSbcConfig cfg;
        const int crossed = parsed == 0 ? gc_audio_avdtp_intersect(&session->local, &remote, &cfg, why,
                                                                    sizeof why)
                                         : parsed;
        if (parsed || crossed) {
            to_idle(session, why[0] ? why : "malformed_capability");
            feed->error = parsed == -2 ? GC_AUDIO_UNSUPPORTED_FORMAT : GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        session->selected = cfg;
        session->selection_valid = 1;
        session->state = GC_AVDTP_CAPABILITIES;
        set_reason(session, "");
        feed->error = GC_AUDIO_OK;
        return;
    }
    if (signal == GC_AVDTP_SET_CONFIGURATION) {
        if (!accept || body_len != 0) {
            session->state = GC_AVDTP_CAPABILITIES;
            session->fixed_pending = 0;
            /* A refusal is the one trigger for the fixed-bitpool retry; a
             * malformed accept or a timeout is not. */
            if (!accept) session->config_refused = 1;
            set_reason(session, accept ? "malformed_response" : "config_rejected");
            feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        if (session->fixed_pending) {
            /* The peer took a single bitpool: that one value is the window. */
            session->selected.bitpool_min = (uint8_t)session->fixed_pending;
            session->selected.bitpool_max = (uint8_t)session->fixed_pending;
            session->fixed_bitpool = session->fixed_pending;
            session->fixed_pending = 0;
        }
        session->config_refused = 0;
        session->state = GC_AVDTP_CONFIGURED;
        set_reason(session, session->fixed_bitpool ? "configured_fixed" : "");
        feed->error = GC_AUDIO_OK;
        return;
    }
    if (signal == GC_AVDTP_OPEN) {
        if (!accept || body_len != 0) {
            session->state = GC_AVDTP_CONFIGURED;
            set_reason(session, accept ? "malformed_response" : "open_rejected");
            feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        session->state = GC_AVDTP_OPENED;
        set_reason(session, "");
        feed->error = GC_AUDIO_OK;
        return;
    }
    if (signal == GC_AVDTP_START) {
        if (!accept || body_len != 0) {
            start_refused(session, accept ? "malformed_response" : "start_refused");
            feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        session->state = GC_AVDTP_STREAMING;
        session->suspend_unconfirmed = 0;
        set_reason(session, "");
        feed->error = GC_AUDIO_OK;
        return;
    }
    if (signal == GC_AVDTP_SUSPEND) {
        if (!accept || body_len != 0) {
            session->state = GC_AVDTP_STREAMING;
            set_reason(session, "suspend_rejected");
            feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        session->state = GC_AVDTP_SUSPENDED;
        session->suspend_unconfirmed = 0;
        set_reason(session, "");
        feed->error = GC_AUDIO_OK;
        return;
    }
    if (signal == GC_AVDTP_CLOSE || signal == GC_AVDTP_ABORT) {
        if (!accept || body_len != 0) {
            to_idle(session, "close_rejected");
            feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
            return;
        }
        to_idle(session, signal == GC_AVDTP_ABORT ? "abort" : "close");
        feed->error = GC_AUDIO_OK;
        return;
    }
    to_idle(session, "unexpected_signal");
    feed->error = GC_AUDIO_NEGOTIATION_REJECTED;
}

static GcAvdtpFeed handle_command(GcAudioAvdtp *session, uint8_t label, uint8_t signal,
                                  const uint8_t *body, size_t body_len, uint8_t *reply,
                                  size_t reply_cap)
{
    const size_t reject_need = 3;
    const size_t accept_need = 2;
    if (!reply || reply_cap < reject_need) return feed_fail(GC_AUDIO_INVALID_ARGUMENT);
    int accept = 0;
    uint8_t code = GC_AVDTP_ERR_BAD_STATE;
    if (signal == GC_AVDTP_SUSPEND) {
        if (body_len == 1 && seid_ok(session, body[0]) && session->state == GC_AVDTP_STREAMING &&
            !session->outstanding) {
            accept = 1;
        } else if (body_len != 1) {
            code = GC_AVDTP_ERR_BAD_LENGTH;
        } else if (body_len == 1 && !seid_ok(session, body[0])) {
            code = GC_AVDTP_ERR_BAD_ACP_SEID;
        }
    } else if (signal == GC_AVDTP_START) {
        if (body_len == 1 && seid_ok(session, body[0]) && !session->outstanding &&
            (session->state == GC_AVDTP_MEDIA_READY || session->state == GC_AVDTP_SUSPENDED)) {
            accept = 1;
        } else if (body_len != 1) {
            code = GC_AVDTP_ERR_BAD_LENGTH;
        } else if (body_len == 1 && !seid_ok(session, body[0])) {
            code = GC_AVDTP_ERR_BAD_ACP_SEID;
        }
    } else if (signal == GC_AVDTP_ABORT) {
        if (body_len == 1 && seid_ok(session, body[0]) && session->state != GC_AVDTP_IDLE) {
            accept = 1;
        } else if (body_len != 1) {
            code = GC_AVDTP_ERR_BAD_LENGTH;
        } else if (session->state != GC_AVDTP_IDLE && body_len == 1 && !seid_ok(session, body[0])) {
            code = GC_AVDTP_ERR_BAD_ACP_SEID;
        }
    } else if (signal == GC_AVDTP_CLOSE) {
        if (body_len == 1 && seid_ok(session, body[0]) &&
            (session->state == GC_AVDTP_OPENED || session->state == GC_AVDTP_MEDIA_READY ||
             session->state == GC_AVDTP_STARTING || session->state == GC_AVDTP_STREAMING ||
             session->state == GC_AVDTP_SUSPENDING || session->state == GC_AVDTP_SUSPENDED)) {
            accept = 1;
        } else if (body_len != 1) {
            code = GC_AVDTP_ERR_BAD_LENGTH;
        } else if (body_len == 1 && !seid_ok(session, body[0])) {
            code = GC_AVDTP_ERR_BAD_ACP_SEID;
        }
    } else if (signal == GC_AVDTP_DELAYREPORT) {
        if (body_len == 3 && seid_ok(session, body[0]) &&
            (session->state == GC_AVDTP_OPENED || session->state == GC_AVDTP_MEDIA_READY ||
             session->state == GC_AVDTP_STREAMING || session->state == GC_AVDTP_SUSPENDED)) {
            accept = 1;
        } else if (body_len != 3) {
            code = GC_AVDTP_ERR_BAD_LENGTH;
        } else if (!seid_ok(session, body[0])) {
            code = GC_AVDTP_ERR_BAD_ACP_SEID;
        }
    } else if (signal == GC_AVDTP_RECONFIGURE || signal == GC_AVDTP_SECURITY_CONTROL ||
               signal == GC_AVDTP_GET_CAPABILITIES || signal == GC_AVDTP_GET_ALL_CAPABILITIES ||
               signal == GC_AVDTP_GET_CONFIGURATION || signal == GC_AVDTP_SET_CONFIGURATION ||
               signal == GC_AVDTP_DISCOVER || signal == GC_AVDTP_OPEN) {
        code = GC_AVDTP_ERR_NOT_SUPPORTED;
    } else {
        code = GC_AVDTP_ERR_NOT_SUPPORTED;
    }
    GcAvdtpFeed feed;
    memset(&feed, 0, sizeof feed);
    if (!accept && reply_cap < reject_need) return feed_fail(GC_AUDIO_INVALID_ARGUMENT);
    if (accept && reply_cap < accept_need) return feed_fail(GC_AUDIO_INVALID_ARGUMENT);
    if (accept) {
        if (signal == GC_AVDTP_SUSPEND) {
            session->state = GC_AVDTP_SUSPENDED;
            session->suspend_unconfirmed = 0;
            set_reason(session, "remote_suspend");
        } else if (signal == GC_AVDTP_START) {
            session->state = GC_AVDTP_STREAMING;
            session->suspend_unconfirmed = 0;
            set_reason(session, "remote_start");
        } else if (signal == GC_AVDTP_ABORT) {
            to_idle(session, "remote_abort");
        } else if (signal == GC_AVDTP_CLOSE) {
            to_idle(session, "remote_close");
        } else if (signal == GC_AVDTP_DELAYREPORT) {
            session->sink_delay_reported = 1;
            session->sink_delay_tenths = (body[1] << 8) | body[2];
            set_reason(session, "delay_reported");
        }
        reply[0] = (uint8_t)((label << 4) | GC_AVDTP_MSG_ACCEPT);
        reply[1] = signal;
        feed.reply_len = accept_need;
        feed.accepted = 1;
        feed.error = GC_AUDIO_OK;
        return feed;
    }
    reply[0] = (uint8_t)((label << 4) | GC_AVDTP_MSG_REJECT);
    reply[1] = signal;
    reply[2] = code;
    feed.reply_len = reject_need;
    feed.error = GC_AUDIO_OK;
    return feed;
}

GcAvdtpFeed gc_audio_avdtp_feed(GcAudioAvdtp *session, const uint8_t *pdu, size_t len,
                                uint8_t *reply, size_t reply_cap)
{
    const GcAudioError error = ready(session);
    if (error) return feed_fail(error);
    if (!pdu || len < 2) {
        session->malformed_pdus++;
        return feed_fail(GC_AUDIO_INVALID_ARGUMENT);
    }
    const unsigned packet_type = (pdu[0] >> 2) & 0x3u;
    const unsigned msg = pdu[0] & 0x3u;
    const uint8_t label = (uint8_t)(pdu[0] >> 4);
    const uint8_t signal = pdu[1];
    if (packet_type != 0) {
        session->malformed_pdus++;
        return feed_fail(GC_AUDIO_NEGOTIATION_REJECTED);
    }
    if (msg == GC_AVDTP_MSG_COMMAND) {
        return handle_command(session, label, signal, pdu + 2, len - 2, reply, reply_cap);
    }
    if (msg != GC_AVDTP_MSG_ACCEPT && msg != GC_AVDTP_MSG_REJECT && msg != GC_AVDTP_MSG_GENERAL_REJECT) {
        session->malformed_pdus++;
        return feed_fail(GC_AUDIO_INVALID_ARGUMENT);
    }
    if (!session->outstanding || label != session->out_label || signal != session->out_signal ||
        session->out_epoch != session->epoch) {
        session->stale_responses++;
        GcAvdtpFeed feed = feed_fail(GC_AUDIO_STALE_GENERATION);
        feed.stale = 1;
        return feed;
    }
    GcAvdtpFeed feed;
    memset(&feed, 0, sizeof feed);
    apply_response(session, (uint8_t)msg, pdu + 2, len - 2, &feed);
    return feed;
}

static int frame_matches(const GcAvdtpSbcConfig *cfg, const uint8_t *frame, size_t avail,
                         uint32_t *frame_len)
{
    if (avail < 4 || frame[0] != 0x9Cu) return -1;
    const uint8_t b = frame[1];
    static const uint32_t rates[4] = {16000u, 32000u, 44100u, 48000u};
    static const uint8_t block_n[4] = {4, 8, 12, 16};
    static const uint8_t sub_n[2] = {4, 8};
    if (rates[(b >> 6) & 3] != cfg->sample_rate) return -1;
    if (block_n[(b >> 4) & 3] != cfg->blocks) return -1;
    if (((b >> 2) & 3) != cfg->channel_mode) return -1;
    if (sub_n[b & 1] != cfg->subbands) return -1;
    if (((b >> 1) & 1) != cfg->allocation) return -1;
    const uint8_t bitpool = frame[2];
    if (bitpool < cfg->bitpool_min || bitpool > cfg->bitpool_max) return -1;
    uint32_t len = 0;
    if (gc_audio_avdtp_sbc_frame_bytes(cfg->channel_mode, cfg->blocks, cfg->subbands, bitpool, &len)) {
        return -1;
    }
    if (avail < len) return -1;
    *frame_len = len;
    return 0;
}

GcAvdtpPacket gc_audio_avdtp_packetize(GcAudioAvdtp *session, const uint8_t *frames,
                                       size_t frames_len, uint8_t frame_count, uint8_t *out,
                                       size_t out_cap)
{
    GcAvdtpPacket packet;
    memset(&packet, 0, sizeof packet);
    if (!session) {
        packet.error = GC_AUDIO_INVALID_ARGUMENT;
        return packet;
    }
    if (session->shut_down || !session->bound) {
        packet.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return packet;
    }
    if (session->state != GC_AVDTP_STREAMING) {
        session->rejected_packets++;
        set_reason(session, "not_streaming");
        packet.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return packet;
    }
    if (frame_count < 1 || frame_count > 15 || !frames) {
        session->rejected_packets++;
        set_reason(session, "bad_frame_count");
        packet.error = GC_AUDIO_INVALID_ARGUMENT;
        return packet;
    }
    size_t off = 0;
    uint32_t first = 0;
    for (uint8_t n = 0; n < frame_count; ++n) {
        uint32_t flen = 0;
        if (off > frames_len || frame_matches(&session->selected, frames + off, frames_len - off, &flen)) {
            session->rejected_packets++;
            set_reason(session, "non_whole_frame");
            packet.error = GC_AUDIO_INVALID_ARGUMENT;
            return packet;
        }
        if (n == 0) first = flen;
        off += flen;
    }
    if (off != frames_len) {
        session->rejected_packets++;
        set_reason(session, "non_whole_frame");
        packet.error = GC_AUDIO_INVALID_ARGUMENT;
        return packet;
    }
    if (frames_len > SIZE_MAX - GC_AVDTP_MEDIA_OVERHEAD) {
        session->rejected_packets++;
        packet.error = GC_AUDIO_INVALID_ARGUMENT;
        return packet;
    }
    const size_t sdu = GC_AVDTP_MEDIA_OVERHEAD + frames_len;
    const size_t one = GC_AVDTP_MEDIA_OVERHEAD + first;
    if (session->l2cap_mtu < one) {
        session->rejected_packets++;
        set_reason(session, "mtu_too_small");
        packet.error = GC_AUDIO_NEGOTIATION_REJECTED;
        return packet;
    }
    if (session->l2cap_mtu < sdu) {
        session->rejected_packets++;
        set_reason(session, "packet_exceeds_mtu");
        packet.error = GC_AUDIO_NEGOTIATION_REJECTED;
        return packet;
    }
    if (sdu > SIZE_MAX - GC_AVDTP_L2CAP_BASIC ||
        session->acl_payload_max < GC_AVDTP_L2CAP_BASIC + sdu) {
        session->rejected_packets++;
        set_reason(session, session->acl_payload_max < GC_AVDTP_L2CAP_BASIC + one ? "acl_too_small"
                                                                                   : "packet_exceeds_acl");
        packet.error = GC_AUDIO_NEGOTIATION_REJECTED;
        return packet;
    }
    if (!out || out_cap < sdu) {
        session->rejected_packets++;
        packet.error = GC_AUDIO_INVALID_ARGUMENT;
        return packet;
    }
    const uint16_t seq = session->sequence;
    const uint32_t ts = session->timestamp;
    const uint32_t delta = (uint32_t)frame_count * session->selected.blocks * session->selected.subbands;
    out[0] = 0x80;
    out[1] = GC_AVDTP_RTP_PAYLOAD_TYPE;
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)seq;
    out[4] = (uint8_t)(ts >> 24);
    out[5] = (uint8_t)(ts >> 16);
    out[6] = (uint8_t)(ts >> 8);
    out[7] = (uint8_t)ts;
    out[8] = (uint8_t)(session->ssrc >> 24);
    out[9] = (uint8_t)(session->ssrc >> 16);
    out[10] = (uint8_t)(session->ssrc >> 8);
    out[11] = (uint8_t)session->ssrc;
    out[12] = (uint8_t)(0x60u | frame_count);
    memcpy(out + GC_AVDTP_MEDIA_OVERHEAD, frames, frames_len);
    session->sequence = (uint16_t)(seq + 1u);
    session->timestamp = ts + delta;
    session->accepted_packets++;
    set_reason(session, "");
    packet.error = GC_AUDIO_OK;
    packet.length = sdu;
    packet.sequence = seq;
    packet.timestamp = ts;
    packet.timestamp_delta = delta;
    packet.frame_count = frame_count;
    return packet;
}

GcAudioError gc_audio_avdtp_note_unsent_drop(GcAudioAvdtp *session, uint32_t samples)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    if (session->state != GC_AVDTP_STREAMING) return GC_AUDIO_PROFILE_UNAVAILABLE;
    const uint32_t frame_samples = (uint32_t)session->selected.blocks * session->selected.subbands;
    if (samples == 0 || frame_samples == 0 || samples % frame_samples != 0) {
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    if (samples > 0xFFFFFFFFu - session->unsent_dropped_samples) return GC_AUDIO_INVALID_ARGUMENT;
    session->timestamp += samples;
    session->unsent_dropped_samples += samples;
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_avdtp_note_accepted_drop(GcAudioAvdtp *session, uint16_t packets,
                                               uint32_t samples)
{
    const GcAudioError error = ready(session);
    if (error) return error;
    if (packets == 0 || samples == 0) return GC_AUDIO_INVALID_ARGUMENT;
    if (!session->selection_valid) return GC_AUDIO_PROFILE_UNAVAILABLE;
    const uint32_t frame_samples = (uint32_t)session->selected.blocks * session->selected.subbands;
    if (frame_samples == 0 || samples % frame_samples != 0) return GC_AUDIO_INVALID_ARGUMENT;
    if ((uint32_t)session->accepted_then_dropped_packets + packets > session->accepted_packets) {
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    /* packets is 16-bit, so packets * 15 cannot wrap a 32-bit frame budget. */
    if (samples / frame_samples > (uint32_t)packets * 15u) return GC_AUDIO_INVALID_ARGUMENT;
    session->accepted_then_dropped_packets += packets;
    return GC_AUDIO_OK;
}

static int retired_count(uint16_t mask)
{
    int n = 0;
    for (int i = 0; i < 16; ++i) if (mask & (uint16_t)(1u << i)) ++n;
    return n;
}

void gc_audio_avdtp_snapshot(const GcAudioAvdtp *session, GcAudioAvdtpSnapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->ordinary_host_implemented = 1;
    out->sink_delay_tenths = -1;
    out->queue_delay_ms = -1;
    out->estimate_ms = -1;
    if (!session) return;
    out->shut_down = session->shut_down;
    out->bound = session->bound;
    out->streaming = session->state == GC_AVDTP_STREAMING;
    out->media_ready = session->media_ready_flag && session->state != GC_AVDTP_OPENED &&
                       session->state != GC_AVDTP_IDLE;
    out->selection_valid = session->selection_valid;
    out->caps_fallback = session->caps_fallback;
    out->late_hold = session->late_hold;
    out->suspend_unconfirmed = session->suspend_unconfirmed;
    out->close_unconfirmed = session->close_unconfirmed;
    out->sink_delay_reported = session->sink_delay_reported;
    out->sink_delay_tenths = session->sink_delay_reported ? session->sink_delay_tenths : -1;
    out->state = session->state;
    out->session_generation = session->session_generation;
    out->outstanding = (uint8_t)session->outstanding;
    out->outstanding_label = session->out_label;
    out->outstanding_signal = session->out_signal;
    out->acp_seid = session->acp_seid;
    out->int_seid = session->int_seid;
    out->labels_retired = (uint8_t)retired_count(session->retired);
    out->l2cap_mtu = session->l2cap_mtu;
    out->acl_payload_max = session->acl_payload_max;
    out->sequence_next = session->sequence;
    out->timestamp_next = session->timestamp;
    out->stale_responses = session->stale_responses;
    out->rejected_packets = session->rejected_packets;
    out->accepted_packets = session->accepted_packets;
    out->unsent_dropped_samples = session->unsent_dropped_samples;
    out->accepted_then_dropped_packets = session->accepted_then_dropped_packets;
    out->malformed_pdus = session->malformed_pdus;
    memcpy(out->address, session->address, GC_AUDIO_ADDRESS_TEXT);
    memcpy(out->reason, session->reason, sizeof out->reason);
    out->selected = session->selected;
    out->config_refused = session->config_refused;
    out->fixed_bitpool_used = session->fixed_bitpool;
    out->configure_retries = session->configure_retries;
}

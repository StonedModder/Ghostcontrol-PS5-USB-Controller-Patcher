/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-03 SBC encoder and credit owner. See gc_audio_codec.h.
 * Appendix B of A2DP v1.4 is the algorithm. No third-party encoder is linked.
 */
#include "gc_audio_codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Published prototype tables, A2DP v1.4 Appendix B.8, read row-wise. */
static const double kProto4[40] = {
    0.00000000E+00, 5.36548976E-04, 1.49188357E-03, 2.73370904E-03,
    3.83720193E-03, 3.89205149E-03, 1.86581691E-03, -3.06012286E-03,
    1.09137620E-02, 2.04385087E-02, 2.88757392E-02, 3.21939290E-02,
    2.58767811E-02, 6.13245186E-03, -2.88217274E-02, -7.76463494E-02,
    1.35593274E-01, 1.94987841E-01, 2.46636662E-01, 2.81828203E-01,
    2.94315332E-01, 2.81828203E-01, 2.46636662E-01, 1.94987841E-01,
    -1.35593274E-01, -7.76463494E-02, -2.88217274E-02, 6.13245186E-03,
    2.58767811E-02, 3.21939290E-02, 2.88757392E-02, 2.04385087E-02,
    -1.09137620E-02, -3.06012286E-03, 1.86581691E-03, 3.89205149E-03,
    3.83720193E-03, 2.73370904E-03, 1.49188357E-03, 5.36548976E-04
};

static const double kProto8[80] = {
    0.00000000E+00, 1.56575398E-04, 3.43256425E-04, 5.54620202E-04,
    8.23919506E-04, 1.13992507E-03, 1.47640169E-03, 1.78371725E-03,
    2.01182542E-03, 2.10371989E-03, 1.99454554E-03, 1.61656283E-03,
    9.02154502E-04, -1.78805361E-04, -1.64973098E-03, -3.49717454E-03,
    5.65949473E-03, 8.02941163E-03, 1.04584443E-02, 1.27472335E-02,
    1.46525263E-02, 1.59045603E-02, 1.62208471E-02, 1.53184106E-02,
    1.29371806E-02, 8.85757540E-03, 2.92408442E-03, -4.91578024E-03,
    -1.46404076E-02, -2.61098752E-02, -3.90751381E-02, -5.31873032E-02,
    6.79989431E-02, 8.29847578E-02, 9.75753918E-02, 1.11196689E-01,
    1.23264548E-01, 1.33264415E-01, 1.40753505E-01, 1.45389847E-01,
    1.46955068E-01, 1.45389847E-01, 1.40753505E-01, 1.33264415E-01,
    1.23264548E-01, 1.11196689E-01, 9.75753918E-02, 8.29847578E-02,
    -6.79989431E-02, -5.31873032E-02, -3.90751381E-02, -2.61098752E-02,
    -1.46404076E-02, -4.91578024E-03, 2.92408442E-03, 8.85757540E-03,
    1.29371806E-02, 1.53184106E-02, 1.62208471E-02, 1.59045603E-02,
    1.46525263E-02, 1.27472335E-02, 1.04584443E-02, 8.02941163E-03,
    -5.65949473E-03, -3.49717454E-03, -1.64973098E-03, -1.78805361E-04,
    9.02154502E-04, 1.61656283E-03, 1.99454554E-03, 2.10371989E-03,
    2.01182542E-03, 1.78371725E-03, 1.47640169E-03, 1.13992507E-03,
    8.23919506E-04, 5.54620202E-04, 3.43256425E-04, 1.56575398E-04
};

/* Appendix B.8 offset tables, indexed by sampling_frequency then subband. */
static const int kOffset4[4][4] = {
    {-1, 0, 0, 0},
    {-2, 0, 0, 1},
    {-2, 0, 0, 1},
    {-2, 0, 0, 1}
};

static const int kOffset8[4][8] = {
    {-2, 0, 0, 0, 0, 0, 0, 1},
    {-3, 0, 0, 0, 0, 0, 1, 2},
    {-4, 0, 0, 0, 0, 0, 1, 2},
    {-4, 0, 0, 0, 0, 0, 1, 2}
};

static void set_reason(GcAudioCodec *codec, const char *text)
{
    if (!codec) return;
    snprintf(codec->reason, sizeof codec->reason, "%s", text ? text : "");
}

static int alive(const GcAudioCodec *codec)
{
    return codec && !codec->shut_down;
}

void gc_audio_codec_init(GcAudioCodec *codec)
{
    if (!codec) return;
    memset(codec, 0, sizeof *codec);
    codec->reported_latency_samples = -1;
    codec->reported_delay_tenths = -1;
    codec->sig_cap = GC_AUDIO_CODEC_SIG_MAX;
    codec->hid_cap = GC_AUDIO_CODEC_HID_MAX;
    codec->media_cap = GC_AUDIO_CODEC_MEDIA_MAX;
    codec->shared_cap = GC_AUDIO_CODEC_SIG_MAX + GC_AUDIO_CODEC_HID_MAX + GC_AUDIO_CODEC_MEDIA_MAX;
}

void gc_audio_codec_shutdown(GcAudioCodec *codec)
{
    if (!codec) return;
    codec->shut_down = 1;
    codec->configured = 0;
    set_reason(codec, "shut_down");
}

static int rate_index(uint32_t rate)
{
    if (rate == 16000u) return 0;
    if (rate == 32000u) return 1;
    if (rate == 44100u) return 2;
    if (rate == 48000u) return 3;
    return -1;
}

static int blocks_index(uint8_t blocks)
{
    if (blocks == 4) return 0;
    if (blocks == 8) return 1;
    if (blocks == 12) return 2;
    if (blocks == 16) return 3;
    return -1;
}

static int channels_of(uint8_t mode)
{
    return mode == GC_AVDTP_MODE_MONO ? 1 : 2;
}

static int bitpool_cap(uint8_t mode, uint8_t subbands)
{
    /* Appendix B.5.1: bitpool shall not exceed 16*subbands (mono/dual) or
     * 32*subbands (stereo/joint). Above that the sample fields cannot hold
     * the bits the frame-length formula counts. */
    if (mode == GC_AVDTP_MODE_MONO || mode == GC_AVDTP_MODE_DUAL) return 16 * (int)subbands;
    return 32 * (int)subbands;
}

static GcAudioError check_selected(const GcAvdtpSbcConfig *selected, uint8_t bitpool, char *why, size_t why_cap)
{
    if (!selected) {
        snprintf(why, why_cap, "%s", "no_config");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    if (rate_index(selected->sample_rate) < 0 || blocks_index(selected->blocks) < 0 ||
        (selected->subbands != 4 && selected->subbands != 8) ||
        selected->channel_mode > GC_AVDTP_MODE_JOINT ||
        selected->allocation > GC_AVDTP_ALLOC_SNR ||
        selected->channels != (uint8_t)channels_of(selected->channel_mode)) {
        snprintf(why, why_cap, "%s", "unsupported_config");
        return GC_AUDIO_UNSUPPORTED_FORMAT;
    }
    if (selected->bitpool_min < 2 || selected->bitpool_max > 250 ||
        selected->bitpool_min > selected->bitpool_max) {
        snprintf(why, why_cap, "%s", "bad_window");
        return GC_AUDIO_UNSUPPORTED_FORMAT;
    }
    if (bitpool < selected->bitpool_min || bitpool > selected->bitpool_max) {
        snprintf(why, why_cap, "%s", "bitpool_window");
        return GC_AUDIO_NEGOTIATION_REJECTED;
    }
    if ((int)bitpool > bitpool_cap(selected->channel_mode, selected->subbands)) {
        snprintf(why, why_cap, "%s", "bitpool_uncodable");
        return GC_AUDIO_UNSUPPORTED_FORMAT;
    }
    uint32_t frame = 0;
    if (gc_audio_avdtp_sbc_frame_bytes(selected->channel_mode, selected->blocks, selected->subbands,
                                       bitpool, &frame) ||
        frame == 0 || frame > GC_AUDIO_CODEC_FRAME_MAX) {
        snprintf(why, why_cap, "%s", "frame_bounds");
        return GC_AUDIO_UNSUPPORTED_FORMAT;
    }
    return GC_AUDIO_OK;
}

static void drop_queued_media(GcAudioCodec *codec)
{
    codec->media_head = 0;
    codec->media_count = 0;
    codec->release_samples = 0;
}

GcAudioError gc_audio_codec_configure(GcAudioCodec *codec, const GcAvdtpSbcConfig *selected, uint8_t bitpool)
{
    if (!codec) return GC_AUDIO_INVALID_ARGUMENT;
    if (codec->shut_down) {
        set_reason(codec, "shut_down");
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    char why[48];
    const GcAudioError error = check_selected(selected, bitpool, why, sizeof why);
    if (error) {
        set_reason(codec, why);
        return error;
    }
    codec->selected = *selected;
    codec->bitpool = bitpool;
    codec->frame_samples = (uint32_t)selected->blocks * selected->subbands;
    codec->configured = 1;
    /* A new configuration has its own window: the adaptive controller must be
     * re-armed against it rather than carried across. */
    codec->rate_active = 0;
    codec->rate_cap = 0;
    memset(codec->history, 0, sizeof codec->history);
    drop_queued_media(codec);
    set_reason(codec, "");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_set_bitpool(GcAudioCodec *codec, uint8_t bitpool)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (!codec->configured) {
        set_reason(codec, "encoder_unavailable");
        return GC_AUDIO_ENCODER_UNAVAILABLE;
    }
    /* One writer: while the controller is armed it owns the bitpool. */
    if (codec->rate_active) {
        set_reason(codec, "bitpool_owned_by_rate");
        return GC_AUDIO_BUSY;
    }
    char why[48];
    const GcAudioError error = check_selected(&codec->selected, bitpool, why, sizeof why);
    if (error) {
        set_reason(codec, why);
        return error;
    }
    codec->bitpool = bitpool;
    set_reason(codec, "");
    return GC_AUDIO_OK;
}

/* --- Adaptive bitpool (AUDIO-03) -------------------------------------------
 * Reworked from the sibling A2DP implementation's rate controller (HB
 * src/a2dp/rate.h:33-49, rate.c:32-132). Two things change on purpose: the
 * band is the negotiated window intersected with the owner's floor/ceiling, so
 * a pick is always a value the strict setter accepts; and nothing is clamped
 * into place silently - a refusal is counted and disarms the controller.
 */
static void rate_band(const GcAudioCodec *codec, int floor, int ceiling, int *lo, int *hi)
{
    const int wlo = codec->selected.bitpool_min;
    const int whi = codec->selected.bitpool_max;
    const int cap = bitpool_cap(codec->selected.channel_mode, codec->selected.subbands);
    int l = wlo > floor ? wlo : floor;
    int h = whi < ceiling ? whi : ceiling;
    if (h < wlo) h = wlo;          /* never below the window */
    if (cap > 0 && h > cap) h = cap;
    if (l > h) l = h;              /* a band outside the window collapses onto it */
    *lo = l;
    *hi = h;
}

GcAudioError gc_audio_codec_rate_init(GcAudioCodec *codec, uint8_t floor, uint8_t ceiling,
                                      int64_t now_ms)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (!codec->configured) {
        set_reason(codec, "encoder_unavailable");
        return GC_AUDIO_ENCODER_UNAVAILABLE;
    }
    if (floor < 2 || floor > 250 || ceiling < 2 || ceiling > 250 || floor > ceiling) {
        set_reason(codec, "rate_bounds");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    int lo = 0, hi = 0;
    rate_band(codec, floor, ceiling, &lo, &hi);
    const int was = codec->bitpool;
    int start = was;
    if (start < lo) start = lo;
    if (start > hi) start = hi;
    codec->rate_active = 0; /* settable by the owner path below */
    if (start != was) {
        const GcAudioError error = gc_audio_codec_set_bitpool(codec, (uint8_t)start);
        if (error) return error;
    }
    codec->rate_active = 1;
    codec->rate_lo = lo;
    codec->rate_hi = hi;
    codec->rate_cur = start;
    codec->rate_cap = 0;
    codec->rate_t_down = now_ms - GC_AUDIO_CODEC_RATE_UP_CALM_MS;
    codec->rate_t_up = now_ms - GC_AUDIO_CODEC_RATE_UP_GAP_MS;
    codec->rate_t_calm = now_ms;
    codec->rate_t_cap = now_ms - GC_AUDIO_CODEC_RATE_CAP_HOLD_MS;
    codec->rate_t_late = -1;
    codec->rate_hold_until = now_ms;
    codec->rate_hold = 0;
    codec->rate_last_drops = codec->media_dropped;
    if (start > was) codec->rate_ups++;
    if (start < was) codec->rate_downs++;
    set_reason(codec, "rate_armed");
    return GC_AUDIO_OK;
}

static int rate_apply(GcAudioCodec *codec, int value, GcAudioCodecRateTick *tick)
{
    const int was = codec->bitpool;
    char why[48];
    /* The same window check the strict setter performs, without its
     * single-writer refusal: the controller is the writer here. */
    const GcAudioError error = check_selected(&codec->selected, (uint8_t)value, why, sizeof why);
    if (error) {
        /* Impossible by construction: the band is inside the window. If the
         * window moved anyway, stop rather than retry the same refusal. */
        codec->rate_refused++;
        codec->rate_active = 0;
        tick->refused = 1;
        tick->error = error;
        set_reason(codec, why);
        return 0;
    }
    codec->bitpool = (uint8_t)value;
    codec->rate_cur = value;
    tick->step += value - was;
    set_reason(codec, "rate_step");
    return 1;
}

GcAudioCodecRateTick gc_audio_codec_rate_update(GcAudioCodec *codec, int64_t now_ms, int queue,
                                                int qmax, uint32_t drops)
{
    GcAudioCodecRateTick tick;
    memset(&tick, 0, sizeof tick);
    tick.bitpool = codec ? codec->bitpool : 0;
    if (!codec) {
        tick.error = GC_AUDIO_INVALID_ARGUMENT;
        return tick;
    }
    if (!alive(codec)) {
        set_reason(codec, "shut_down");
        tick.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return tick;
    }
    if (!codec->rate_active) {
        set_reason(codec, "rate_inactive");
        tick.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return tick;
    }
    if (queue < 0 || qmax < 0 || drops < codec->rate_last_drops) {
        set_reason(codec, "bad_observation");
        tick.error = GC_AUDIO_INVALID_ARGUMENT;
        return tick;
    }
    const int dropped = drops > codec->rate_last_drops;
    int late = queue - GC_AUDIO_CODEC_RATE_SLACK;
    int room = qmax - GC_AUDIO_CODEC_RATE_SLACK;
    if (room < 2) room = 2;
    int64_t late_ms = 0;
    int want = 0;
    codec->rate_last_drops = drops;

    if (now_ms < codec->rate_hold_until) {          /* a dip: floor, nothing else */
        codec->rate_hold = 1;
        if (codec->rate_cur != codec->rate_lo) {
            if (rate_apply(codec, codec->rate_lo, &tick)) codec->rate_downs++;
        }
        codec->rate_t_calm = codec->rate_hold_until;
        codec->rate_t_late = -1;
        if (!tick.refused) tick.error = GC_AUDIO_OK;
        tick.bitpool = codec->bitpool;
        return tick;
    }
    codec->rate_hold = 0;
    /* Late packets count only once they persist: a burst that drains by
     * itself is not a slow link. */
    if (late > 0) {
        if (codec->rate_t_late < 0) codec->rate_t_late = now_ms;
        late_ms = now_ms - codec->rate_t_late;
    } else {
        codec->rate_t_late = -1;
    }
    if (dropped) {
        want = -4;
    } else if (late > 0 && late * 2 >= room && late_ms >= GC_AUDIO_CODEC_RATE_LATE_BIG_MS) {
        want = -3;
    } else if (late > 0 && late_ms >= GC_AUDIO_CODEC_RATE_LATE_MS) {
        want = -1;
    }
    if (want < 0) {
        codec->rate_t_calm = now_ms;
        if (now_ms - codec->rate_t_down >= GC_AUDIO_CODEC_RATE_DOWN_MS &&
            codec->rate_cur > codec->rate_lo) {
            int next = codec->rate_cur + want;
            if (next < codec->rate_lo) next = codec->rate_lo;
            if (want <= -3) {          /* this bitpool was too much for the link */
                codec->rate_cap = codec->rate_cur - 1;
                codec->rate_t_cap = now_ms;
            }
            if (rate_apply(codec, next, &tick)) {
                codec->rate_t_down = now_ms;
                codec->rate_t_late = late > 0 ? now_ms : -1;
                codec->rate_downs++;
            }
        }
        if (!tick.refused) tick.error = GC_AUDIO_OK;
        tick.bitpool = codec->bitpool;
        return tick;
    }
    if (late_ms >= GC_AUDIO_CODEC_RATE_LATE_CALM_MS) {  /* not calm */
        codec->rate_t_calm = now_ms;
        if (!tick.refused) tick.error = GC_AUDIO_OK;
        tick.bitpool = codec->bitpool;
        return tick;
    }
    if (now_ms - codec->rate_t_calm >= GC_AUDIO_CODEC_RATE_UP_CALM_MS &&
        now_ms - codec->rate_t_up >= GC_AUDIO_CODEC_RATE_UP_GAP_MS &&
        now_ms - codec->rate_t_down >= GC_AUDIO_CODEC_RATE_UP_CALM_MS &&
        codec->rate_cur < codec->rate_hi) {
        int top = codec->rate_hi;
        if (codec->rate_cap && now_ms - codec->rate_t_cap < GC_AUDIO_CODEC_RATE_CAP_HOLD_MS &&
            codec->rate_cap < top) {
            top = codec->rate_cap;
        }
        if (codec->rate_cur < top) {
            const int inc = codec->rate_cur + 8 <= top ? 2 : 1;
            int next = codec->rate_cur + inc;
            if (next > top) next = top;
            if (rate_apply(codec, next, &tick)) {
                codec->rate_t_up = now_ms;
                codec->rate_ups++;
            }
        }
    }
    if (!tick.refused) tick.error = GC_AUDIO_OK;
    tick.bitpool = codec->bitpool;
    return tick;
}

GcAudioError gc_audio_codec_rate_hold_floor(GcAudioCodec *codec, int64_t now_ms, int64_t until_ms)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (!codec->rate_active) {
        set_reason(codec, "rate_inactive");
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    if (until_ms <= now_ms) {
        set_reason(codec, "rate_hold_expired");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    codec->rate_hold_until = until_ms;
    GcAudioCodecRateTick tick;
    memset(&tick, 0, sizeof tick);
    if (codec->rate_cur != codec->rate_lo) {
        if (rate_apply(codec, codec->rate_lo, &tick)) codec->rate_downs++;
    }
    codec->rate_hold = 1;
    codec->rate_t_calm = until_ms;
    codec->rate_t_late = -1;
    set_reason(codec, "rate_held");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_rate_stop(GcAudioCodec *codec)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    codec->rate_active = 0;
    codec->rate_hold = 0;
    codec->rate_cap = 0;
    set_reason(codec, "rate_stopped");
    return GC_AUDIO_OK;
}

static int occupied(const GcAudioCodec *codec)
{
    return codec->sig_count + codec->hid_count + codec->media_count;
}

static void drop_oldest_media(GcAudioCodec *codec)
{
    if (codec->media_count <= 0) return;
    const GcAudioMediaSlot *slot = &codec->media[codec->media_head];
    codec->media_dropped++;
    codec->media_dropped_samples += slot->samples;
    codec->media_head = (codec->media_head + 1) % GC_AUDIO_CODEC_MEDIA_MAX;
    codec->media_count--;
}

GcAudioError gc_audio_codec_set_bounds(GcAudioCodec *codec, int signaling_cap, int hid_cap,
                                       int media_cap, int shared_cap)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (signaling_cap < 1 || signaling_cap > GC_AUDIO_CODEC_SIG_MAX || hid_cap < 1 ||
        hid_cap > GC_AUDIO_CODEC_HID_MAX || media_cap < 1 || media_cap > GC_AUDIO_CODEC_MEDIA_MAX ||
        shared_cap < 1 || shared_cap > signaling_cap + hid_cap + media_cap) {
        set_reason(codec, "bad_bounds");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    if (codec->sig_count > signaling_cap || codec->hid_count > hid_cap ||
        codec->sig_count + codec->hid_count > shared_cap) {
        set_reason(codec, "would_drop_priority");
        return GC_AUDIO_QUEUE_FULL;
    }
    codec->sig_cap = signaling_cap;
    codec->hid_cap = hid_cap;
    codec->media_cap = media_cap;
    codec->shared_cap = shared_cap;
    while (codec->media_count > media_cap || occupied(codec) > shared_cap) {
        if (codec->media_count <= 0) {
            set_reason(codec, "bounds_unsatisfiable");
            return GC_AUDIO_QUEUE_FULL;
        }
        drop_oldest_media(codec);
    }
    set_reason(codec, "");
    return GC_AUDIO_OK;
}

/* CRC-8 of Appendix B.6.1.1. Polynomial x^8+x^4+x^3+x^2+1, initial state 0x0F.
 * Data bit is XOR'd with b7; the result enters b0; taps match the diagram. */
static void crc_bit(uint8_t *crc, int bit)
{
    const int feedback = ((*crc >> 7) & 1) ^ (bit & 1);
    *crc = (uint8_t)(*crc << 1);
    if (feedback) *crc = (uint8_t)(*crc ^ 0x1Du);
}

static void crc_bits(uint8_t *crc, unsigned value, int width)
{
    for (int i = width - 1; i >= 0; --i) crc_bit(crc, (int)((value >> i) & 1u));
}

typedef struct {
    uint8_t *bytes;
    size_t cap;
    size_t n;
    unsigned acc;
    int filled;
    int overflow;
} BitWriter;

static void bw_put(BitWriter *w, unsigned value, int width)
{
    if (width < 0 || width > 16) {
        w->overflow = 1;
        return;
    }
    for (int i = width - 1; i >= 0; --i) {
        w->acc = (w->acc << 1) | ((value >> i) & 1u);
        w->filled++;
        if (w->filled == 8) {
            if (w->n >= w->cap) {
                w->overflow = 1;
                return;
            }
            w->bytes[w->n++] = (uint8_t)w->acc;
            w->acc = 0;
            w->filled = 0;
        }
    }
}

static void bw_pad(BitWriter *w)
{
    if (w->filled) bw_put(w, 0, 8 - w->filled);
}

/* Figure 8.5. One block of M new samples, oldest of the block first. */
static void analyze_block(double *history, const double *proto, int subbands, const double *samples, double *out)
{
    const int length = 10 * subbands;
    for (int i = length - 1; i >= subbands; --i) history[i] = history[i - subbands];
    for (int i = subbands - 1; i >= 0; --i) history[i] = samples[subbands - 1 - i];
    double windowed[80];
    for (int i = 0; i < length; ++i) windowed[i] = proto[i] * history[i];
    double partial[16];
    const int span = 2 * subbands;
    for (int i = 0; i < span; ++i) {
        double sum = 0.0;
        for (int k = 0; k < 5; ++k) sum += windowed[i + k * span];
        partial[i] = sum;
    }
    const double half = (double)subbands / 2.0;
    for (int sb = 0; sb < subbands; ++sb) {
        double sum = 0.0;
        for (int k = 0; k < span; ++k) {
            const double angle = ((double)sb + 0.5) * ((double)k - half) * 3.141592653589793 / (double)subbands;
            sum += cos(angle) * partial[k];
        }
        out[sb] = sum;
    }
}

static int scale_factor_of(double max_abs)
{
    int sf = 0;
    if (max_abs < 0.0) max_abs = -max_abs;
    while (sf < 15 && ldexp(1.0, sf + 1) < max_abs) ++sf;
    if (ldexp(1.0, sf + 1) < max_abs) return -1;
    return sf;
}

/* B.6.3. The published mono listing writes bitneedn once; the stereo listing
 * and the glossary name that counter bitneed. A second array would leave the
 * real counter stale, so both paths write bitneed. The stereo maximum search
 * is completed the same way the mono search is written. */
static int allocate_mono(const int *scale, int subbands, int freq, int snr, int bitpool, int *bits)
{
    int bitneed[8];
    int max_bitneed = 0;
    for (int sb = 0; sb < subbands; ++sb) {
        if (snr) {
            bitneed[sb] = scale[sb];
        } else if (scale[sb] == 0) {
            bitneed[sb] = -5;
        } else {
            const int loudness = scale[sb] - (subbands == 4 ? kOffset4[freq][sb] : kOffset8[freq][sb]);
            bitneed[sb] = loudness > 0 ? loudness / 2 : loudness;
        }
        if (bitneed[sb] > max_bitneed) max_bitneed = bitneed[sb];
    }
    int bitcount = 0;
    int slicecount = 0;
    int bitslice = max_bitneed + 1;
    int guard = 0;
    do {
        if (++guard > 64) return -1;
        --bitslice;
        bitcount += slicecount;
        slicecount = 0;
        for (int sb = 0; sb < subbands; ++sb) {
            if (bitneed[sb] > bitslice + 1 && bitneed[sb] < bitslice + 16) ++slicecount;
            else if (bitneed[sb] == bitslice + 1) slicecount += 2;
        }
    } while (bitcount + slicecount < bitpool);
    if (bitcount + slicecount == bitpool) {
        bitcount += slicecount;
        --bitslice;
    }
    for (int sb = 0; sb < subbands; ++sb) {
        if (bitneed[sb] < bitslice + 2) bits[sb] = 0;
        else {
            const int take = bitneed[sb] - bitslice;
            bits[sb] = take < 16 ? take : 16;
        }
    }
    int sb = 0;
    while (bitcount < bitpool && sb < subbands) {
        if (bits[sb] >= 2 && bits[sb] < 16) {
            ++bits[sb];
            ++bitcount;
        } else if (bitneed[sb] == bitslice + 1 && bitpool > bitcount + 1) {
            bits[sb] = 2;
            bitcount += 2;
        }
        ++sb;
    }
    sb = 0;
    while (bitcount < bitpool && sb < subbands) {
        if (bits[sb] < 16) {
            ++bits[sb];
            ++bitcount;
        }
        ++sb;
    }
    int sum = 0;
    for (int i = 0; i < subbands; ++i) sum += bits[i];
    return sum == bitpool ? 0 : -1;
}

static int allocate_stereo(const int scale[2][8], int subbands, int freq, int snr, int bitpool, int bits[2][8])
{
    int bitneed[2][8];
    int max_bitneed = 0;
    for (int ch = 0; ch < 2; ++ch) {
        for (int sb = 0; sb < subbands; ++sb) {
            if (snr) {
                bitneed[ch][sb] = scale[ch][sb];
            } else if (scale[ch][sb] == 0) {
                bitneed[ch][sb] = -5;
            } else {
                const int loudness =
                    scale[ch][sb] - (subbands == 4 ? kOffset4[freq][sb] : kOffset8[freq][sb]);
                bitneed[ch][sb] = loudness > 0 ? loudness / 2 : loudness;
            }
            if (bitneed[ch][sb] > max_bitneed) max_bitneed = bitneed[ch][sb];
        }
    }
    int bitcount = 0;
    int slicecount = 0;
    int bitslice = max_bitneed + 1;
    int guard = 0;
    do {
        if (++guard > 64) return -1;
        --bitslice;
        bitcount += slicecount;
        slicecount = 0;
        for (int ch = 0; ch < 2; ++ch) {
            for (int sb = 0; sb < subbands; ++sb) {
                if (bitneed[ch][sb] > bitslice + 1 && bitneed[ch][sb] < bitslice + 16) ++slicecount;
                else if (bitneed[ch][sb] == bitslice + 1) slicecount += 2;
            }
        }
    } while (bitcount + slicecount < bitpool);
    if (bitcount + slicecount == bitpool) {
        bitcount += slicecount;
        --bitslice;
    }
    for (int ch = 0; ch < 2; ++ch) {
        for (int sb = 0; sb < subbands; ++sb) {
            if (bitneed[ch][sb] < bitslice + 2) bits[ch][sb] = 0;
            else {
                const int take = bitneed[ch][sb] - bitslice;
                bits[ch][sb] = take < 16 ? take : 16;
            }
        }
    }
    int ch = 0;
    int sb = 0;
    while (bitcount < bitpool && sb < subbands) {
        if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
            ++bits[ch][sb];
            ++bitcount;
        } else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1) {
            bits[ch][sb] = 2;
            bitcount += 2;
        }
        if (ch == 1) {
            ch = 0;
            ++sb;
        } else {
            ch = 1;
        }
    }
    ch = 0;
    sb = 0;
    while (bitcount < bitpool && sb < subbands) {
        if (bits[ch][sb] < 16) {
            ++bits[ch][sb];
            ++bitcount;
        }
        if (ch == 1) {
            ch = 0;
            ++sb;
        } else {
            ch = 1;
        }
    }
    int sum = 0;
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < subbands; ++s) sum += bits[c][s];
    return sum == bitpool ? 0 : -1;
}

static int quantize_subband(const double *samples, int blocks, int sf, int nbits, unsigned *out)
{
    if (nbits <= 0) return 0;
    if (nbits > 16) return -1;
    const double scalefactor = ldexp(1.0, sf + 1);
    const unsigned levels = (1u << nbits) - 1u;
    for (int blk = 0; blk < blocks; ++blk) {
        const double q = floor(((samples[blk] / scalefactor + 1.0) * (double)levels) / 2.0);
        if (q < -1e-6 || q > (double)levels + 1e-6) return -1;
        double clipped = q;
        if (clipped < 0.0) clipped = 0.0;
        if (clipped > (double)levels) clipped = (double)levels;
        out[blk] = (unsigned)clipped;
    }
    return 0;
}

GcAudioError gc_audio_sbc_encode(GcAudioCodec *codec, const int16_t *pcm, uint8_t *out, size_t cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!codec || !pcm || !out) return GC_AUDIO_INVALID_ARGUMENT;
    if (codec->shut_down) {
        set_reason(codec, "shut_down");
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    if (!codec->configured) {
        set_reason(codec, "encoder_unavailable");
        return GC_AUDIO_ENCODER_UNAVAILABLE;
    }
    const GcAvdtpSbcConfig *cfg = &codec->selected;
    const int channels = channels_of(cfg->channel_mode);
    const int blocks = cfg->blocks;
    const int subbands = cfg->subbands;
    const int freq = rate_index(cfg->sample_rate);
    const int snr = cfg->allocation == GC_AVDTP_ALLOC_SNR;
    const int joint = cfg->channel_mode == GC_AVDTP_MODE_JOINT;
    const int dual = cfg->channel_mode == GC_AVDTP_MODE_MONO || cfg->channel_mode == GC_AVDTP_MODE_DUAL;
    uint32_t frame_len = 0;
    if (gc_audio_avdtp_sbc_frame_bytes(cfg->channel_mode, cfg->blocks, cfg->subbands, codec->bitpool, &frame_len) ||
        frame_len == 0 || frame_len > GC_AUDIO_CODEC_FRAME_MAX || cap < frame_len) {
        set_reason(codec, cap < frame_len ? "short_buffer" : "frame_bounds");
        return cap < frame_len ? GC_AUDIO_INVALID_ARGUMENT : GC_AUDIO_UNSUPPORTED_FORMAT;
    }

    double saved[2][80];
    memcpy(saved, codec->history, sizeof saved);
    const double *proto = subbands == 4 ? kProto4 : kProto8;
    double sb[16][2][8];
    for (int blk = 0; blk < blocks; ++blk) {
        for (int ch = 0; ch < channels; ++ch) {
            double samples[8];
            for (int i = 0; i < subbands; ++i) {
                samples[i] = (double)pcm[((blk * subbands + i) * channels) + ch];
            }
            analyze_block(codec->history[ch], proto, subbands, samples, sb[blk][ch]);
        }
    }

    int join[8];
    for (int s = 0; s < 8; ++s) join[s] = 0;
    if (joint) {
        for (int s = 0; s < subbands - 1; ++s) {
            double left_max = 0.0, right_max = 0.0, sum_max = 0.0, diff_max = 0.0;
            for (int blk = 0; blk < blocks; ++blk) {
                const double left = sb[blk][0][s];
                const double right = sb[blk][1][s];
                const double sum = (left + right) / 2.0;
                const double diff = (left - right) / 2.0;
                if (fabs(left) > left_max) left_max = fabs(left);
                if (fabs(right) > right_max) right_max = fabs(right);
                if (fabs(sum) > sum_max) sum_max = fabs(sum);
                if (fabs(diff) > diff_max) diff_max = fabs(diff);
            }
            const int raw = scale_factor_of(left_max) + scale_factor_of(right_max);
            const int coded = scale_factor_of(sum_max) + scale_factor_of(diff_max);
            if (raw > coded) {
                join[s] = 1;
                for (int blk = 0; blk < blocks; ++blk) {
                    const double left = sb[blk][0][s];
                    const double right = sb[blk][1][s];
                    sb[blk][0][s] = (left + right) / 2.0;
                    sb[blk][1][s] = (left - right) / 2.0;
                }
            }
        }
    }

    int scale[2][8];
    for (int ch = 0; ch < channels; ++ch) {
        for (int s = 0; s < subbands; ++s) {
            double max_abs = 0.0;
            for (int blk = 0; blk < blocks; ++blk) {
                const double v = fabs(sb[blk][ch][s]);
                if (v > max_abs) max_abs = v;
            }
            scale[ch][s] = scale_factor_of(max_abs);
            if (scale[ch][s] < 0) {
                memcpy(codec->history, saved, sizeof saved);
                set_reason(codec, "scale_overflow");
                return GC_AUDIO_UNSUPPORTED_FORMAT;
            }
        }
    }

    int bits[2][8];
    memset(bits, 0, sizeof bits);
    int alloc_ok;
    if (dual) {
        alloc_ok = 0;
        for (int ch = 0; ch < channels; ++ch) {
            if (allocate_mono(scale[ch], subbands, freq, snr, codec->bitpool, bits[ch])) alloc_ok = -1;
        }
    } else {
        alloc_ok = allocate_stereo(scale, subbands, freq, snr, codec->bitpool, bits);
    }
    if (alloc_ok) {
        memcpy(codec->history, saved, sizeof saved);
        set_reason(codec, "bit_allocation");
        return GC_AUDIO_UNSUPPORTED_FORMAT;
    }

    unsigned audio[16][2][8];
    memset(audio, 0, sizeof audio);
    for (int ch = 0; ch < channels; ++ch) {
        for (int s = 0; s < subbands; ++s) {
            double column[16];
            for (int blk = 0; blk < blocks; ++blk) column[blk] = sb[blk][ch][s];
            /* Zero-bit subbands do not call the quantizer's write loop. */
            unsigned q[16] = {0};
            if (quantize_subband(column, blocks, scale[ch][s], bits[ch][s], q)) {
                memcpy(codec->history, saved, sizeof saved);
                set_reason(codec, "quantize");
                return GC_AUDIO_UNSUPPORTED_FORMAT;
            }
            for (int blk = 0; blk < blocks; ++blk) audio[blk][ch][s] = q[blk];
        }
    }

    const int bidx = blocks_index((uint8_t)blocks);
    const int sbidx = subbands == 8 ? 1 : 0;
    const unsigned header =
        ((unsigned)freq << 6) | ((unsigned)bidx << 4) | ((unsigned)cfg->channel_mode << 2) |
        ((unsigned)cfg->allocation << 1) | (unsigned)sbidx;

    uint8_t crc = 0x0F;
    crc_bits(&crc, header, 8);
    crc_bits(&crc, codec->bitpool, 8);
    /* Table 8.12 puts join[] after crc_check and still inside frame_header().
     * B.6.1.1 covers every frame_header bit except syncword and crc_check,
     * then the scale factors. The joint bits, including the reserved 0, are
     * in that scope. A check that omits them is rejected by an independent
     * decoder. */
    if (joint) {
        for (int s = 0; s < subbands - 1; ++s) crc_bits(&crc, (unsigned)join[s], 1);
        crc_bits(&crc, 0, 1);
    }
    for (int ch = 0; ch < channels; ++ch) {
        for (int s = 0; s < subbands; ++s) crc_bits(&crc, (unsigned)scale[ch][s], 4);
    }

    uint8_t packed[GC_AUDIO_CODEC_FRAME_MAX];
    BitWriter writer;
    memset(&writer, 0, sizeof writer);
    writer.bytes = packed;
    writer.cap = sizeof packed;
    bw_put(&writer, 0x9C, 8);
    bw_put(&writer, header, 8);
    bw_put(&writer, codec->bitpool, 8);
    bw_put(&writer, crc, 8);
    if (joint) {
        for (int s = 0; s < subbands - 1; ++s) bw_put(&writer, (unsigned)join[s], 1);
        bw_put(&writer, 0, 1);
    }
    for (int ch = 0; ch < channels; ++ch) {
        for (int s = 0; s < subbands; ++s) bw_put(&writer, (unsigned)scale[ch][s], 4);
    }
    for (int blk = 0; blk < blocks; ++blk) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int s = 0; s < subbands; ++s) {
                if (bits[ch][s]) bw_put(&writer, audio[blk][ch][s], bits[ch][s]);
            }
        }
    }
    bw_pad(&writer);
    if (writer.overflow || writer.n != frame_len) {
        memcpy(codec->history, saved, sizeof saved);
        set_reason(codec, "pack_length");
        return GC_AUDIO_UNSUPPORTED_FORMAT;
    }
    memcpy(out, packed, frame_len);
    if (out_len) *out_len = frame_len;
    set_reason(codec, "");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_packet_room(const GcAudioCodec *codec, uint16_t l2cap_mtu,
                                        uint16_t acl_payload_max, uint32_t *frame_bytes, uint8_t *frames)
{
    if (frame_bytes) *frame_bytes = 0;
    if (frames) *frames = 0;
    if (!codec) return GC_AUDIO_INVALID_ARGUMENT;
    if (codec->shut_down) return GC_AUDIO_PROFILE_UNAVAILABLE;
    if (!codec->configured) return GC_AUDIO_ENCODER_UNAVAILABLE;
    uint32_t frame = 0;
    if (gc_audio_avdtp_sbc_frame_bytes(codec->selected.channel_mode, codec->selected.blocks,
                                       codec->selected.subbands, codec->bitpool, &frame) ||
        frame == 0 || frame > 65535u - GC_AVDTP_MEDIA_OVERHEAD) {
        return GC_AUDIO_UNSUPPORTED_FORMAT;
    }
    const uint32_t sdu = GC_AVDTP_MEDIA_OVERHEAD + frame;
    const uint32_t acl_need = GC_AVDTP_L2CAP_BASIC + sdu;
    if (l2cap_mtu < sdu || acl_payload_max < acl_need) return GC_AUDIO_NEGOTIATION_REJECTED;
    uint32_t fit = 1;
    while (fit < 15u) {
        const uint32_t next = GC_AVDTP_MEDIA_OVERHEAD + frame * (fit + 1u);
        if (next < frame) break;
        if (l2cap_mtu < next || acl_payload_max < GC_AVDTP_L2CAP_BASIC + next) break;
        ++fit;
    }
    if (frame_bytes) *frame_bytes = frame;
    if (frames) *frames = (uint8_t)fit;
    return GC_AUDIO_OK;
}

static int credits_available(const GcAudioCodec *codec)
{
    if (!codec->credits_known) return -1;
    if (codec->inflight >= codec->reported_credits) return 0;
    return (int)(codec->reported_credits - codec->inflight);
}

GcAudioError gc_audio_codec_report_credits(GcAudioCodec *codec, uint16_t credits)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    codec->credits_known = 1;
    codec->credits_stale = 0;
    codec->reported_credits = credits;
    set_reason(codec, "credits_reported");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_report_completed(GcAudioCodec *codec, uint16_t packets)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (!codec->credits_known) {
        set_reason(codec, "credits_unreported");
        return GC_AUDIO_RADIO_UNAVAILABLE;
    }
    if (codec->credits_stale) {
        /* A completion for a link that is gone is not evidence about the
         * next link's buffers. */
        set_reason(codec, "credits_stale");
        return GC_AUDIO_RADIO_UNAVAILABLE;
    }
    if (packets > codec->inflight) {
        codec->over_reported_completions += (uint32_t)(packets - codec->inflight);
        codec->inflight = 0;
    } else {
        codec->inflight = (uint16_t)(codec->inflight - packets);
    }
    set_reason(codec, "credits_completed");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_report_link_down(GcAudioCodec *codec, uint16_t *retired)
{
    if (retired) *retired = 0;
    if (!codec) return GC_AUDIO_INVALID_ARGUMENT;
    if (codec->shut_down) {
        set_reason(codec, "shut_down");
        return GC_AUDIO_PROFILE_UNAVAILABLE;
    }
    const uint16_t was = codec->inflight;
    codec->inflight = 0;
    codec->link_downs++;
    codec->credits_stale = 1;
    if (retired) *retired = was;
    set_reason(codec, "link_down");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_report_latency_samples(GcAudioCodec *codec, int samples)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (samples < -1) {
        set_reason(codec, "bad_latency");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    codec->reported_latency_samples = samples;
    set_reason(codec, samples < 0 ? "latency_unknown" : "latency_reported");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_report_delay_tenths(GcAudioCodec *codec, int tenths)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (tenths < -1) {
        set_reason(codec, "bad_delay");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    codec->reported_delay_tenths = tenths;
    set_reason(codec, tenths < 0 ? "delay_unknown" : "delay_reported");
    return GC_AUDIO_OK;
}

GcAudioError gc_audio_codec_advance_samples(GcAudioCodec *codec, uint32_t samples)
{
    if (!alive(codec)) return codec ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT;
    if (samples == 0) {
        set_reason(codec, "zero_samples");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    if (codec->release_samples > UINT64_MAX - samples) {
        set_reason(codec, "sample_clock_overflow");
        return GC_AUDIO_INVALID_ARGUMENT;
    }
    codec->release_samples += samples;
    set_reason(codec, "samples_advanced");
    return GC_AUDIO_OK;
}

static GcAudioCodecEnqueue enqueue_fail(GcAudioError error)
{
    GcAudioCodecEnqueue result;
    memset(&result, 0, sizeof result);
    result.error = error;
    return result;
}

static int make_room(GcAudioCodec *codec, int kind)
{
    const int class_full = kind == GC_AUDIO_KIND_SIGNALING ? codec->sig_count >= codec->sig_cap
                           : kind == GC_AUDIO_KIND_HID      ? codec->hid_count >= codec->hid_cap
                                                            : codec->media_count >= codec->media_cap;
    if (class_full && kind != GC_AUDIO_KIND_MEDIA) return 0;
    while (class_full || occupied(codec) >= codec->shared_cap) {
        if (codec->media_count <= 0) return 0;
        if (kind != GC_AUDIO_KIND_MEDIA && !class_full && occupied(codec) < codec->shared_cap) break;
        drop_oldest_media(codec);
        if (kind == GC_AUDIO_KIND_MEDIA && codec->media_count < codec->media_cap &&
            occupied(codec) < codec->shared_cap) {
            return 1;
        }
        if (kind != GC_AUDIO_KIND_MEDIA && occupied(codec) < codec->shared_cap) return 1;
        if (kind == GC_AUDIO_KIND_MEDIA && codec->media_count >= codec->media_cap) continue;
        if (occupied(codec) >= codec->shared_cap && codec->media_count > 0) continue;
        return 0;
    }
    return 1;
}

GcAudioCodecEnqueue gc_audio_codec_enqueue_signaling(GcAudioCodec *codec, const uint8_t *bytes, size_t len)
{
    if (!alive(codec) || !bytes || len == 0 || len > GC_AUDIO_CODEC_SIG_SLOT) {
        return enqueue_fail(codec && codec->shut_down ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT);
    }
    GcAudioCodecEnqueue result;
    memset(&result, 0, sizeof result);
    if (codec->sig_count >= codec->sig_cap) {
        codec->signaling_refused++;
        set_reason(codec, "signaling_full");
        result.error = GC_AUDIO_QUEUE_FULL;
        return result;
    }
    const int before = codec->media_dropped;
    if (!make_room(codec, GC_AUDIO_KIND_SIGNALING) || occupied(codec) >= codec->shared_cap) {
        codec->signaling_refused++;
        set_reason(codec, "signaling_no_slot");
        result.error = GC_AUDIO_QUEUE_FULL;
        result.dropped_media = codec->media_dropped - before;
        return result;
    }
    result.dropped_media = codec->media_dropped - before;
    const int index = (codec->sig_head + codec->sig_count) % GC_AUDIO_CODEC_SIG_MAX;
    memcpy(codec->signaling[index].bytes, bytes, len);
    codec->signaling[index].len = (uint16_t)len;
    codec->sig_count++;
    set_reason(codec, result.dropped_media ? "dropped_media_for_signaling" : "");
    result.error = GC_AUDIO_OK;
    return result;
}

GcAudioCodecEnqueue gc_audio_codec_enqueue_hid(GcAudioCodec *codec, const uint8_t *bytes, size_t len)
{
    if (!alive(codec) || !bytes || len == 0 || len > GC_AUDIO_CODEC_HID_SLOT) {
        return enqueue_fail(codec && codec->shut_down ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT);
    }
    GcAudioCodecEnqueue result;
    memset(&result, 0, sizeof result);
    if (codec->hid_count >= codec->hid_cap) {
        codec->hid_refused++;
        set_reason(codec, "hid_full");
        result.error = GC_AUDIO_QUEUE_FULL;
        return result;
    }
    const int before = codec->media_dropped;
    if (!make_room(codec, GC_AUDIO_KIND_HID) || occupied(codec) >= codec->shared_cap) {
        codec->hid_refused++;
        set_reason(codec, "hid_no_slot");
        result.error = GC_AUDIO_QUEUE_FULL;
        result.dropped_media = codec->media_dropped - before;
        return result;
    }
    result.dropped_media = codec->media_dropped - before;
    const int index = (codec->hid_head + codec->hid_count) % GC_AUDIO_CODEC_HID_MAX;
    memcpy(codec->hid[index].bytes, bytes, len);
    codec->hid[index].len = (uint16_t)len;
    codec->hid_count++;
    set_reason(codec, result.dropped_media ? "dropped_media_for_hid" : "");
    result.error = GC_AUDIO_OK;
    return result;
}

GcAudioCodecEnqueue gc_audio_codec_enqueue_pcm(GcAudioCodec *codec, const int16_t *pcm)
{
    if (!alive(codec) || !pcm) {
        return enqueue_fail(codec && codec->shut_down ? GC_AUDIO_PROFILE_UNAVAILABLE : GC_AUDIO_INVALID_ARGUMENT);
    }
    if (!codec->configured) {
        set_reason(codec, "encoder_unavailable");
        return enqueue_fail(GC_AUDIO_ENCODER_UNAVAILABLE);
    }
    const int needs_drop = codec->media_count >= codec->media_cap || occupied(codec) >= codec->shared_cap;
    if (needs_drop && codec->media_count <= 0) {
        set_reason(codec, "media_no_slot");
        return enqueue_fail(GC_AUDIO_QUEUE_FULL);
    }
    uint8_t frame[GC_AUDIO_CODEC_FRAME_MAX];
    size_t len = 0;
    const GcAudioError encoded = gc_audio_sbc_encode(codec, pcm, frame, sizeof frame, &len);
    if (encoded) return enqueue_fail(encoded);
    GcAudioCodecEnqueue result;
    memset(&result, 0, sizeof result);
    const int before = codec->media_dropped;
    const uint32_t before_samples = codec->media_dropped_samples;
    if (codec->media_count >= codec->media_cap || occupied(codec) >= codec->shared_cap) {
        if (!make_room(codec, GC_AUDIO_KIND_MEDIA) || codec->media_count >= codec->media_cap ||
            occupied(codec) >= codec->shared_cap) {
            set_reason(codec, "media_no_slot");
            result.error = GC_AUDIO_QUEUE_FULL;
            result.dropped_media = codec->media_dropped - before;
            result.dropped_samples = codec->media_dropped_samples - before_samples;
            return result;
        }
    }
    result.dropped_media = codec->media_dropped - before;
    result.dropped_samples = codec->media_dropped_samples - before_samples;
    const int index = (codec->media_head + codec->media_count) % GC_AUDIO_CODEC_MEDIA_MAX;
    memcpy(codec->media[index].bytes, frame, len);
    codec->media[index].len = (uint16_t)len;
    codec->media[index].samples = codec->frame_samples;
    codec->media_count++;
    set_reason(codec, result.dropped_media ? "media_dropped" : "");
    result.error = GC_AUDIO_OK;
    return result;
}

GcAudioCodecPull gc_audio_codec_pull(GcAudioCodec *codec, uint8_t *out, size_t cap)
{
    GcAudioCodecPull pull;
    memset(&pull, 0, sizeof pull);
    pull.kind = GC_AUDIO_KIND_NONE;
    if (!codec || !out) {
        pull.error = GC_AUDIO_INVALID_ARGUMENT;
        return pull;
    }
    if (codec->shut_down) {
        set_reason(codec, "shut_down");
        pull.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return pull;
    }
    const int available = credits_available(codec);
    if (available < 0) {
        set_reason(codec, "credits_unreported");
        pull.error = GC_AUDIO_RADIO_UNAVAILABLE;
        return pull;
    }
    if (codec->credits_stale) {
        /* The link this budget described is gone; nothing may be sent on it. */
        set_reason(codec, "credits_stale");
        pull.error = GC_AUDIO_RADIO_UNAVAILABLE;
        return pull;
    }
    if (available == 0) {
        set_reason(codec, "no_credit");
        pull.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return pull;
    }
    const uint8_t *src = NULL;
    size_t len = 0;
    int kind = GC_AUDIO_KIND_NONE;
    uint32_t samples = 0;
    if (codec->sig_count > 0) {
        src = codec->signaling[codec->sig_head].bytes;
        len = codec->signaling[codec->sig_head].len;
        kind = GC_AUDIO_KIND_SIGNALING;
    } else if (codec->hid_count > 0) {
        src = codec->hid[codec->hid_head].bytes;
        len = codec->hid[codec->hid_head].len;
        kind = GC_AUDIO_KIND_HID;
    } else if (codec->media_count > 0) {
        const GcAudioMediaSlot *slot = &codec->media[codec->media_head];
        if (codec->release_samples < slot->samples) {
            set_reason(codec, "awaiting_samples");
            pull.error = GC_AUDIO_PROFILE_UNAVAILABLE;
            return pull;
        }
        src = slot->bytes;
        len = slot->len;
        samples = slot->samples;
        kind = GC_AUDIO_KIND_MEDIA;
    } else {
        set_reason(codec, "queue_empty");
        pull.error = GC_AUDIO_PROFILE_UNAVAILABLE;
        return pull;
    }
    if (len > cap) {
        set_reason(codec, "short_buffer");
        pull.error = GC_AUDIO_INVALID_ARGUMENT;
        return pull;
    }
    memcpy(out, src, len);
    if (kind == GC_AUDIO_KIND_SIGNALING) {
        codec->sig_head = (codec->sig_head + 1) % GC_AUDIO_CODEC_SIG_MAX;
        codec->sig_count--;
        codec->sent_signaling++;
    } else if (kind == GC_AUDIO_KIND_HID) {
        codec->hid_head = (codec->hid_head + 1) % GC_AUDIO_CODEC_HID_MAX;
        codec->hid_count--;
        codec->sent_hid++;
    } else {
        codec->release_samples -= samples;
        codec->media_head = (codec->media_head + 1) % GC_AUDIO_CODEC_MEDIA_MAX;
        codec->media_count--;
        codec->sent_media++;
    }
    codec->inflight++;
    set_reason(codec, "sent");
    pull.error = GC_AUDIO_OK;
    pull.length = len;
    pull.kind = kind;
    pull.samples = samples;
    return pull;
}

void gc_audio_codec_snapshot(const GcAudioCodec *codec, GcAudioCodecSnapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->ordinary_host_implemented = 1;
    out->estimate_credits = -1;
    out->estimate_ms = -1;
    out->reported_latency_samples = -1;
    out->reported_delay_tenths = -1;
    out->reported_credits = -1;
    out->available_credits = -1;
    if (!codec) return;
    out->shut_down = codec->shut_down;
    out->configured = codec->configured && !codec->shut_down;
    out->encoder_available = out->configured;
    out->credits_known = codec->credits_known;
    out->inflight = codec->inflight;
    out->reported_latency_samples = codec->reported_latency_samples;
    out->reported_delay_tenths = codec->reported_delay_tenths;
    out->signaling_queued = codec->sig_count;
    out->hid_queued = codec->hid_count;
    out->media_queued = codec->media_count;
    out->signaling_cap = codec->sig_cap;
    out->hid_cap = codec->hid_cap;
    out->media_cap = codec->media_cap;
    out->shared_cap = codec->shared_cap;
    out->bitpool = codec->bitpool;
    out->bitpool_min = codec->selected.bitpool_min;
    out->bitpool_max = codec->selected.bitpool_max;
    out->frame_samples = codec->frame_samples;
    out->release_samples = codec->release_samples;
    out->media_dropped = codec->media_dropped;
    out->media_dropped_samples = codec->media_dropped_samples;
    out->signaling_refused = codec->signaling_refused;
    out->hid_refused = codec->hid_refused;
    out->sent_signaling = codec->sent_signaling;
    out->sent_hid = codec->sent_hid;
    out->sent_media = codec->sent_media;
    out->over_reported_completions = codec->over_reported_completions;
    out->rate_active = codec->rate_active && !codec->shut_down;
    out->rate_lo = codec->rate_lo;
    out->rate_hi = codec->rate_hi;
    out->rate_downs = codec->rate_downs;
    out->rate_ups = codec->rate_ups;
    out->rate_refused = codec->rate_refused;
    out->rate_held = codec->rate_hold;
    out->credits_stale = codec->credits_stale && !codec->shut_down;
    out->link_downs = codec->link_downs;
    snprintf(out->reason, sizeof out->reason, "%s", codec->reason);
    uint64_t queued = 0;
    for (int i = 0; i < codec->media_count; ++i) {
        const int index = (codec->media_head + i) % GC_AUDIO_CODEC_MEDIA_MAX;
        queued += codec->media[index].samples;
    }
    out->queued_media_samples = queued;
    if (codec->credits_known) {
        out->reported_credits = codec->reported_credits;
        /* The remembered number stays visible, but a budget whose link is gone
         * is not availability: -1, the same "unknown" a never-reported budget
         * has. Nothing may be pulled on it. */
        out->available_credits = codec->credits_stale ? -1 : credits_available(codec);
    }
}

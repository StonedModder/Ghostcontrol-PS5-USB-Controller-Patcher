/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-05 headset playback DSP. See gc_audio_dsp.h for the chain and the
 * guarantees. Coefficients follow the public RBJ Audio EQ Cookbook; dynamics
 * are textbook stereo-linked feed-forward designs.
 */
#include "gc_audio_dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Fixed voicing, deliberately not user-tunable: a bad threshold is a support
 * problem, not a preference. */
#define NIGHT_THRESHOLD 0.0316227766f /* -30 dBFS */
#define NIGHT_RATIO 3.0f
#define NIGHT_MAKEUP 1.9952623150f    /* +6 dB */
#define NIGHT_ATTACK_S 0.010
#define NIGHT_RELEASE_S 0.200
#define LIMITER_HOLD_S 0.020    /* longer than one 50 Hz period */
#define LIMITER_RELEASE_S 0.080
#define HOLD_REFRESH 0.8912509381f /* -1 dB */
#define TRANSITION_S 0.010            /* gain ramp and EQ crossfade */
#define CEILING (32767.0f / 32768.0f) /* largest positive s16, normalized */
#define TINY 1e-20f

static const unsigned band_hz[GC_AUDIO_EQ_BANDS] = {
    GC_AUDIO_EQ_LOW_HZ, GC_AUDIO_EQ_MID1_HZ, GC_AUDIO_EQ_MID2_HZ,
    GC_AUDIO_EQ_MID3_HZ, GC_AUDIO_EQ_HIGH_HZ,
};

int gc_audio_dsp_config_valid(const GcAudioDspConfig *config)
{
    if (!config) return -1;
    switch (config->sample_rate) {
    case 16000: case 32000: case 44100: case 48000: break;
    default: return -1;
    }
    if (config->channels != 1 && config->channels != 2) return -1;
    if (config->eq_enabled > 1 || config->night > 1) return -1;
    for (int i = 0; i < GC_AUDIO_EQ_BANDS; ++i)
        if (config->eq_tenths[i] < GC_AUDIO_EQ_MIN_TENTHS ||
            config->eq_tenths[i] > GC_AUDIO_EQ_MAX_TENTHS)
            return -1;
    if (config->gain_tenths < GC_AUDIO_GAIN_MIN_TENTHS ||
        config->gain_tenths > GC_AUDIO_GAIN_MAX_TENTHS)
        return -1;
    return 0;
}

GcAudioDspConfig gc_audio_dsp_neutral(uint32_t sample_rate, uint8_t channels)
{
    GcAudioDspConfig config;
    memset(&config, 0, sizeof(config));
    config.sample_rate = sample_rate;
    config.channels = channels;
    return config;
}

static float db_tenths_to_linear(int tenths)
{
    return (float)pow(10.0, (double)tenths / 200.0);
}

static GcAudioBiquad identity_biquad(void)
{
    GcAudioBiquad q = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    return q;
}

/* RBJ cookbook. Shelves use slope S=1, peaks Q=1. Band centres above 0.45 Fs
 * (the 10 kHz shelf at 16 kHz) are pulled under Nyquist. */
static GcAudioBiquad design_band(int band, int tenths, uint32_t rate)
{
    if (tenths == 0) return identity_biquad();
    double f0 = band_hz[band];
    if (f0 > 0.45 * rate) f0 = 0.45 * rate;
    const double A = pow(10.0, (double)tenths / 400.0);
    const double w0 = 2.0 * M_PI * f0 / (double)rate;
    const double c = cos(w0), s = sin(w0);
    double b0, b1, b2, a0, a1, a2;
    if (band == 0 || band == GC_AUDIO_EQ_BANDS - 1) {
        const double alpha = s / 2.0 * sqrt(2.0);
        const double k = 2.0 * sqrt(A) * alpha;
        if (band == 0) {
            b0 = A * ((A + 1) - (A - 1) * c + k);
            b1 = 2 * A * ((A - 1) - (A + 1) * c);
            b2 = A * ((A + 1) - (A - 1) * c - k);
            a0 = (A + 1) + (A - 1) * c + k;
            a1 = -2 * ((A - 1) + (A + 1) * c);
            a2 = (A + 1) + (A - 1) * c - k;
        } else {
            b0 = A * ((A + 1) + (A - 1) * c + k);
            b1 = -2 * A * ((A - 1) + (A + 1) * c);
            b2 = A * ((A + 1) + (A - 1) * c - k);
            a0 = (A + 1) - (A - 1) * c + k;
            a1 = 2 * ((A - 1) - (A + 1) * c);
            a2 = (A + 1) - (A - 1) * c - k;
        }
    } else {
        const double alpha = s / 2.0; /* Q = 1 */
        b0 = 1 + alpha * A;
        b1 = -2 * c;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * c;
        a2 = 1 - alpha / A;
    }
    GcAudioBiquad q = {(float)(b0 / a0), (float)(b1 / a0), (float)(b2 / a0),
                       (float)(a1 / a0), (float)(a2 / a0)};
    return q;
}

static void design_bank(GcAudioEqBank *bank, const GcAudioDspConfig *config)
{
    bank->active = 0;
    for (int i = 0; i < GC_AUDIO_EQ_BANDS; ++i) {
        const int tenths = config->eq_enabled ? config->eq_tenths[i] : 0;
        bank->coeff[i] = design_band(i, tenths, config->sample_rate);
        if (tenths) bank->active = 1;
    }
}

static float flush(float v) { return fabsf(v) < TINY ? 0.0f : v; }

static float run_bank(GcAudioEqBank *bank, int channel, float x)
{
    if (!bank->active) return x;
    for (int i = 0; i < GC_AUDIO_EQ_BANDS; ++i) {
        const GcAudioBiquad *q = &bank->coeff[i];
        if (q->b0 == 1.0f && q->b1 == 0.0f && q->b2 == 0.0f && q->a1 == 0.0f && q->a2 == 0.0f)
            continue;
        const float y = q->b0 * x + bank->z1[i][channel];
        bank->z1[i][channel] = flush(q->b1 * x - q->a1 * y + bank->z2[i][channel]);
        bank->z2[i][channel] = flush(q->b2 * x - q->a2 * y);
        x = y;
    }
    return x;
}

static uint32_t transition_frames(uint32_t rate);

static int config_neutral(const GcAudioDspConfig *config)
{
    if (config->gain_tenths || config->night) return 0;
    if (!config->eq_enabled) return 1;
    for (int i = 0; i < GC_AUDIO_EQ_BANDS; ++i)
        if (config->eq_tenths[i]) return 0;
    return 1;
}

static void refresh_passthrough(GcAudioDsp *dsp)
{
    /* The audible bank must itself be flat: the config can already be
     * neutral while a fade toward it is still queued behind a finished one.
     * (A queued bank always reflects the latest config, so with a neutral
     * config and a flat audible bank nothing non-neutral can be pending.) */
    dsp->passthrough = config_neutral(&dsp->config) && dsp->gain_ramp_left == 0 &&
                       dsp->crossfade_left == 0 && !dsp->eq.active &&
                       dsp->limiter_gain >= 1.0f && dsp->night_gain == 1.0;
}

static int same_bank(const GcAudioEqBank *a, const GcAudioEqBank *b)
{
    return a->active == b->active && !memcmp(a->coeff, b->coeff, sizeof(a->coeff));
}

/* Starts a crossfade from the fully audible current bank to `next`. The new
 * bank starts from the old history so low bands settle quickly. */
static void begin_crossfade(GcAudioDsp *dsp, const GcAudioEqBank *next)
{
    GcAudioEqBank incoming = *next;
    memcpy(incoming.z1, dsp->eq.z1, sizeof(incoming.z1));
    memcpy(incoming.z2, dsp->eq.z2, sizeof(incoming.z2));
    dsp->eq_previous = dsp->eq;
    dsp->eq = incoming;
    dsp->crossfade_total = dsp->crossfade_left = transition_frames(dsp->config.sample_rate);
}

static uint32_t transition_frames(uint32_t rate)
{
    return (uint32_t)(rate * TRANSITION_S);
}

int gc_audio_dsp_init(GcAudioDsp *dsp, const GcAudioDspConfig *config)
{
    if (!dsp) return -1;
    memset(dsp, 0, sizeof(*dsp));
    if (gc_audio_dsp_config_valid(config)) return -1;
    dsp->config = *config;
    design_bank(&dsp->eq, config);
    const double rate = (double)config->sample_rate;
    dsp->night_attack = (float)exp(-1.0 / (NIGHT_ATTACK_S * rate));
    dsp->night_release = (float)exp(-1.0 / (NIGHT_RELEASE_S * rate));
    dsp->limiter_release = (float)exp(-1.0 / (LIMITER_RELEASE_S * rate));
    dsp->limiter_hold = (uint32_t)(LIMITER_HOLD_S * rate);
    dsp->gain_target = dsp->gain_current = db_tenths_to_linear(config->gain_tenths);
    dsp->limiter_gain = 1.0f;
    dsp->night_gain = config->night ? NIGHT_MAKEUP : 1.0f;
    dsp->stats.min_limiter_gain = 1.0f;
    refresh_passthrough(dsp);
    return 0;
}

int gc_audio_dsp_update(GcAudioDsp *dsp, const GcAudioDspConfig *config)
{
    if (!dsp || gc_audio_dsp_config_valid(config)) return -1;
    if (config->sample_rate != dsp->config.sample_rate ||
        config->channels != dsp->config.channels)
        return -1;
    const uint32_t span = transition_frames(config->sample_rate);
    GcAudioEqBank next;
    memset(&next, 0, sizeof(next));
    design_bank(&next, config);
    if (dsp->crossfade_left) {
        /* A fade is audible right now. Never replace either side of it; the
         * newest request waits and fades in from wherever this one lands. */
        if (same_bank(&next, &dsp->eq)) dsp->eq_pending_valid = 0;
        else {
            dsp->eq_pending = next;
            dsp->eq_pending_valid = 1;
        }
    } else {
        /* No fade is running, so this request supersedes anything still
         * queued (a fade can end exactly at a buffer boundary with a pending
         * bank not yet started). The latest request always wins. */
        dsp->eq_pending_valid = 0;
        if (!same_bank(&next, &dsp->eq)) begin_crossfade(dsp, &next);
    }
    const float target = db_tenths_to_linear(config->gain_tenths);
    if (target != dsp->gain_target || dsp->gain_ramp_left) {
        dsp->gain_target = target;
        dsp->gain_ramp_left = span;
        dsp->gain_step = (target - dsp->gain_current) / (float)span;
    }
    dsp->config = *config;
    refresh_passthrough(dsp);
    return 0;
}

void gc_audio_dsp_reset(GcAudioDsp *dsp)
{
    if (!dsp) return;
    memset(dsp->eq.z1, 0, sizeof(dsp->eq.z1));
    memset(dsp->eq.z2, 0, sizeof(dsp->eq.z2));
    memset(&dsp->eq_previous, 0, sizeof(dsp->eq_previous));
    if (dsp->eq_pending_valid) {
        /* A stream restart has no audible fade to protect: apply the newest. */
        dsp->eq = dsp->eq_pending;
        memset(dsp->eq.z1, 0, sizeof(dsp->eq.z1));
        memset(dsp->eq.z2, 0, sizeof(dsp->eq.z2));
        dsp->eq_pending_valid = 0;
    }
    dsp->crossfade_left = dsp->crossfade_total = 0;
    dsp->gain_current = dsp->gain_target;
    dsp->gain_ramp_left = 0;
    dsp->night_env = 0.0f;
    dsp->night_hold_left = 0;
    dsp->night_gain = dsp->config.night ? NIGHT_MAKEUP : 1.0f;
    dsp->limiter_gain = 1.0f;
    dsp->limiter_deficit = 0.0f;
    dsp->limiter_hold_left = 0;
    refresh_passthrough(dsp);
}

/* Stereo-linked feed-forward compressor gain for one frame (makeup included).
 * The level detector is a peak follower with instant rise and a hold longer
 * than one period of a 50 Hz tone, so a steady low tone sees a steady level
 * and the gain does not ripple inside each cycle (which would distort it).
 * The gain itself then moves with the attack/release time constants. */
static float night_step(GcAudioDsp *dsp, float level)
{
    if (level >= dsp->night_env) {
        dsp->night_env = level;
        dsp->night_hold_left = dsp->limiter_hold;
    } else if (dsp->night_hold_left) {
        --dsp->night_hold_left;
    } else {
        dsp->night_env = flush(dsp->night_env * dsp->night_release);
    }
    float target = 1.0f;
    if (dsp->config.night) {
        float reduction = 1.0f;
        if (dsp->night_env > NIGHT_THRESHOLD) {
            reduction = powf(dsp->night_env / NIGHT_THRESHOLD, 1.0f / NIGHT_RATIO - 1.0f);
            ++dsp->stats.compressed_frames;
        }
        target = reduction * NIGHT_MAKEUP;
    }
    /* Falling gain uses the attack constant, rising gain the release one.
     * Snap exactly to unity once off, so passthrough can resume bit-exact. */
    const double coeff = target < dsp->night_gain ? dsp->night_attack : dsp->night_release;
    dsp->night_gain = target + (dsp->night_gain - target) * coeff;
    if (!dsp->config.night && fabs(dsp->night_gain - 1.0) < 1e-6) dsp->night_gain = 1.0;
    return (float)dsp->night_gain;
}

static int16_t to_s16(float v)
{
    float scaled = v * 32768.0f;
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32768.0f) scaled = -32768.0f;
    return (int16_t)lrintf(scaled);
}

void gc_audio_dsp_process(GcAudioDsp *dsp, const int16_t *in, int16_t *out, size_t frames)
{
    if (!dsp || !in || !out || !frames) return;
    const int channels = dsp->config.channels;
    dsp->stats.frames += frames;
    if (dsp->passthrough) {
        if (in != out) memmove(out, in, frames * (size_t)channels * sizeof(*in));
        return;
    }
    for (size_t f = 0; f < frames; ++f) {
        float x[2] = {0.0f, 0.0f};
        float peak_in = 0.0f;
        for (int c = 0; c < channels; ++c) {
            x[c] = (float)in[f * (size_t)channels + (size_t)c] / 32768.0f;
            if (fabsf(x[c]) > peak_in) peak_in = fabsf(x[c]);
        }
        const float night = night_step(dsp, peak_in);
        if (!dsp->crossfade_left && dsp->eq_pending_valid) {
            dsp->eq_pending_valid = 0;
            if (!same_bank(&dsp->eq_pending, &dsp->eq)) begin_crossfade(dsp, &dsp->eq_pending);
        }
        float t = 1.0f;
        if (dsp->crossfade_left) {
            t = 1.0f - (float)dsp->crossfade_left / (float)dsp->crossfade_total;
            --dsp->crossfade_left;
        }
        if (dsp->gain_ramp_left) {
            dsp->gain_current += dsp->gain_step;
            if (--dsp->gain_ramp_left == 0) dsp->gain_current = dsp->gain_target;
        }
        float y[2] = {0.0f, 0.0f};
        float peak = 0.0f;
        for (int c = 0; c < channels; ++c) {
            const float pre = x[c] * night;
            float v = run_bank(&dsp->eq, c, pre);
            if (t < 1.0f) v = run_bank(&dsp->eq_previous, c, pre) * (1.0f - t) + v * t;
            v *= dsp->gain_current;
            y[c] = v;
            if (fabsf(v) > peak) peak = fabsf(v);
        }
        /* Stereo-linked peak limiter: release toward unity, then clamp so the
         * linked peak never exceeds the positive s16 ceiling. Release is
         * tracked as the reduction (1 - gain) so it keeps shrinking below
         * float resolution near unity and then snaps to exactly 0. */
        if (dsp->limiter_hold_left) --dsp->limiter_hold_left;
        else {
            dsp->limiter_deficit *= dsp->limiter_release;
            if (dsp->limiter_deficit < 1e-5f) dsp->limiter_deficit = 0.0f;
        }
        float g = 1.0f - dsp->limiter_deficit;
        if (peak * g > CEILING) {
            g = CEILING / peak;
            dsp->limiter_deficit = 1.0f - g;
        }
        /* While the signal still needs limiting AND its limited peaks reach
         * within 1 dB of the ceiling, hold the reduction instead of releasing;
         * otherwise a steady low tone would release between peaks and be
         * re-pinned flat at every one. A signal that has become quieter (or
         * fits outright) releases back toward the gain it actually needs. */
        if (peak > CEILING && peak * g > CEILING * HOLD_REFRESH)
            dsp->limiter_hold_left = dsp->limiter_hold;
        dsp->limiter_gain = g;
        if (g < 1.0f) {
            ++dsp->stats.limited_frames;
            if (g < dsp->stats.min_limiter_gain) dsp->stats.min_limiter_gain = g;
        }
        for (int c = 0; c < channels; ++c)
            out[f * (size_t)channels + (size_t)c] = to_s16(y[c] * g);
    }
    refresh_passthrough(dsp);
}

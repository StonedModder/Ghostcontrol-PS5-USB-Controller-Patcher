/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-05: per-session headset playback DSP.
 *
 *   input s16 -> [night compressor] -> 5-band EQ -> user gain
 *             -> stereo-linked peak limiter -> s16 output
 *
 * Written for Ghostcontrol from the public RBJ "Audio EQ Cookbook" biquad
 * formulas and textbook feed-forward dynamics; no submitted or third-party
 * code is imported. Pure: no allocation, IO, globals or shared statics, so
 * every headset/stream owns an independent GcAudioDsp and two sessions can
 * never race on filter or envelope state.
 *
 * Guarantees the host tests pin:
 *   - Neutral settings (EQ off or all bands 0, gain 0, night off) are
 *     bit-exact passthrough.
 *   - The limiter's ceiling is digital full scale with instant attack, a
 *     20 ms hold and 80 ms release, so no setting can wrap a sample; gain is
 *     never negative, so no sample changes sign through it. Without
 *     lookahead (no added latency), the very first overshoot after quiet can
 *     sit at the ceiling until its first peak (about a quarter cycle of the
 *     tone); later cycles are not.
 *   - Silence in is exact silence out for every setting; no denormals.
 *   - Changing settings mid-stream ramps gain and crossfades EQ over a
 *     short window instead of stepping, so transitions do not click. EQ
 *     changes that arrive faster than one crossfade are coalesced: the
 *     newest waits until the running fade ends, then fades in.
 *
 * Saved or configured settings are not evidence that a stream applied them:
 * this module processes whatever buffer it is given. Where samples come from
 * (an authorized PCM provider) and where they go (an encoder/radio owner) are
 * AUDIO-01..03/07, not this module.
 */
#ifndef GC_AUDIO_DSP_H
#define GC_AUDIO_DSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_AUDIO_EQ_BANDS 5
/* Band centres in Hz: low shelf, three peaking bands, high shelf. */
#define GC_AUDIO_EQ_LOW_HZ 80
#define GC_AUDIO_EQ_MID1_HZ 250
#define GC_AUDIO_EQ_MID2_HZ 1000
#define GC_AUDIO_EQ_MID3_HZ 3500
#define GC_AUDIO_EQ_HIGH_HZ 10000
/* All levels are integer tenths of a dB so settings round-trip exactly. */
#define GC_AUDIO_EQ_MIN_TENTHS (-120)
#define GC_AUDIO_EQ_MAX_TENTHS 120
#define GC_AUDIO_GAIN_MIN_TENTHS (-240)
#define GC_AUDIO_GAIN_MAX_TENTHS 120

typedef struct {
    uint32_t sample_rate;  /* 16000, 32000, 44100 or 48000 */
    uint8_t channels;      /* 1 or 2, interleaved */
    uint8_t eq_enabled;    /* 0 or 1 */
    int16_t eq_tenths[GC_AUDIO_EQ_BANDS];
    int16_t gain_tenths;
    uint8_t night;         /* 0 or 1: stereo-linked night compressor */
} GcAudioDspConfig;

typedef struct {
    float b0, b1, b2, a1, a2;
} GcAudioBiquad;

typedef struct {
    GcAudioBiquad coeff[GC_AUDIO_EQ_BANDS];
    float z1[GC_AUDIO_EQ_BANDS][2], z2[GC_AUDIO_EQ_BANDS][2];
    uint8_t active;
} GcAudioEqBank;

/* Diagnostics measured over the session; never estimates. */
typedef struct {
    uint64_t frames;
    uint64_t limited_frames;     /* frames where the limiter reduced gain */
    uint64_t compressed_frames;  /* frames where night reduced gain */
    float min_limiter_gain;      /* deepest limiter reduction seen, linear */
} GcAudioDspStats;

typedef struct {
    GcAudioDspConfig config;
    GcAudioEqBank eq, eq_previous;
    /* An EQ change that arrives while a crossfade is still running waits
     * here (latest wins) and starts when the audible fade completes, so the
     * output never jumps between two partly-blended filter outputs. */
    GcAudioEqBank eq_pending;
    uint8_t eq_pending_valid;
    uint32_t crossfade_left, crossfade_total;
    float gain_current, gain_target, gain_step;
    uint32_t gain_ramp_left;
    float night_env;
    double night_gain; /* double: float stalls ~1e-5 above unity near the end */
    uint32_t night_hold_left;
    float night_attack, night_release;
    float limiter_gain, limiter_deficit, limiter_release;
    uint32_t limiter_hold, limiter_hold_left;
    uint8_t passthrough;
    GcAudioDspStats stats;
} GcAudioDsp;

/* 0 when the config is complete and in range; -1 otherwise. */
int gc_audio_dsp_config_valid(const GcAudioDspConfig *config);
/* Neutral settings for a rate/channel count (bit-exact passthrough). */
GcAudioDspConfig gc_audio_dsp_neutral(uint32_t sample_rate, uint8_t channels);

/* Initializes a session. Returns -1 (leaving *dsp zeroed) on invalid config. */
int gc_audio_dsp_init(GcAudioDsp *dsp, const GcAudioDspConfig *config);
/* Changes settings mid-stream with a click-free transition. The rate and
 * channel count are a stream property and cannot change here (-1). */
int gc_audio_dsp_update(GcAudioDsp *dsp, const GcAudioDspConfig *config);
/* Clears filter/envelope history (stream restart) but keeps settings. */
void gc_audio_dsp_reset(GcAudioDsp *dsp);
/* Processes `frames` interleaved frames in place. in == out is allowed. */
void gc_audio_dsp_process(GcAudioDsp *dsp, const int16_t *in, int16_t *out, size_t frames);

#ifdef __cplusplus
}
#endif

#endif

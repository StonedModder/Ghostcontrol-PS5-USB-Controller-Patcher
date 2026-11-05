/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-05: per-headset audio settings with checked atomic persistence.
 *
 * One small private file per canonical headset address, separate from
 * controller rules/profiles and from any pairing secret (no link keys are
 * ever stored or accepted here). Every save is:
 *
 *   validate -> generation check -> exclusive temp (O_EXCL|O_NOFOLLOW, 0600)
 *   -> full write -> fsync -> close -> rename over the target -> fsync dir
 *   -> authoritative reopen/decode/compare readback
 *
 * and reports `saved` (the rename happened) separately from `verified`
 * (readback matched). A failure before the rename leaves the previous record
 * byte-for-byte intact. A stale temporary, a linked/nonregular target or an
 * unreadable existing record is never deleted, truncated or overwritten.
 *
 * Generations: each committed record carries generation N >= 1. A save names
 * the generation it was based on and is refused as a conflict when the disk
 * moved on. The check, commit and readback run under one exclusive ordinary
 * flock on <root>/.audio-settings.lock (bounded 1 s wait, then refused), so
 * two writers -- threads or processes -- cannot both pass the same check.
 * Quarantine takes the same lock.
 *
 * `saved`, `verified` and `durable` are separate facts: a directory fsync
 * failure after the rename leaves saved=1 (and verified if the readback
 * matched) with durable=0 and an error saying so; nothing is rolled back.
 *
 * Link with core/src/gc_file_safety.c (gc_ensure_real_directory_tree).
 *
 * Saved settings are configuration, not evidence that a stream applied them.
 */
#ifndef GC_AUDIO_SETTINGS_H
#define GC_AUDIO_SETTINGS_H

#include "gc_audio_dsp.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_AUDIO_SETTINGS_SCHEMA 1
#define GC_AUDIO_ADDRESS_TEXT 18 /* "AA:BB:CC:DD:EE:FF" + NUL */
#define GC_AUDIO_SETTINGS_MAX_BYTES 1024
#define GC_AUDIO_LATENCY_MIN_MS 40
#define GC_AUDIO_LATENCY_MAX_MS 500
#define GC_AUDIO_VOLUME_MAX 127      /* AVRCP absolute volume scale */
#define GC_AUDIO_BITPOOL_MIN 2
#define GC_AUDIO_BITPOOL_MAX 250

typedef struct {
    uint64_t generation;   /* committed generation; 0 = defaults, never saved */
    uint16_t latency_ms;   /* target buffer latency */
    uint8_t volume;        /* requested headset volume 0..127 (not applied gain) */
    int16_t gain_tenths;   /* software gain, tenths of dB */
    uint8_t eq_enabled;
    int16_t eq_tenths[GC_AUDIO_EQ_BANDS];
    uint8_t night;
    uint8_t held_bitpool;  /* 0 = negotiate; else hold SBC bitpool 2..250 */
} GcAudioHeadsetSettings;

typedef enum {
    GC_AUDIO_LOAD_DEFAULTS = 0, /* no record: defaults, not an error */
    GC_AUDIO_LOAD_OK = 1,
    GC_AUDIO_LOAD_ERROR = -1,   /* unreadable/malformed/unsafe: defaults + error */
} GcAudioLoadStatus;

typedef struct {
    GcAudioLoadStatus status;
    GcAudioHeadsetSettings settings;
    char error[160];
} GcAudioSettingsLoad;

typedef struct {
    int saved;        /* the new record replaced the target */
    int verified;     /* reopen + decode matched exactly what was written */
    int durable;      /* file fsync AND directory fsync after the rename succeeded */
    int conflict;     /* disk generation differed from the expected one */
    uint64_t generation; /* generation of the record now on disk, when known */
    char error[160];
} GcAudioSettingsWrite;

/* "aa:bb:cc:dd:ee:ff" or uppercase -> "AA:BB:CC:DD:EE:FF". Exactly six
 * two-digit hex octets separated by ':'; anything else is -1. The all-zero
 * address is rejected. */
int gc_audio_address_canonical(const char *text, char out[GC_AUDIO_ADDRESS_TEXT]);

GcAudioHeadsetSettings gc_audio_settings_defaults(void);
int gc_audio_settings_valid(const GcAudioHeadsetSettings *settings);
int gc_audio_settings_equal(const GcAudioHeadsetSettings *a, const GcAudioHeadsetSettings *b);

/* Exact codec. Decode requires every key exactly once, in order, the matching
 * canonical address and schema 1; anything else is -1. */
int gc_audio_settings_encode(const GcAudioHeadsetSettings *settings, const char *address,
                             char *out, size_t cap);
int gc_audio_settings_decode(const char *text, size_t length, const char *address,
                             GcAudioHeadsetSettings *out);

/* <root>/audio-AABBCCDDEEFF.ini; -1 for a bad address or too-long path. */
int gc_audio_settings_path(const char *root, const char *address, char *out, size_t cap);

void gc_audio_settings_load(const char *root, const char *address, GcAudioSettingsLoad *out);

/* `expected_generation` is the generation this edit was based on (0 when no
 * record exists). The record is written with expected_generation + 1. */
GcAudioSettingsWrite gc_audio_settings_save(const char *root, const char *address,
                                            const GcAudioHeadsetSettings *desired,
                                            uint64_t expected_generation);

/* Recovery for an unreadable record: renames it to <file>.corrupt (refusing
 * if that already exists) so it is kept for diagnostics and a fresh save
 * with expected generation 0 can proceed. A readable record is left alone. */
int gc_audio_settings_quarantine(const char *root, const char *address,
                                 char *error, size_t error_size);

GcAudioDspConfig gc_audio_settings_dsp_config(const GcAudioHeadsetSettings *settings,
                                              uint32_t sample_rate, uint8_t channels);

#ifdef __cplusplus
}
#endif

#endif

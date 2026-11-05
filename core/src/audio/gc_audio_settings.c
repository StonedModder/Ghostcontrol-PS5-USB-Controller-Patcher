/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-05 per-headset settings store. See gc_audio_settings.h.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* flock() */
#endif
#if !defined(_POSIX_C_SOURCE) && !defined(__PROSPERO__)
/* FreeBSD/Prospero headers hide flock() behind __BSD_VISIBLE when a strict
 * POSIX environment is requested; their default environment shows it. */
#define _POSIX_C_SOURCE 200809L
#endif
#include "gc_audio_settings.h"

#include "gc_file_safety.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (!error || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int gc_audio_address_canonical(const char *text, char out[GC_AUDIO_ADDRESS_TEXT])
{
    if (!text || !out || strlen(text) != 17) return -1;
    static const char digits[] = "0123456789ABCDEF";
    int nonzero = 0;
    for (int octet = 0; octet < 6; ++octet) {
        const char *p = text + octet * 3;
        const int hi = hex_value(p[0]), lo = hex_value(p[1]);
        if (hi < 0 || lo < 0) return -1;
        if (octet < 5 && p[2] != ':') return -1;
        if (hi || lo) nonzero = 1;
        out[octet * 3] = digits[hi];
        out[octet * 3 + 1] = digits[lo];
        if (octet < 5) out[octet * 3 + 2] = ':';
    }
    out[17] = '\0';
    return nonzero ? 0 : -1;
}

GcAudioHeadsetSettings gc_audio_settings_defaults(void)
{
    GcAudioHeadsetSettings s;
    memset(&s, 0, sizeof(s));
    s.latency_ms = 150;
    s.volume = 100;
    return s;
}

int gc_audio_settings_valid(const GcAudioHeadsetSettings *s)
{
    if (!s) return -1;
    if (s->latency_ms < GC_AUDIO_LATENCY_MIN_MS || s->latency_ms > GC_AUDIO_LATENCY_MAX_MS) return -1;
    if (s->volume > GC_AUDIO_VOLUME_MAX) return -1;
    if (s->gain_tenths < GC_AUDIO_GAIN_MIN_TENTHS || s->gain_tenths > GC_AUDIO_GAIN_MAX_TENTHS) return -1;
    if (s->eq_enabled > 1 || s->night > 1) return -1;
    for (int i = 0; i < GC_AUDIO_EQ_BANDS; ++i)
        if (s->eq_tenths[i] < GC_AUDIO_EQ_MIN_TENTHS || s->eq_tenths[i] > GC_AUDIO_EQ_MAX_TENTHS) return -1;
    if (s->held_bitpool && (s->held_bitpool < GC_AUDIO_BITPOOL_MIN || s->held_bitpool > GC_AUDIO_BITPOOL_MAX))
        return -1;
    return 0;
}

int gc_audio_settings_equal(const GcAudioHeadsetSettings *a, const GcAudioHeadsetSettings *b)
{
    if (!a || !b) return 0;
    if (a->generation != b->generation || a->latency_ms != b->latency_ms || a->volume != b->volume ||
        a->gain_tenths != b->gain_tenths || a->eq_enabled != b->eq_enabled || a->night != b->night ||
        a->held_bitpool != b->held_bitpool)
        return 0;
    for (int i = 0; i < GC_AUDIO_EQ_BANDS; ++i)
        if (a->eq_tenths[i] != b->eq_tenths[i]) return 0;
    return 1;
}

int gc_audio_settings_encode(const GcAudioHeadsetSettings *s, const char *address, char *out, size_t cap)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!out || !cap || gc_audio_settings_valid(s) || gc_audio_address_canonical(address, canonical))
        return -1;
    const int n = snprintf(out, cap,
        "schema=%d\naddress=%s\ngeneration=%llu\ncodec=sbc\nlatency_ms=%u\nvolume=%u\n"
        "gain_tenths=%d\neq_enabled=%u\neq_tenths=%d,%d,%d,%d,%d\nnight=%u\nheld_bitpool=%u\n",
        GC_AUDIO_SETTINGS_SCHEMA, canonical, (unsigned long long)s->generation,
        (unsigned)s->latency_ms, (unsigned)s->volume, (int)s->gain_tenths, (unsigned)s->eq_enabled,
        s->eq_tenths[0], s->eq_tenths[1], s->eq_tenths[2], s->eq_tenths[3], s->eq_tenths[4],
        (unsigned)s->night, (unsigned)s->held_bitpool);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* A plain decimal with an optional leading '-' (when allowed), no leading
 * zeros or '+', bounded to [min, max]. Consumes exactly [p, end). */
static int parse_int(const char *p, const char *end, int allow_negative, long long min,
                     long long max, long long *out)
{
    int negative = 0;
    if (p < end && *p == '-') {
        if (!allow_negative) return -1;
        negative = 1;
        ++p;
    }
    if (p >= end || end - p > 20) return -1;
    if (*p == '0' && end - p > 1) return -1;
    if (negative && *p == '0') return -1; /* no "-0" */
    unsigned long long value = 0;
    for (; p < end; ++p) {
        if (*p < '0' || *p > '9') return -1;
        const unsigned digit = (unsigned)(*p - '0');
        if (value > (ULLONG_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    if (negative) {
        if (value > (unsigned long long)LLONG_MAX) return -1;
        const long long v = -(long long)value;
        if (v < min || v > max) return -1;
        *out = v;
    } else {
        if (value > (unsigned long long)LLONG_MAX || (long long)value < min || (long long)value > max)
            return -1;
        *out = (long long)value;
    }
    return 0;
}

static int parse_u64(const char *p, const char *end, uint64_t *out)
{
    if (p >= end || end - p > 20 || (*p == '0' && end - p > 1)) return -1;
    uint64_t value = 0;
    for (; p < end; ++p) {
        if (*p < '0' || *p > '9') return -1;
        const unsigned digit = (unsigned)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    *out = value;
    return 0;
}

/* Takes the next "key=value\n" line, requiring exactly `key`. */
static int take_line(const char **cursor, const char *end, const char *key,
                     const char **value, const char **value_end)
{
    const size_t key_length = strlen(key);
    const char *p = *cursor;
    if ((size_t)(end - p) <= key_length || memcmp(p, key, key_length) || p[key_length] != '=')
        return -1;
    p += key_length + 1;
    const char *newline = memchr(p, '\n', (size_t)(end - p));
    if (!newline) return -1;
    for (const char *q = p; q < newline; ++q)
        if ((unsigned char)*q < 0x20 || *q == 0x7f) return -1;
    *value = p;
    *value_end = newline;
    *cursor = newline + 1;
    return 0;
}

int gc_audio_settings_decode(const char *text, size_t length, const char *address,
                             GcAudioHeadsetSettings *out)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!text || !out || length > GC_AUDIO_SETTINGS_MAX_BYTES ||
        gc_audio_address_canonical(address, canonical))
        return -1;
    if (memchr(text, '\0', length)) return -1;
    const char *p = text, *end = text + length, *v, *ve;
    GcAudioHeadsetSettings s;
    memset(&s, 0, sizeof(s));
    long long n;
    if (take_line(&p, end, "schema", &v, &ve) || (size_t)(ve - v) != 1 || *v != '1') return -1;
    if (take_line(&p, end, "address", &v, &ve) || (size_t)(ve - v) != 17 || memcmp(v, canonical, 17))
        return -1;
    if (take_line(&p, end, "generation", &v, &ve) || parse_u64(v, ve, &s.generation) || !s.generation)
        return -1;
    if (take_line(&p, end, "codec", &v, &ve) || (size_t)(ve - v) != 3 || memcmp(v, "sbc", 3)) return -1;
    if (take_line(&p, end, "latency_ms", &v, &ve) ||
        parse_int(v, ve, 0, GC_AUDIO_LATENCY_MIN_MS, GC_AUDIO_LATENCY_MAX_MS, &n))
        return -1;
    s.latency_ms = (uint16_t)n;
    if (take_line(&p, end, "volume", &v, &ve) || parse_int(v, ve, 0, 0, GC_AUDIO_VOLUME_MAX, &n)) return -1;
    s.volume = (uint8_t)n;
    if (take_line(&p, end, "gain_tenths", &v, &ve) ||
        parse_int(v, ve, 1, GC_AUDIO_GAIN_MIN_TENTHS, GC_AUDIO_GAIN_MAX_TENTHS, &n))
        return -1;
    s.gain_tenths = (int16_t)n;
    if (take_line(&p, end, "eq_enabled", &v, &ve) || parse_int(v, ve, 0, 0, 1, &n)) return -1;
    s.eq_enabled = (uint8_t)n;
    if (take_line(&p, end, "eq_tenths", &v, &ve)) return -1;
    for (int band = 0; band < GC_AUDIO_EQ_BANDS; ++band) {
        const char *stop = band + 1 < GC_AUDIO_EQ_BANDS ? memchr(v, ',', (size_t)(ve - v)) : ve;
        if (!stop || parse_int(v, stop, 1, GC_AUDIO_EQ_MIN_TENTHS, GC_AUDIO_EQ_MAX_TENTHS, &n)) return -1;
        s.eq_tenths[band] = (int16_t)n;
        v = stop + 1;
    }
    if (take_line(&p, end, "night", &v, &ve) || parse_int(v, ve, 0, 0, 1, &n)) return -1;
    s.night = (uint8_t)n;
    if (take_line(&p, end, "held_bitpool", &v, &ve) || parse_int(v, ve, 0, 0, GC_AUDIO_BITPOOL_MAX, &n))
        return -1;
    s.held_bitpool = (uint8_t)n;
    if (p != end || gc_audio_settings_valid(&s)) return -1;
    *out = s;
    return 0;
}

int gc_audio_settings_path(const char *root, const char *address, char *out, size_t cap)
{
    char canonical[GC_AUDIO_ADDRESS_TEXT];
    if (!root || !*root || !out || gc_audio_address_canonical(address, canonical)) return -1;
    char compact[13];
    for (int i = 0, j = 0; i < 17; ++i)
        if (canonical[i] != ':') compact[j++] = canonical[i];
    compact[12] = '\0';
    const int n = snprintf(out, cap, "%s/audio-%s.ini", root, compact);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

/* 1 = read, 0 = missing, -1 = unsafe/unreadable. */
static int read_record(const char *path, char *buffer, size_t cap, size_t *length,
                       char *error, size_t error_size)
{
    *length = 0;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        set_error(error, error_size, "audio settings unreadable: %s", strerror(errno));
        return -1;
    }
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
        (unsigned long long)info.st_size > GC_AUDIO_SETTINGS_MAX_BYTES) {
        set_error(error, error_size, "audio settings are not a bounded single-link regular file");
        close(fd);
        return -1;
    }
    for (;;) {
        if (*length >= cap) {
            set_error(error, error_size, "audio settings exceed their bound");
            close(fd);
            return -1;
        }
        const ssize_t n = read(fd, buffer + *length, cap - *length);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            set_error(error, error_size, "audio settings read failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0) break;
        *length += (size_t)n;
    }
    if (close(fd)) {
        set_error(error, error_size, "audio settings close failed: %s", strerror(errno));
        return -1;
    }
    return 1;
}

void gc_audio_settings_load(const char *root, const char *address, GcAudioSettingsLoad *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->settings = gc_audio_settings_defaults();
    out->status = GC_AUDIO_LOAD_ERROR;
    char path[PATH_MAX];
    if (gc_audio_settings_path(root, address, path, sizeof(path))) {
        set_error(out->error, sizeof(out->error), "invalid headset address or settings root");
        return;
    }
    char text[GC_AUDIO_SETTINGS_MAX_BYTES + 1];
    size_t length = 0;
    const int found = read_record(path, text, sizeof(text), &length, out->error, sizeof(out->error));
    if (found < 0) return;
    if (found == 0) {
        out->status = GC_AUDIO_LOAD_DEFAULTS;
        return;
    }
    GcAudioHeadsetSettings decoded;
    if (gc_audio_settings_decode(text, length, address, &decoded)) {
        set_error(out->error, sizeof(out->error),
                  "audio settings are malformed or an unsupported schema; defaults in use");
        return;
    }
    out->settings = decoded;
    out->status = GC_AUDIO_LOAD_OK;
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        const ssize_t n = write(fd, data + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0) errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* One exclusive ordinary flock per settings root, held across the
 * generation check, commit and readback, so two writers (threads or
 * processes) can never both pass the same generation check. Bounded wait;
 * a holder that never lets go is reported, not broken. */
#define LOCK_WAIT_MS 1000

static int settings_lock(const char *root, char *error, size_t error_size)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.audio-settings.lock", root) >= (int)sizeof(path)) {
        set_error(error, error_size, "audio settings lock path too long");
        return -1;
    }
    const int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(error, error_size, "audio settings lock unavailable: %s", strerror(errno));
        return -1;
    }
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        set_error(error, error_size, "audio settings lock is not a single-link regular file");
        close(fd);
        return -1;
    }
    for (int waited = 0;; waited += 2) {
        if (!flock(fd, LOCK_EX | LOCK_NB)) return fd;
        if (errno != EWOULDBLOCK && errno != EINTR) {
            set_error(error, error_size, "audio settings lock failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
        if (waited >= LOCK_WAIT_MS) {
            set_error(error, error_size, "another writer holds the audio settings lock; nothing written");
            close(fd);
            return -1;
        }
        const struct timespec pause = {0, 2000000};
        nanosleep(&pause, NULL);
    }
}

static GcAudioSettingsWrite save_locked(const char *root, const char *address,
                                        const GcAudioHeadsetSettings *desired,
                                        uint64_t expected_generation, const char *path);

GcAudioSettingsWrite gc_audio_settings_save(const char *root, const char *address,
                                            const GcAudioHeadsetSettings *desired,
                                            uint64_t expected_generation)
{
    GcAudioSettingsWrite result;
    memset(&result, 0, sizeof(result));
    char path[PATH_MAX];
    if (!desired || gc_audio_settings_valid(desired) ||
        gc_audio_settings_path(root, address, path, sizeof(path))) {
        set_error(result.error, sizeof(result.error), "invalid audio settings, address or root");
        return result;
    }
    if (expected_generation == UINT64_MAX) {
        set_error(result.error, sizeof(result.error), "audio settings generation exhausted");
        return result;
    }
    if (root[0] != '/' || gc_ensure_real_directory_tree(root, 0700)) {
        set_error(result.error, sizeof(result.error), "audio settings root is not a real directory");
        return result;
    }
    const int lock = settings_lock(root, result.error, sizeof(result.error));
    if (lock < 0) return result;
    result = save_locked(root, address, desired, expected_generation, path);
    close(lock); /* releases the flock */
    return result;
}

static GcAudioSettingsWrite save_locked(const char *root, const char *address,
                                        const GcAudioHeadsetSettings *desired,
                                        uint64_t expected_generation, const char *path)
{
    GcAudioSettingsWrite result;
    memset(&result, 0, sizeof(result));
    char temporary[PATH_MAX + 8];

    /* Generation check against the authoritative disk record. An unreadable
     * record is never overwritten; quarantine is the explicit recovery. */
    GcAudioSettingsLoad current;
    gc_audio_settings_load(root, address, &current);
    if (current.status == GC_AUDIO_LOAD_ERROR) {
        set_error(result.error, sizeof(result.error), "%s; existing record preserved, nothing written",
                  current.error);
        return result;
    }
    const uint64_t on_disk = current.status == GC_AUDIO_LOAD_OK ? current.settings.generation : 0;
    result.generation = on_disk;
    if (on_disk != expected_generation) {
        result.conflict = 1;
        set_error(result.error, sizeof(result.error),
                  "audio settings changed elsewhere (generation %llu, expected %llu); nothing written",
                  (unsigned long long)on_disk, (unsigned long long)expected_generation);
        return result;
    }

    GcAudioHeadsetSettings record = *desired;
    record.generation = expected_generation + 1;
    char text[GC_AUDIO_SETTINGS_MAX_BYTES];
    const int length = gc_audio_settings_encode(&record, address, text, sizeof(text));
    if (length < 0) {
        set_error(result.error, sizeof(result.error), "audio settings could not be encoded");
        return result;
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    /* A stale or foreign temporary is not ours to truncate or delete. */
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(result.error, sizeof(result.error), "audio settings temporary unavailable: %s",
                  strerror(errno));
        return result;
    }
    int failed = 0;
    if (write_all(fd, text, (size_t)length)) {
        set_error(result.error, sizeof(result.error), "audio settings write failed: %s", strerror(errno));
        failed = 1;
    } else if (fsync(fd)) {
        set_error(result.error, sizeof(result.error), "audio settings sync failed: %s", strerror(errno));
        failed = 1;
    }
    if (close(fd) && !failed) {
        set_error(result.error, sizeof(result.error), "audio settings close failed: %s", strerror(errno));
        failed = 1;
    }
    if (!failed && rename(temporary, path)) {
        set_error(result.error, sizeof(result.error), "audio settings publish failed: %s", strerror(errno));
        failed = 1;
    }
    if (failed) {
        unlink(temporary); /* ours: created O_EXCL above */
        return result;
    }
    result.saved = 1;
    result.generation = record.generation;
    /* Make the rename itself durable. A failure here does not undo the
     * commit; it is reported as committed-but-not-durable. */
    char sync_error[96] = "";
    const int directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        set_error(sync_error, sizeof(sync_error), "directory open failed: %s", strerror(errno));
    } else {
        if (fsync(directory))
            set_error(sync_error, sizeof(sync_error), "directory sync failed: %s", strerror(errno));
        if (close(directory) && !sync_error[0])
            set_error(sync_error, sizeof(sync_error), "directory close failed: %s", strerror(errno));
    }
    result.durable = !sync_error[0];

    /* Authoritative readback: committed is not the same as verified. */
    GcAudioSettingsLoad readback;
    gc_audio_settings_load(root, address, &readback);
    if (readback.status != GC_AUDIO_LOAD_OK || !gc_audio_settings_equal(&readback.settings, &record)) {
        set_error(result.error, sizeof(result.error), "audio settings committed but readback %s%s%s",
                  readback.status == GC_AUDIO_LOAD_OK ? "did not match" : "failed",
                  sync_error[0] ? "; also " : "", sync_error);
        return result;
    }
    result.verified = 1;
    if (!result.durable)
        set_error(result.error, sizeof(result.error), "audio settings committed and read back, but %s",
                  sync_error);
    return result;
}

static int quarantine_locked(const char *root, const char *address, const char *path,
                             char *error, size_t error_size);

int gc_audio_settings_quarantine(const char *root, const char *address, char *error, size_t error_size)
{
    char path[PATH_MAX];
    if (gc_audio_settings_path(root, address, path, sizeof(path)) || root[0] != '/') {
        set_error(error, error_size, "invalid headset address or settings root");
        return -1;
    }
    const int lock = settings_lock(root, error, error_size);
    if (lock < 0) return -1;
    const int rc = quarantine_locked(root, address, path, error, error_size);
    close(lock);
    return rc;
}

static int quarantine_locked(const char *root, const char *address, const char *path,
                             char *error, size_t error_size)
{
    char corrupt[PATH_MAX + 16];
    GcAudioSettingsLoad current;
    gc_audio_settings_load(root, address, &current);
    if (current.status != GC_AUDIO_LOAD_ERROR) {
        set_error(error, error_size, "audio settings are readable; nothing to quarantine");
        return -1;
    }
    struct stat info;
    if (lstat(path, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        set_error(error, error_size, "audio settings are not a single-link regular file; left in place");
        return -1;
    }
    snprintf(corrupt, sizeof(corrupt), "%s.corrupt", path);
    if (!lstat(corrupt, &info) || errno != ENOENT) {
        set_error(error, error_size, "a quarantined audio settings file already exists; left in place");
        return -1;
    }
    if (rename(path, corrupt)) {
        set_error(error, error_size, "audio settings quarantine failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

GcAudioDspConfig gc_audio_settings_dsp_config(const GcAudioHeadsetSettings *settings,
                                              uint32_t sample_rate, uint8_t channels)
{
    GcAudioDspConfig config = gc_audio_dsp_neutral(sample_rate, channels);
    if (!settings || gc_audio_settings_valid(settings)) return config;
    config.eq_enabled = settings->eq_enabled;
    memcpy(config.eq_tenths, settings->eq_tenths, sizeof(config.eq_tenths));
    config.gain_tenths = settings->gain_tenths;
    config.night = settings->night;
    return config;
}

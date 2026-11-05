/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "gc_state.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

struct GcState {
    pthread_mutex_t lock;
    pthread_mutex_t flush_lock;
    GcStateSnapshot snapshot;
    char status_path[512];
    unsigned temp_sequence;
};

static void sync_status_parent(const char *path)
{
    char parent[512];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return;
    size_t length = (size_t)(slash - path);
    if (length >= sizeof(parent))
        return;
    memcpy(parent, path, length);
    parent[length] = '\0';
    int directory = open(parent, O_RDONLY | O_NOFOLLOW);
    if (directory >= 0) {
        (void)fsync(directory);
        (void)close(directory);
    }
}

static uint64_t now_ms(void)
{
    struct timeval value;
    gettimeofday(&value, NULL);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_usec / 1000u;
}

const char *gc_service_status_name(GcServiceStatus status)
{
    static const char *const names[] = {
        "inactive", "starting", "active", "stopping", "error"
    };
    return status >= GC_SERVICE_INACTIVE && status <= GC_SERVICE_ERROR
               ? names[status] : "error";
}

static int append(char **cursor, size_t *remaining, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(*cursor, *remaining, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= *remaining)
        return -1;
    *cursor += (size_t)count;
    *remaining -= (size_t)count;
    return 0;
}

static int append_json_string(char **cursor, size_t *remaining,
                              const char *value)
{
    if (append(cursor, remaining, "\"") != 0)
        return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            if (append(cursor, remaining, "\\%c", *p) != 0)
                return -1;
        } else if (*p == '\n') {
            if (append(cursor, remaining, "\\n") != 0)
                return -1;
        } else if (*p == '\r') {
            if (append(cursor, remaining, "\\r") != 0)
                return -1;
        } else if (*p == '\t') {
            if (append(cursor, remaining, "\\t") != 0)
                return -1;
        } else if (*p < 0x20u) {
            if (append(cursor, remaining, "\\u%04x", *p) != 0)
                return -1;
        } else if (append(cursor, remaining, "%c", *p) != 0) {
            return -1;
        }
    }
    return append(cursor, remaining, "\"");
}

static int snapshot_json(const GcStateSnapshot *snapshot, char *output,
                         size_t output_size)
{
    char *cursor = output;
    size_t remaining = output_size;
    if (!output || output_size == 0 ||
        append(&cursor, &remaining,
               "{\"schema\":1,\"version\":") != 0 ||
        append_json_string(&cursor, &remaining, snapshot->version) != 0 ||
        append(&cursor, &remaining,
               ",\"pid\":%d,\"started_ms\":%llu,\"service\":",
               snapshot->pid, (unsigned long long)snapshot->started_ms) != 0 ||
        append_json_string(&cursor, &remaining,
                           gc_service_status_name(snapshot->service)) != 0 ||
        append(&cursor, &remaining, ",\"error\":") != 0 ||
        append_json_string(&cursor, &remaining, snapshot->error) != 0 ||
        append(&cursor, &remaining, ",\"config_generation\":%llu,\"slots\":[",
               (unsigned long long)snapshot->config_generation) != 0)
        return -1;

    for (size_t slot = 0; slot < GC_STATE_MAX_SLOTS; ++slot) {
        const GcSlotState *value = &snapshot->slots[slot];
        if (append(&cursor, &remaining,
                   "%s{\"slot\":%u,\"connected\":%s,\"streaming\":%s,"
                   "\"vid\":%u,\"pid\":%u,\"vid_pid\":\"%04x:%04x\","
                   "\"vda_handle\":%d,\"bound_user\":%d,\"inject_count\":%u,"
                   "\"name\":",
                   slot ? "," : "", (unsigned)slot,
                   value->connected ? "true" : "false",
                   value->streaming ? "true" : "false",
                   value->vid, value->pid, value->vid, value->pid,
                   value->vda_handle, value->bound_user, value->inject_count) != 0 ||
            append_json_string(&cursor, &remaining, value->name) != 0 ||
            append(&cursor, &remaining, ",\"protocol\":") != 0 ||
            append_json_string(&cursor, &remaining, value->protocol) != 0 ||
            append(&cursor, &remaining, ",\"profile\":") != 0 ||
            append_json_string(&cursor, &remaining, value->profile) != 0 ||
            append(&cursor, &remaining, ",\"device_path\":") != 0 ||
            append_json_string(&cursor, &remaining, value->device_path) != 0 ||
            append(&cursor, &remaining, "}") != 0)
            return -1;
    }
    const GcWirelessState *wireless = &snapshot->wireless;
    if (append(&cursor, &remaining, "],\"wireless_ds4\":{\"status\":") ||
        append_json_string(&cursor, &remaining, wireless->status) ||
        append(&cursor, &remaining,
            ",\"connected\":%u,\"routes\":%u,\"imports\":%u,\"game_pid\":%d,"
            "\"frames\":%llu,\"publications\":%llu,\"gateway_calls\":%llu,"
            "\"missed_deadlines\":%llu,\"lease_expirations\":%llu},\"notifications\":[",
            wireless->connected, wireless->routes, wireless->imports, wireless->game_pid,
            (unsigned long long)wireless->frames, (unsigned long long)wireless->publications,
            (unsigned long long)wireless->gateway_calls,
            (unsigned long long)wireless->missed_deadlines,
            (unsigned long long)wireless->lease_expirations))
        return -1;
    for (size_t i = 0; i < snapshot->notification_count; ++i) {
        const GcNotificationState *note = &snapshot->notifications[i];
        if (append(&cursor, &remaining, "%s{\"timestamp_ms\":%llu,\"message\":",
                   i ? "," : "", (unsigned long long)note->timestamp_ms) != 0 ||
            append_json_string(&cursor, &remaining, note->message) != 0 ||
            append(&cursor, &remaining, "}") != 0)
            return -1;
    }
    return append(&cursor, &remaining, "]}\n");
}

GcState *gc_state_open(const char *root, const char *version,
                       char *error, size_t error_size)
{
    if (!root || !version) {
        if (error && error_size)
            snprintf(error, error_size, "state root/version missing");
        return NULL;
    }
    GcState *state = calloc(1, sizeof(*state));
    if (!state)
        return NULL;
    if (pthread_mutex_init(&state->lock, NULL) != 0) {
        free(state);
        return NULL;
    }
    if (pthread_mutex_init(&state->flush_lock, NULL) != 0) {
        pthread_mutex_destroy(&state->lock);
        free(state);
        return NULL;
    }
    state->snapshot.pid = (int32_t)getpid();
    state->snapshot.started_ms = now_ms();
    state->snapshot.service = GC_SERVICE_INACTIVE;
    snprintf(state->snapshot.version, sizeof(state->snapshot.version), "%s", version);
    for (size_t slot = 0; slot < GC_STATE_MAX_SLOTS; ++slot) {
        state->snapshot.slots[slot].vda_handle = -1;
        state->snapshot.slots[slot].bound_user = -1;
    }
    int count = snprintf(state->status_path, sizeof(state->status_path),
                         "%s/runtime/status.json", root);
    if (count < 0 || (size_t)count >= sizeof(state->status_path)) {
        gc_state_close(state);
        if (error && error_size)
            snprintf(error, error_size, "state path too long");
        return NULL;
    }
    if (gc_state_flush(state) != 0) {
        if (error && error_size)
            snprintf(error, error_size, "cannot write state: %s", strerror(errno));
        gc_state_close(state);
        return NULL;
    }
    return state;
}

void gc_state_close(GcState *state)
{
    if (!state)
        return;
    pthread_mutex_destroy(&state->flush_lock);
    pthread_mutex_destroy(&state->lock);
    free(state);
}

void gc_state_set_service(GcState *state, GcServiceStatus status,
                          const char *error)
{
    if (!state)
        return;
    pthread_mutex_lock(&state->lock);
    state->snapshot.service = status;
    snprintf(state->snapshot.error, sizeof(state->snapshot.error), "%s",
             error ? error : "");
    pthread_mutex_unlock(&state->lock);
    (void)gc_state_flush(state);
}

void gc_state_set_generation(GcState *state, uint64_t generation)
{
    if (!state)
        return;
    pthread_mutex_lock(&state->lock);
    state->snapshot.config_generation = generation;
    pthread_mutex_unlock(&state->lock);
    (void)gc_state_flush(state);
}

void gc_state_set_wireless(GcState *state, const GcWirelessState *wireless)
{
    if (!state || !wireless) return;
    pthread_mutex_lock(&state->lock);
    state->snapshot.wireless = *wireless;
    pthread_mutex_unlock(&state->lock);
    (void)gc_state_flush(state);
}

void gc_state_set_slot(GcState *state, size_t slot, const GcSlotState *value)
{
    if (!state || !value || slot >= GC_STATE_MAX_SLOTS)
        return;
    pthread_mutex_lock(&state->lock);
    state->snapshot.slots[slot] = *value;
    pthread_mutex_unlock(&state->lock);
    (void)gc_state_flush(state);
}

void gc_state_patch_slot(GcState *state, size_t slot,
                         const GcSlotState *value, unsigned fields)
{
    if (!state || !value || slot >= GC_STATE_MAX_SLOTS)
        return;
    pthread_mutex_lock(&state->lock);
    GcSlotState *destination = &state->snapshot.slots[slot];
    if (fields & GC_STATE_SLOT_PATCH_LIVE) {
        destination->connected = value->connected;
        destination->vid = value->vid;
        destination->pid = value->pid;
        destination->vda_handle = value->vda_handle;
        destination->inject_count = value->inject_count;
        snprintf(destination->protocol, sizeof(destination->protocol), "%s",
                 value->protocol);
        snprintf(destination->device_path, sizeof(destination->device_path),
                 "%s", value->device_path);
    }
    if (fields & GC_STATE_SLOT_PATCH_NAME)
        snprintf(destination->name, sizeof(destination->name), "%s",
                 value->name);
    if (fields & GC_STATE_SLOT_PATCH_PROFILE)
        snprintf(destination->profile, sizeof(destination->profile), "%s",
                 value->profile);
    if (fields & GC_STATE_SLOT_PATCH_STREAMING)
        destination->streaming = value->streaming;
    if (fields & GC_STATE_SLOT_PATCH_BOUND_USER)
        destination->bound_user = value->bound_user;
    pthread_mutex_unlock(&state->lock);
    (void)gc_state_flush(state);
}

void gc_state_clear_slot(GcState *state, size_t slot)
{
    if (!state || slot >= GC_STATE_MAX_SLOTS)
        return;
    GcSlotState value;
    memset(&value, 0, sizeof(value));
    value.vda_handle = -1;
    value.bound_user = -1;
    gc_state_set_slot(state, slot, &value);
}

void gc_state_note(GcState *state, const char *message)
{
    if (!state || !message)
        return;
    pthread_mutex_lock(&state->lock);
    if (state->snapshot.notification_count == GC_STATE_MAX_NOTIFICATIONS) {
        memmove(&state->snapshot.notifications[0],
                &state->snapshot.notifications[1],
                sizeof(state->snapshot.notifications[0]) *
                    (GC_STATE_MAX_NOTIFICATIONS - 1));
        --state->snapshot.notification_count;
    }
    GcNotificationState *note =
        &state->snapshot.notifications[state->snapshot.notification_count++];
    note->timestamp_ms = now_ms();
    snprintf(note->message, sizeof(note->message), "%s", message);
    pthread_mutex_unlock(&state->lock);
    (void)gc_state_flush(state);
}

int gc_state_snapshot(GcState *state, GcStateSnapshot *snapshot)
{
    if (!state || !snapshot)
        return -1;
    pthread_mutex_lock(&state->lock);
    *snapshot = state->snapshot;
    pthread_mutex_unlock(&state->lock);
    return 0;
}

int gc_state_json(GcState *state, char *output, size_t output_size)
{
    GcStateSnapshot snapshot;
    return gc_state_snapshot(state, &snapshot) == 0
               ? snapshot_json(&snapshot, output, output_size) : -1;
}

int gc_state_flush(GcState *state)
{
    char json[GC_STATE_JSON_MAX];
    char temporary[576];
    if (!state)
        return -1;
    pthread_mutex_lock(&state->flush_lock);
    if (gc_state_json(state, json, sizeof(json)) != 0) {
        pthread_mutex_unlock(&state->flush_lock);
        return -1;
    }
    struct stat existing;
    if (lstat(state->status_path, &existing) == 0 &&
        (S_ISLNK(existing.st_mode) || !S_ISREG(existing.st_mode))) {
        errno = EINVAL;
        pthread_mutex_unlock(&state->flush_lock);
        return -1;
    }
    int count = snprintf(temporary, sizeof(temporary), "%s.tmp.%d.%u",
                         state->status_path, (int)getpid(),
                         ++state->temp_sequence);
    if (count < 0 || (size_t)count >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        pthread_mutex_unlock(&state->flush_lock);
        return -1;
    }
    int fd = open(temporary,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        pthread_mutex_unlock(&state->flush_lock);
        return -1;
    }
    struct stat temporary_info;
    if (fstat(fd, &temporary_info) != 0 ||
        !S_ISREG(temporary_info.st_mode) || temporary_info.st_nlink != 1) {
        int saved = errno ? errno : EIO;
        close(fd);
        unlink(temporary);
        errno = saved;
        pthread_mutex_unlock(&state->flush_lock);
        return -1;
    }
    size_t length = strlen(json), offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, json + offset, length - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            int saved = errno;
            close(fd);
            unlink(temporary);
            errno = saved;
            pthread_mutex_unlock(&state->flush_lock);
            return -1;
        }
        offset += (size_t)written;
    }
    int saved = 0;
    if (fsync(fd) != 0)
        saved = errno;
    if (close(fd) != 0 && !saved)
        saved = errno;
    if (!saved && rename(temporary, state->status_path) != 0)
        saved = errno;
    if (saved) {
        unlink(temporary);
        errno = saved;
        pthread_mutex_unlock(&state->flush_lock);
        return -1;
    }
    sync_status_parent(state->status_path);
    pthread_mutex_unlock(&state->flush_lock);
    return 0;
}

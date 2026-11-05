/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "gc_config.h"

#include "gc_ini.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GC_CONFIG_FILE_MAX (64u * 1024u)

struct GcConfigStore {
    pthread_mutex_t lock;
    /* Serializes disk mutation, reload parsing, and snapshot publication so
     * an older concurrent reload cannot overwrite a newer save. */
    pthread_mutex_t mutation_lock;
    char root[GC_CONFIG_PATH_MAX];
    GcRuntimeConfig runtime;
    GcProfile profiles[GC_CONFIG_MAX_PROFILES];
    size_t profile_count;
    GcControllerRule controllers[GC_CONFIG_MAX_CONTROLLERS];
    size_t controller_count;
    _Atomic uint64_t generation;
    int instance_policy_valid;
    int generation_exhausted;
    GcInstanceProductMode product_mode;
    int product_configured;
    GcInstancePolicyInvalidationHook invalidation_hook;
    void *invalidation_context;
};

static unsigned g_temp_sequence;

static const char k_default_runtime[] =
    "[ghostcontrol]\n"
    "schema=1\n"
    "\n"
    "[service]\n"
    "autostart=true\n"
    "run_in_background=true\n"
    "daemon_mode=true\n"
    "auto_restart=true\n"
    "wireless_ds4=true\n"
    "health_interval_ms=1000\n"
    "disconnect_restart_ms=3000\n"
    "input_stale_release_ms=400\n"
    "\n"
    "[ghostpad]\n"
    "enabled=false\n"
    "port=6967\n"
    "stale_release_ms=250\n"
    "\n"
    "[dualsense_fx]\n"
    "enabled=false\n"
    "effect=0\n"
    "period_ms=1000\n"
    "step_ms=33\n"
    "r=0\n"
    "g=0\n"
    "b=255\n"
    "\n"
    "[update]\n"
    "enabled=true\n"
    "include_prereleases=false\n"
    "\n"
    "[profiles]\n"
    "active=auto\n"
    "\n"
    "[ipc]\n"
    "port=39071\n"
    "\n"
    "[xmb]\n"
    "enabled=true\n"
    "homeui_tile_owner=auto\n"
    "settings_entry_owner=auto\n";

static const char k_default_controllers[] =
    "# Ghostcontrol controller overrides. Built-in hardware support remains\n"
    "# available when this file contains no sections. The toolbox writes\n"
    "# [controller:<id>] sections here after validation.\n"
    "[ghostcontrol]\n"
    "schema=1\n";

static void
set_error(char *error, size_t error_size, const char *format, ...)
{
    if (!error || error_size == 0)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
    error[error_size - 1] = '\0';
}

/* Writer-owned authority; held checks intentionally take no locks. */
static int
policy_check_held(GcConfigStore *store, char *error, size_t error_size)
{
    if (!store->instance_policy_valid || store->generation_exhausted) {
        set_error(error, error_size, "instance_policy_quarantined");
        return -1;
    }
    return 0;
}

static void
policy_invalidate_held(GcConfigStore *store, GcInstanceInvalidationReason reason)
{
    pthread_mutex_lock(&store->lock);
    uint64_t generation = atomic_load_explicit(&store->generation, memory_order_acquire);
    if (store->instance_policy_valid) {
        store->instance_policy_valid = 0;
        if (generation < UINT64_MAX) ++generation;
        atomic_store_explicit(&store->generation, generation, memory_order_release);
    }
    if (generation == UINT64_MAX) store->generation_exhausted = 1;
    pthread_mutex_unlock(&store->lock);
    /* Callback observes the published quarantine, with writer held but no snapshot. */
    if (store->invalidation_hook)
        store->invalidation_hook(store->invalidation_context, reason, generation);
}

int
gc_config_instance_policy_valid(GcConfigStore *store)
{
    if (!store) return 0;
    pthread_mutex_lock(&store->lock);
    int valid = store->instance_policy_valid && !store->generation_exhausted;
    pthread_mutex_unlock(&store->lock);
    return !!valid;
}

int
gc_config_instance_check_held(GcConfigStore *store, GcInstanceRequirement required,
                              char *error, size_t error_size)
{
    if (!store || (required != GC_INSTANCE_REQUIRE_STORAGE && required != GC_INSTANCE_REQUIRE_LIVE)) {
        set_error(error, error_size, "invalid_instance_request"); return -1;
    }
    if (policy_check_held(store, error, error_size)) return -1;
    if (store->product_mode == GC_INSTANCE_PRODUCT_DISABLED ||
        (required == GC_INSTANCE_REQUIRE_LIVE &&
         store->product_mode != GC_INSTANCE_PRODUCT_RESIDENT_STORAGE_LIVE)) {
        set_error(error, error_size, "instance_capability_unavailable"); return -1;
    }
    if (error && error_size) error[0] = 0;
    return 0;
}

int
gc_config_instance_acquire(GcConfigStore *store, GcInstanceRequirement required,
                           char *error, size_t error_size)
{
    if (!store) { set_error(error, error_size, "invalid_instance_request"); return -1; }
    pthread_mutex_lock(&store->mutation_lock);
    if (gc_config_instance_check_held(store, required, error, error_size)) {
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    return 0;
}

void
gc_config_instance_release(GcConfigStore *store)
{
    if (store) pthread_mutex_unlock(&store->mutation_lock);
}

int
gc_config_instance_product_configure(GcConfigStore *store, GcInstanceProductMode mode,
                                     char *error, size_t error_size)
{
    if (!store) { set_error(error, error_size, "invalid_instance_request"); return -1; }
    pthread_mutex_lock(&store->mutation_lock);
    int result = -1;
    if (mode < GC_INSTANCE_PRODUCT_DISABLED || mode > GC_INSTANCE_PRODUCT_RESIDENT_STORAGE_LIVE ||
        store->product_configured) set_error(error, error_size, "invalid_instance_request");
    else if (policy_check_held(store, error, error_size)) { /* no consumption */ }
    else if ((mode == GC_INSTANCE_PRODUCT_RESIDENT_STORAGE_LIVE &&
              (!store->invalidation_hook || store->invalidation_context != store)) ||
             (mode == GC_INSTANCE_PRODUCT_COMPANION_STORAGE && store->invalidation_hook))
        set_error(error, error_size, "instance_capability_unavailable");
    else {
        pthread_mutex_lock(&store->lock);
        store->product_mode = mode; store->product_configured = 1;
        pthread_mutex_unlock(&store->lock);
        if (error && error_size) error[0] = 0;
        result = 0;
    }
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_instance_capabilities(GcConfigStore *store, GcInstanceCapabilities *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!store || !out) return -1;
    pthread_mutex_lock(&store->lock);
    out->instance_contract_version = GC_INSTANCE_CONTRACT_VERSION;
    out->instance_policy_valid = !!(store->instance_policy_valid && !store->generation_exhausted);
    out->instance_storage_available = out->instance_policy_valid &&
        store->product_mode != GC_INSTANCE_PRODUCT_DISABLED;
    out->instance_live_available = out->instance_policy_valid &&
        store->product_mode == GC_INSTANCE_PRODUCT_RESIDENT_STORAGE_LIVE;
    out->instance_available = out->instance_live_available;
    pthread_mutex_unlock(&store->lock);
    return 0;
}

int
gc_config_instance_invalidation_register(GcConfigStore *store,
    GcInstancePolicyInvalidationHook hook, void *context, char *error, size_t error_size)
{
    if (!store || !hook || !context || context != store) {
        set_error(error, error_size, "invalid_instance_request"); return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    int result = -1;
    if (store->invalidation_hook || store->product_configured)
        set_error(error, error_size, "invalid_instance_request");
    else {
        store->invalidation_hook = hook; store->invalidation_context = context;
        if (error && error_size) error[0] = 0;
        result = 0;
    }
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_instance_invalidation_unregister(GcConfigStore *store,
    GcInstancePolicyInvalidationHook hook, void *context, char *error, size_t error_size)
{
    if (!store || !hook || !context || context != store) {
        set_error(error, error_size, "invalid_instance_request"); return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    int result = -1;
    if (store->invalidation_hook != hook || store->invalidation_context != context)
        set_error(error, error_size, "invalid_instance_request");
    else {
        store->invalidation_hook = NULL; store->invalidation_context = NULL;
        if (error && error_size) error[0] = 0;
        result = 0;
    }
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

static int
join_path(char *output, size_t output_size, const char *left,
          const char *right)
{
    if (!output || !left || !right)
        return -1;
    int written = snprintf(output, output_size, "%s/%s", left, right);
    return written < 0 || (size_t)written >= output_size ? -1 : 0;
}

static int
ensure_one_directory(const char *path, char *error, size_t error_size)
{
    struct stat info;
    if (lstat(path, &info) == 0) {
        if (S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode)) {
            set_error(error, error_size, "path is not a real directory: %s",
                      path);
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT || mkdir(path, 0755) != 0) {
        set_error(error, error_size, "cannot create directory %s: %s", path,
                  strerror(errno));
        return -1;
    }
    return 0;
}

static int
ensure_directory_tree(const char *path, char *error, size_t error_size)
{
    char copy[GC_CONFIG_PATH_MAX];
    size_t length = strlen(path);
    if (length < 2 || length >= sizeof(copy) || path[0] != '/') {
        set_error(error, error_size, "data root must be a bounded absolute path");
        return -1;
    }
    memcpy(copy, path, length + 1);
    for (size_t index = 1; index < length; ++index) {
        if (copy[index] != '/')
            continue;
        copy[index] = '\0';
        if (ensure_one_directory(copy, error, error_size) != 0)
            return -1;
        copy[index] = '/';
    }
    return ensure_one_directory(copy, error, error_size);
}

static int
split_parent(const char *path, char *parent, size_t parent_size)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return -1;
    size_t length = (size_t)(slash - path);
    if (length >= parent_size)
        return -1;
    memcpy(parent, path, length);
    parent[length] = '\0';
    return 0;
}

static void
sync_parent_directory(const char *path)
{
    char parent[GC_CONFIG_PATH_MAX];
    if (split_parent(path, parent, sizeof(parent)) != 0)
        return;
    int directory_fd = open(parent, O_RDONLY | O_NOFOLLOW);
    if (directory_fd >= 0) {
        (void)fsync(directory_fd);
        (void)close(directory_fd);
    }
}

static int
write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, data + offset, length - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int
atomic_write(const char *path, const char *data, size_t length,
             char *error, size_t error_size)
{
    struct stat existing;
    if (lstat(path, &existing) == 0 && S_ISLNK(existing.st_mode)) {
        set_error(error, error_size, "refusing to replace symlink: %s", path);
        return -1;
    }

    char temporary[GC_CONFIG_PATH_MAX + 48];
    unsigned sequence = __sync_add_and_fetch(&g_temp_sequence, 1);
    int name_length = snprintf(temporary, sizeof(temporary), "%s.tmp.%d.%u",
                               path, (int)getpid(), sequence);
    if (name_length < 0 || (size_t)name_length >= sizeof(temporary)) {
        set_error(error, error_size, "temporary path is too long");
        return -1;
    }

    int fd = open(temporary,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        set_error(error, error_size, "cannot create %s: %s", temporary,
                  strerror(errno));
        return -1;
    }
    int result = 0;
    struct stat temporary_info;
    if (fstat(fd, &temporary_info) != 0 ||
        !S_ISREG(temporary_info.st_mode)) {
        set_error(error, error_size, "temporary config is not a regular file");
        result = -1;
    }
    if (result == 0 &&
        (write_all(fd, data, length) != 0 || fsync(fd) != 0)) {
        set_error(error, error_size, "cannot write %s: %s", temporary,
                  strerror(errno));
        result = -1;
    }
    if (close(fd) != 0 && result == 0) {
        set_error(error, error_size, "cannot close %s: %s", temporary,
                  strerror(errno));
        result = -1;
    }
    if (result == 0 && rename(temporary, path) != 0) {
        set_error(error, error_size, "cannot publish %s: %s", path,
                  strerror(errno));
        result = -1;
    }
    if (result != 0) {
        (void)unlink(temporary);
        return -1;
    }

    sync_parent_directory(path);
    return 0;
}

static int
write_if_missing(const char *path, const char *data, size_t length,
                 char *error, size_t error_size)
{
    struct stat info;
    if (lstat(path, &info) == 0) {
        if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {
            set_error(error, error_size, "path is not a regular file: %s", path);
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        set_error(error, error_size, "cannot inspect %s: %s", path,
                  strerror(errno));
        return -1;
    }
    return atomic_write(path, data, length, error, error_size);
}

static char *
read_file(const char *path, size_t *length, char *error, size_t error_size)
{
    struct stat info;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        set_error(error, error_size, "cannot open %s: %s", path,
                  strerror(errno));
        return NULL;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (uint64_t)info.st_size > GC_CONFIG_FILE_MAX) {
        (void)close(fd);
        set_error(error, error_size, "invalid or oversized config file: %s",
                  path);
        return NULL;
    }
    size_t size = (size_t)info.st_size;
    char *data = malloc(size + 1);
    if (!data) {
        (void)close(fd);
        set_error(error, error_size, "out of memory reading %s", path);
        return NULL;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            free(data);
            (void)close(fd);
            set_error(error, error_size, "short read from %s", path);
            return NULL;
        }
        offset += (size_t)count;
    }
    (void)close(fd);
    data[size] = '\0';
    if (memchr(data, 0, size)) {
        free(data); set_error(error, error_size, "embedded NUL in config file"); return NULL;
    }
    *length = size;
    return data;
}

static int
snapshot_optional_file(const char *path, char **data, size_t *length,
                       int *existed, char *error, size_t error_size)
{
    struct stat info;
    *data = NULL;
    *length = 0;
    *existed = 0;
    if (lstat(path, &info) != 0) {
        if (errno == ENOENT)
            return 0;
        set_error(error, error_size, "cannot inspect %s: %s", path,
                  strerror(errno));
        return -1;
    }
    if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {
        set_error(error, error_size, "path is not a regular file: %s", path);
        return -1;
    }
    *data = read_file(path, length, error, error_size);
    if (!*data)
        return -1;
    *existed = 1;
    return 0;
}

static int
restore_file(const char *path, const char *data, size_t length, int existed,
             char *error, size_t error_size)
{
    if (existed) {
        if (atomic_write(path, data, length, error, error_size)) return -1;
        size_t restored_length = 0;
        char *restored = read_file(path, &restored_length, error, error_size);
        int result = restored && restored_length == length && !memcmp(restored, data, length) ? 0 : -1;
        free(restored);
        if (result) set_error(error, error_size, "rollback readback failed");
        return result;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        set_error(error, error_size, "cannot roll back %s: %s", path,
                  strerror(errno));
        return -1;
    }
    sync_parent_directory(path);
    return 0;
}

static void
runtime_defaults(GcRuntimeConfig *runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->schema = GC_CONFIG_SCHEMA;
    runtime->autostart = 1;
    runtime->run_in_background = 1;
    runtime->daemon_mode = 1;
    runtime->auto_restart = 1;
    runtime->wireless_ds4 = 1;
    runtime->ghostpad_enabled = 0;
    runtime->ghostpad_port = 6967;
    runtime->ghostpad_stale_ms = 250;
    runtime->dualsense_fx_enabled = 0;
    runtime->dualsense_fx_effect = 0;
    runtime->dualsense_fx_period_ms = 1000;
    runtime->dualsense_fx_step_ms = 33;
    runtime->dualsense_fx_r = 0;
    runtime->dualsense_fx_g = 0;
    runtime->dualsense_fx_b = 255;
    runtime->health_interval_ms = 1000;
    runtime->disconnect_restart_ms = 3000;
    runtime->input_stale_release_ms = 400;
    runtime->update_enabled = 1;
    (void)snprintf(runtime->active_profile,
                   sizeof(runtime->active_profile), "auto");
    runtime->ipc_port = GC_IPC_PORT;
    runtime->xmb_enabled = 1;
    (void)snprintf(runtime->homeui_tile_owner,
                   sizeof(runtime->homeui_tile_owner), "auto");
    (void)snprintf(runtime->settings_entry_owner,
                   sizeof(runtime->settings_entry_owner), "auto");
}

static int
parse_bool(const char *value, int *output)
{
    if (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        *output = 1;
        return 0;
    }
    if (strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        *output = 0;
        return 0;
    }
    return -1;
}

static int
parse_unsigned(const char *value, unsigned minimum, unsigned maximum,
               unsigned *output)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 0);
    if (errno || end == value || *end || parsed < minimum || parsed > maximum)
        return -1;
    *output = (unsigned)parsed;
    return 0;
}

static int
valid_profile_id(const char *value)
{
    if (!value || !*value)
        return 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor;
         ++cursor) {
        if (!((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9')) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.')
            return 0;
    }
    return strcmp(value, ".") != 0 && strcmp(value, "..") != 0;
}

static int
runtime_entry(void *context, const char *section, const char *key,
              const char *value, char *error, size_t error_size)
{
    GcRuntimeConfig *runtime = context;
    if (strcasecmp(section, "ghostcontrol") == 0 &&
        strcasecmp(key, "schema") == 0) {
        unsigned schema;
        if (parse_unsigned(value, GC_CONFIG_SCHEMA, GC_CONFIG_SCHEMA,
                           &schema) != 0)
            goto invalid;
        runtime->schema = (int)schema;
        return 0;
    }
    if (strcasecmp(section, "service") == 0) {
        if (strcasecmp(key, "autostart") == 0)
            return parse_bool(value, &runtime->autostart);
        if (strcasecmp(key, "run_in_background") == 0)
            return parse_bool(value, &runtime->run_in_background);
        if (strcasecmp(key, "daemon_mode") == 0)
            return parse_bool(value, &runtime->daemon_mode);
        if (strcasecmp(key, "auto_restart") == 0)
            return parse_bool(value, &runtime->auto_restart);
        if (strcasecmp(key, "wireless_ds4") == 0)
            return parse_bool(value, &runtime->wireless_ds4);
        if (strcasecmp(key, "health_interval_ms") == 0)
            return parse_unsigned(value, 250, 60000,
                                  &runtime->health_interval_ms);
        if (strcasecmp(key, "disconnect_restart_ms") == 0)
            return parse_unsigned(value, 0, 600000,
                                  &runtime->disconnect_restart_ms);
        if (strcasecmp(key, "input_stale_release_ms") == 0) {
            unsigned parsed;
            if (parse_unsigned(value, 0, 5000, &parsed) == 0 &&
                (parsed == 0 || parsed >= 50)) {
                runtime->input_stale_release_ms = parsed;
                return 0;
            }
            goto invalid;
        }
    } else if (strcasecmp(section, "ghostpad") == 0) {
        unsigned parsed;
        if (strcasecmp(key, "enabled") == 0)
            return parse_bool(value, &runtime->ghostpad_enabled);
        if (strcasecmp(key, "port") == 0 &&
            parse_unsigned(value, 1024, 65535, &parsed) == 0) {
            runtime->ghostpad_port = (uint16_t)parsed;
            return 0;
        }
        if (strcasecmp(key, "stale_release_ms") == 0)
            return parse_unsigned(value, 50, 5000,
                                  &runtime->ghostpad_stale_ms);
    } else if (strcasecmp(section, "dualsense_fx") == 0) {
        unsigned parsed;
        if (strcasecmp(key, "enabled") == 0)
            return parse_bool(value, &runtime->dualsense_fx_enabled);
        if (strcasecmp(key, "effect") == 0 &&
            parse_unsigned(value, 0, 4, &parsed) == 0) {
            runtime->dualsense_fx_effect = (int)parsed;
            return 0;
        }
        if (strcasecmp(key, "period_ms") == 0)
            return parse_unsigned(value, 100, 60000,
                                  &runtime->dualsense_fx_period_ms);
        if (strcasecmp(key, "step_ms") == 0)
            return parse_unsigned(value, 0, 1000,
                                  &runtime->dualsense_fx_step_ms);
        if (strcasecmp(key, "r") == 0)
            return parse_unsigned(value, 0, 255, &runtime->dualsense_fx_r);
        if (strcasecmp(key, "g") == 0)
            return parse_unsigned(value, 0, 255, &runtime->dualsense_fx_g);
        if (strcasecmp(key, "b") == 0)
            return parse_unsigned(value, 0, 255, &runtime->dualsense_fx_b);
        goto invalid;
    } else if (strcasecmp(section, "update") == 0) {
        if (strcasecmp(key, "enabled") == 0)
            return parse_bool(value, &runtime->update_enabled);
        if (strcasecmp(key, "include_prereleases") == 0)
            return parse_bool(value, &runtime->include_prereleases);
    } else if (strcasecmp(section, "profiles") == 0 &&
               strcasecmp(key, "active") == 0) {
        if ((strcasecmp(value, "auto") != 0 && !valid_profile_id(value)) ||
            strlen(value) >= sizeof(runtime->active_profile))
            goto invalid;
        (void)snprintf(runtime->active_profile,
                       sizeof(runtime->active_profile), "%s", value);
        return 0;
    } else if (strcasecmp(section, "ipc") == 0 &&
               strcasecmp(key, "port") == 0) {
        unsigned port;
        if (parse_unsigned(value, GC_IPC_PORT, GC_IPC_PORT, &port) != 0)
            goto invalid;
        runtime->ipc_port = (uint16_t)port;
        return 0;
    } else if (strcasecmp(section, "xmb") == 0 &&
               strcasecmp(key, "enabled") == 0) {
        return parse_bool(value, &runtime->xmb_enabled);
    } else if (strcasecmp(section, "xmb") == 0 &&
               (strcasecmp(key, "homeui_tile_owner") == 0 ||
                strcasecmp(key, "settings_entry_owner") == 0)) {
        /* Consumed by the ShellUI module; the core only validates and
         * round-trips it so the file survives a rewrite. */
        if (strcasecmp(value, "auto") != 0 &&
            strcasecmp(value, "ghostcontrol") != 0 &&
            strcasecmp(value, "onionhen") != 0)
            goto invalid;
        char *field = strcasecmp(key, "homeui_tile_owner") == 0
                          ? runtime->homeui_tile_owner
                          : runtime->settings_entry_owner;
        (void)snprintf(field, sizeof(runtime->homeui_tile_owner), "%s",
                       value);
        return 0;
    }

    set_error(error, error_size, "unknown config key [%s] %s", section, key);
    return -1;

invalid:
    set_error(error, error_size, "invalid value for [%s] %s", section, key);
    return -1;
}

static int
load_runtime(const char *root, GcRuntimeConfig *runtime, char *error,
             size_t error_size)
{
    char path[GC_CONFIG_PATH_MAX];
    if (join_path(path, sizeof(path), root, "config.ini") != 0)
        return -1;
    size_t length = 0;
    char *text = read_file(path, &length, error, error_size);
    if (!text)
        return -1;
    runtime_defaults(runtime);
    int result = gc_ini_parse(text, length, runtime_entry, runtime, error,
                              error_size);
    free(text);
    return result;
}

enum {
    CTRL_SEEN_NAME = 1u << 0,
    CTRL_SEEN_ENABLED = 1u << 1,
    CTRL_SEEN_IGNORE = 1u << 2,
    CTRL_SEEN_VID = 1u << 3,
    CTRL_SEEN_PID = 1u << 4,
    CTRL_SEEN_PROTOCOL = 1u << 5,
    CTRL_SEEN_IN = 1u << 6,
    CTRL_SEEN_OUT = 1u << 7,
    CTRL_SEEN_INTERFACE = 1u << 8,
    CTRL_SEEN_OUTPUT_TYPE = 1u << 9,
    CTRL_SEEN_INSTANCE = 1u << 10,
    CTRL_SEEN_PROFILE = 1u << 11,
};

typedef struct {
    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS];
    unsigned seen[GC_CONFIG_MAX_CONTROLLERS];
    size_t count;
    int schema_seen;
    unsigned schema;
} ControllerParseContext;

/* Canonical display-order Classic address, never a path or an LE identity. */
static int
valid_instance_selector(const char *value)
{
    if (!value || strlen(value) != 23 ||
        memcmp(value, "bt-classic:", 11) != 0)
        return 0;
    int nonzero = 0, nonff = 0;
    for (size_t i = 11; i < 23; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return 0;
        nonzero |= c != '0'; nonff |= c != 'F';
    }
    return nonzero && nonff;
}

static int
valid_controller_protocol(const char *value)
{
    static const char *const protocols[] = {
        "nintendo", "xbox_gip", "xusb", "ds3", "ds4", "logitech", "hid",
        "dinput",
    };
    for (size_t index = 0;
         index < sizeof(protocols) / sizeof(protocols[0]); ++index)
        if (strcmp(value, protocols[index]) == 0)
            return 1;
    return 0;
}

static int
valid_controller_name(const char *value)
{
    size_t length = value ? strlen(value) : 0;
    if (!length || length >= GC_PROFILE_NAME_MAX)
        return 0;
    for (size_t index = 0; index < length; ++index)
        if ((unsigned char)value[index] < 0x20 ||
            (unsigned char)value[index] == 0x7f || value[index] == '|')
            return 0;
    return 1;
}

static int
parse_endpoint(const char *value, int input, uint8_t *output)
{
    if (strcasecmp(value, "auto") == 0 || strcmp(value, "0") == 0) {
        *output = 0;
        return 0;
    }
    unsigned parsed;
    if (parse_unsigned(value, 1, 255, &parsed) != 0 ||
        (!!(parsed & 0x80u) != !!input) || (parsed & 0x70u) ||
        !(parsed & 0x0fu))
        return -1;
    *output = (uint8_t)parsed;
    return 0;
}

static int
parse_interface_number(const char *value, int *output)
{
    if (strcasecmp(value, "auto") == 0) {
        *output = -1;
        return 0;
    }
    unsigned parsed;
    if (parse_unsigned(value, 0, 255, &parsed) != 0)
        return -1;
    *output = (int)parsed;
    return 0;
}

static int
controller_rule_valid(const GcControllerRule *rule)
{
    if (!rule || !memchr(rule->id, 0, sizeof(rule->id)) ||
        !memchr(rule->name, 0, sizeof(rule->name)) ||
        !memchr(rule->protocol, 0, sizeof(rule->protocol)) ||
        !memchr(rule->instance_id, 0, sizeof(rule->instance_id)) ||
        !memchr(rule->profile_id, 0, sizeof(rule->profile_id)) ||
        !valid_profile_id(rule->id) ||
        (rule->instance_id[0] && (!valid_instance_selector(rule->instance_id) ||
         !strcmp(rule->id, "auto") || !strcmp(rule->id, "default"))) ||
        (!rule->instance_id[0] && rule->profile_id[0]) ||
        (rule->profile_id[0] && !valid_profile_id(rule->profile_id)) ||
        strlen(rule->id) >= GC_PROFILE_ID_MAX ||
        !valid_controller_name(rule->name) || rule->vid == 0 ||
        !valid_controller_protocol(rule->protocol) ||
        !gc_controller_output_name(rule->output_type) ||
        (rule->ignore != 0 && rule->ignore != 1) ||
        rule->preferred_interface_number < -1 ||
        rule->preferred_interface_number > 255 ||
        (rule->preferred_in_endpoint &&
         (!(rule->preferred_in_endpoint & 0x80u) ||
          (rule->preferred_in_endpoint & 0x70u) ||
          !(rule->preferred_in_endpoint & 0x0fu))) ||
        (rule->preferred_out_endpoint &&
         ((rule->preferred_out_endpoint & 0xf0u) ||
          !(rule->preferred_out_endpoint & 0x0fu))) ||
        (strcmp(rule->protocol, "ds3") == 0 &&
         rule->preferred_out_endpoint))
        return 0;
    return rule->enabled == 0 || rule->enabled == 1;
}

static int
controller_entry(void *context, const char *section, const char *key,
                 const char *value, char *error, size_t error_size)
{
    ControllerParseContext *parsed = context;
    if ((!*section || strcasecmp(section, "ghostcontrol") == 0) &&
        strcasecmp(key, "schema") == 0) {
        unsigned schema;
        if (parsed->schema_seen) goto invalid;
        if (parse_unsigned(value, GC_CONTROLLERS_SCHEMA_LEGACY,
                           GC_CONTROLLERS_SCHEMA_INSTANCE, &schema) != 0)
            goto invalid;
        parsed->schema = schema;
        parsed->schema_seen = 1;
        return 0;
    }
    if (strncasecmp(section, "controller:", 11) != 0)
        goto unknown;
    const char *id = section + 11;
    if (!valid_profile_id(id) || strlen(id) >= GC_PROFILE_ID_MAX)
        goto invalid;
    size_t index;
    for (index = 0; index < parsed->count; ++index)
        if (strcmp(parsed->rules[index].id, id) == 0)
            break;
    if (index == parsed->count) {
        if (parsed->count >= GC_CONFIG_MAX_CONTROLLERS) {
            set_error(error, error_size, "controller rule limit reached");
            return -1;
        }
        memset(&parsed->rules[index], 0, sizeof(parsed->rules[index]));
        (void)snprintf(parsed->rules[index].id,
                       sizeof(parsed->rules[index].id), "%s", id);
        parsed->rules[index].enabled = 1;
        parsed->rules[index].preferred_interface_number = -1;
        ++parsed->count;
    }
    GcControllerRule *rule = &parsed->rules[index];
    unsigned *seen = &parsed->seen[index];
#define ONCE(bit) do { if (*seen & (bit)) goto invalid; *seen |= (bit); } while (0)
    if (strcasecmp(key, "name") == 0) {
        ONCE(CTRL_SEEN_NAME);
        if (!valid_controller_name(value)) goto invalid;
        (void)snprintf(rule->name, sizeof(rule->name), "%s", value);
    } else if (strcasecmp(key, "enabled") == 0) {
        ONCE(CTRL_SEEN_ENABLED);
        if (parse_bool(value, &rule->enabled) != 0) goto invalid;
    } else if (strcasecmp(key, "ignore") == 0) {
        ONCE(CTRL_SEEN_IGNORE);
        if (parse_bool(value, &rule->ignore) != 0) goto invalid;
    } else if (strcasecmp(key, "vid") == 0) {
        unsigned number;
        ONCE(CTRL_SEEN_VID);
        if (parse_unsigned(value, 1, 0xffff, &number) != 0) goto invalid;
        rule->vid = (uint16_t)number;
    } else if (strcasecmp(key, "pid") == 0) {
        unsigned number;
        ONCE(CTRL_SEEN_PID);
        if (parse_unsigned(value, 0, 0xffff, &number) != 0) goto invalid;
        rule->pid = (uint16_t)number;
    } else if (strcasecmp(key, "protocol") == 0) {
        ONCE(CTRL_SEEN_PROTOCOL);
        if (!valid_controller_protocol(value)) goto invalid;
        (void)snprintf(rule->protocol, sizeof(rule->protocol), "%s", value);
    } else if (strcasecmp(key, "in_endpoint") == 0) {
        ONCE(CTRL_SEEN_IN);
        if (parse_endpoint(value, 1, &rule->preferred_in_endpoint) != 0)
            goto invalid;
    } else if (strcasecmp(key, "out_endpoint") == 0) {
        ONCE(CTRL_SEEN_OUT);
        if (parse_endpoint(value, 0, &rule->preferred_out_endpoint) != 0)
            goto invalid;
    } else if (strcasecmp(key, "output_type") == 0) {
        ONCE(CTRL_SEEN_OUTPUT_TYPE);
        if (gc_controller_output_parse(value, &rule->output_type) != 0)
            goto invalid;
    } else if (strcasecmp(key, "instance_id") == 0) {
        ONCE(CTRL_SEEN_INSTANCE);
        if (*value && !valid_instance_selector(value)) goto invalid;
        snprintf(rule->instance_id, sizeof(rule->instance_id), "%s", value);
    } else if (strcasecmp(key, "profile_id") == 0) {
        ONCE(CTRL_SEEN_PROFILE);
        if (strlen(value) >= sizeof(rule->profile_id) ||
            (*value && !valid_profile_id(value))) goto invalid;
        snprintf(rule->profile_id, sizeof(rule->profile_id), "%s", value);
    } else if (strcasecmp(key, "interface") == 0) {
        ONCE(CTRL_SEEN_INTERFACE);
        if (parse_interface_number(value,
                                   &rule->preferred_interface_number) != 0)
            goto invalid;
    } else {
        goto unknown;
    }
#undef ONCE
    return 0;

unknown:
    set_error(error, error_size, "unknown controller key [%s] %s", section,
              key);
    return -1;
invalid:
    set_error(error, error_size, "invalid controller value [%s] %s", section,
              key);
    return -1;
}

static int
load_controllers(const char *root, GcControllerRule *rules, size_t *count,
                 char *error, size_t error_size)
{
    char path[GC_CONFIG_PATH_MAX];
    if (join_path(path, sizeof(path), root, "controllers.ini") != 0)
        return -1;
    size_t length = 0;
    char *text = read_file(path, &length, error, error_size);
    if (!text)
        return -1;
    if (memchr(text, 0, length)) {
        free(text);
        set_error(error, error_size, "invalid controller text");
        return -1;
    }
    ControllerParseContext parsed;
    memset(&parsed, 0, sizeof(parsed));
    int result = gc_ini_parse(text, length, controller_entry, &parsed, error,
                              error_size);
    free(text);
    if (result != 0)
        return -1;
    if (!parsed.schema_seen) {
        set_error(error, error_size, "controllers schema is missing");
        return -1;
    }
    for (size_t index = 0; index < parsed.count; ++index) {
        unsigned owner_keys = parsed.seen[index] &
            (CTRL_SEEN_INSTANCE | CTRL_SEEN_PROFILE);
        if ((parsed.schema == GC_CONTROLLERS_SCHEMA_LEGACY && owner_keys) ||
            (parsed.schema == GC_CONTROLLERS_SCHEMA_INSTANCE &&
             owner_keys != (CTRL_SEEN_INSTANCE | CTRL_SEEN_PROFILE))) {
            set_error(error, error_size, "invalid controller schema keys");
            return -1;
        }
        if (!(parsed.seen[index] & CTRL_SEEN_NAME))
            (void)snprintf(parsed.rules[index].name,
                           sizeof(parsed.rules[index].name), "%s",
                           parsed.rules[index].id);
        if (!(parsed.seen[index] & CTRL_SEEN_VID) ||
            !(parsed.seen[index] & CTRL_SEEN_PID) ||
            !controller_rule_valid(&parsed.rules[index])) {
            set_error(error, error_size, "invalid controller rule: %s",
                      parsed.rules[index].id);
            return -1;
        }
        for (size_t other = 0; other < index; ++other)
            if (parsed.rules[index].vid == parsed.rules[other].vid &&
                parsed.rules[index].pid == parsed.rules[other].pid &&
                !strcmp(parsed.rules[index].instance_id,
                        parsed.rules[other].instance_id)) {
                set_error(error, error_size,
                          "duplicate controller VID:PID: %04x:%04x",
                          parsed.rules[index].vid, parsed.rules[index].pid);
                return -1;
            }
    }
    memcpy(rules, parsed.rules, parsed.count * sizeof(rules[0]));
    *count = parsed.count;
    return 0;
}

static int
serialize_controllers(const GcControllerRule *rules, size_t count,
                      char *output, size_t output_size, char *error,
                      size_t error_size)
{
    unsigned schema = GC_CONTROLLERS_SCHEMA_LEGACY;
    if (count > GC_CONFIG_MAX_CONTROLLERS) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!controller_rule_valid(&rules[i])) return -1;
        if (rules[i].instance_id[0] || rules[i].profile_id[0])
            schema = GC_CONTROLLERS_SCHEMA_INSTANCE;
    }
    size_t used = (size_t)snprintf(output, output_size,
                                   "[ghostcontrol]\nschema=%u\n", schema);
    if (used >= output_size || count > GC_CONFIG_MAX_CONTROLLERS)
        return -1;
    for (size_t index = 0; index < count; ++index) {
        if (!controller_rule_valid(&rules[index])) {
            set_error(error, error_size, "invalid controller rule: %s",
                      rules[index].id);
            return -1;
        }
        for (size_t other = 0; other < index; ++other)
            if (strcmp(rules[index].id, rules[other].id) == 0 ||
                (rules[index].vid == rules[other].vid &&
                 rules[index].pid == rules[other].pid &&
                 !strcmp(rules[index].instance_id, rules[other].instance_id))) {
                set_error(error, error_size, "duplicate controller rule");
                return -1;
            }
        char in_endpoint[8];
        char out_endpoint[8];
        char interface_number[8];
        if (rules[index].preferred_in_endpoint)
            (void)snprintf(in_endpoint, sizeof(in_endpoint), "0x%02x",
                           rules[index].preferred_in_endpoint);
        else
            (void)snprintf(in_endpoint, sizeof(in_endpoint), "auto");
        if (rules[index].preferred_out_endpoint)
            (void)snprintf(out_endpoint, sizeof(out_endpoint), "0x%02x",
                           rules[index].preferred_out_endpoint);
        else
            (void)snprintf(out_endpoint, sizeof(out_endpoint), "auto");
        if (rules[index].preferred_interface_number >= 0)
            (void)snprintf(interface_number, sizeof(interface_number), "%d",
                           rules[index].preferred_interface_number);
        else
            (void)snprintf(interface_number, sizeof(interface_number), "auto");
        int written = snprintf(
            output + used, output_size - used,
            "\n[controller:%s]\nname=\"%s\"\nenabled=%s\nignore=%s\n"
            "vid=0x%04x\npid=0x%04x\nprotocol=%s\n"
            "interface=%s\nin_endpoint=%s\nout_endpoint=%s\noutput_type=%s\n",
            rules[index].id, rules[index].name,
            rules[index].enabled ? "true" : "false",
            rules[index].ignore ? "true" : "false", rules[index].vid,
            rules[index].pid, rules[index].protocol, interface_number,
            in_endpoint,
            out_endpoint, gc_controller_output_name(rules[index].output_type));
        if (written < 0 || (size_t)written >= output_size - used)
            return -1;
        used += (size_t)written;
        if (schema == GC_CONTROLLERS_SCHEMA_INSTANCE) {
            written = snprintf(output + used, output_size - used,
                "instance_id=%s\nprofile_id=%s\n",
                rules[index].instance_id, rules[index].profile_id);
            if (written < 0 || (size_t)written >= output_size - used) return -1;
            used += (size_t)written;
        }
    }
    return (int)used;
}

static int
profile_name_compare(const void *left, const void *right)
{
    return strcmp(left, right);
}

static int
load_profiles(const char *root, GcProfile *profiles, size_t *profile_count,
              char *error, size_t error_size)
{
    char directory[GC_CONFIG_PATH_MAX];
    if (join_path(directory, sizeof(directory), root, "profiles") != 0)
        return -1;
    DIR *stream = opendir(directory);
    if (!stream) {
        set_error(error, error_size, "cannot open profiles directory: %s",
                  strerror(errno));
        return -1;
    }

    char names[GC_CONFIG_MAX_PROFILES][GC_PROFILE_ID_MAX + 5];
    size_t name_count = 0;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length <= 4 || strcasecmp(entry->d_name + length - 4, ".ini") != 0)
            continue;
        if (name_count >= GC_CONFIG_MAX_PROFILES ||
            length >= sizeof(names[0])) {
            (void)closedir(stream);
            set_error(error, error_size, "too many or oversized profile files");
            return -1;
        }
        (void)snprintf(names[name_count++], sizeof(names[0]), "%s",
                       entry->d_name);
    }
    (void)closedir(stream);
    qsort(names, name_count, sizeof(names[0]), profile_name_compare);

    size_t loaded = 0;
    for (size_t index = 0; index < name_count; ++index) {
        char relative[GC_PROFILE_ID_MAX + 16];
        char path[GC_CONFIG_PATH_MAX];
        (void)snprintf(relative, sizeof(relative), "profiles/%s", names[index]);
        if (join_path(path, sizeof(path), root, relative) != 0)
            return -1;
        size_t length = 0;
        char *text = read_file(path, &length, error, error_size);
        if (!text)
            return -1;
        GcProfile profile;
        int result =
            gc_profile_parse(text, length, &profile, error, error_size);
        free(text);
        if (result != 0)
            return -1;
        size_t base_length = strlen(names[index]) - 4;
        if (strlen(profile.id) != base_length ||
            strncmp(profile.id, names[index], base_length) != 0) {
            set_error(error, error_size,
                      "profile id '%s' must match filename '%s'", profile.id,
                      names[index]);
            return -1;
        }
        profiles[loaded++] = profile;
    }
    if (loaded == 0) {
        set_error(error, error_size, "at least one profile is required");
        return -1;
    }
    *profile_count = loaded;
    return 0;
}

static int
serialize_runtime(const GcRuntimeConfig *runtime, char *output,
                  size_t output_size)
{
    if (!runtime || !memchr(runtime->active_profile, 0, sizeof(runtime->active_profile)) ||
        !memchr(runtime->homeui_tile_owner, 0, sizeof(runtime->homeui_tile_owner)) ||
        !memchr(runtime->settings_entry_owner, 0, sizeof(runtime->settings_entry_owner)) ||
        runtime->schema != GC_CONFIG_SCHEMA ||
        (runtime->wireless_ds4 != 0 && runtime->wireless_ds4 != 1) ||
        (runtime->ghostpad_enabled != 0 && runtime->ghostpad_enabled != 1) ||
        runtime->ghostpad_port < 1024 || runtime->ghostpad_stale_ms < 50 ||
        runtime->ghostpad_stale_ms > 5000 ||
        (runtime->dualsense_fx_enabled != 0 && runtime->dualsense_fx_enabled != 1) ||
        runtime->dualsense_fx_effect < 0 || runtime->dualsense_fx_effect > 4 ||
        runtime->dualsense_fx_period_ms < 100 || runtime->dualsense_fx_period_ms > 60000 ||
        runtime->dualsense_fx_step_ms > 1000 || runtime->dualsense_fx_r > 255 ||
        runtime->dualsense_fx_g > 255 || runtime->dualsense_fx_b > 255 ||
        runtime->ipc_port != GC_IPC_PORT || runtime->health_interval_ms < 250 ||
        runtime->health_interval_ms > 60000 ||
        runtime->disconnect_restart_ms > 600000 ||
        (runtime->input_stale_release_ms != 0 &&
         runtime->input_stale_release_ms < 50) ||
        runtime->input_stale_release_ms > 5000 ||
        (strcasecmp(runtime->active_profile, "auto") != 0 &&
         !valid_profile_id(runtime->active_profile)))
        return -1;
    int written = snprintf(
        output, output_size,
        "[ghostcontrol]\n"
        "schema=1\n\n"
        "[service]\n"
        "autostart=%s\n"
        "run_in_background=%s\n"
        "daemon_mode=%s\n"
        "auto_restart=%s\n"
        "wireless_ds4=%s\n"
        "health_interval_ms=%u\n"
        "disconnect_restart_ms=%u\n"
        "input_stale_release_ms=%u\n\n"
        "[ghostpad]\n"
        "enabled=%s\n"
        "port=%u\n"
        "stale_release_ms=%u\n\n"
        "[dualsense_fx]\n"
        "enabled=%s\n"
        "effect=%d\n"
        "period_ms=%u\n"
        "step_ms=%u\n"
        "r=%u\n"
        "g=%u\n"
        "b=%u\n\n"
        "[update]\n"
        "enabled=%s\n"
        "include_prereleases=%s\n\n"
        "[profiles]\n"
        "active=%s\n\n"
        "[ipc]\n"
        "port=%u\n\n"
        "[xmb]\n"
        "enabled=%s\n"
        "homeui_tile_owner=%s\n"
        "settings_entry_owner=%s\n",
        runtime->autostart ? "true" : "false",
        runtime->run_in_background ? "true" : "false",
        runtime->daemon_mode ? "true" : "false",
        runtime->auto_restart ? "true" : "false",
        runtime->wireless_ds4 ? "true" : "false",
        runtime->health_interval_ms, runtime->disconnect_restart_ms,
        runtime->input_stale_release_ms,
        runtime->ghostpad_enabled ? "true" : "false",
        (unsigned)runtime->ghostpad_port, runtime->ghostpad_stale_ms,
        runtime->dualsense_fx_enabled ? "true" : "false",
        runtime->dualsense_fx_effect,
        runtime->dualsense_fx_period_ms, runtime->dualsense_fx_step_ms,
        runtime->dualsense_fx_r, runtime->dualsense_fx_g,
        runtime->dualsense_fx_b,
        runtime->update_enabled ? "true" : "false",
        runtime->include_prereleases ? "true" : "false",
        runtime->active_profile, (unsigned)runtime->ipc_port,
        runtime->xmb_enabled ? "true" : "false",
        runtime->homeui_tile_owner[0] ? runtime->homeui_tile_owner : "auto",
        runtime->settings_entry_owner[0] ? runtime->settings_entry_owner
                                        : "auto");
    return written < 0 || (size_t)written >= output_size ? -1 : written;
}

static int
ensure_schema(const char *root, char *error, size_t error_size)
{
    static const char *const directories[] = {
        "profiles", "imports", "exports", "logs", "runtime", "updates",
    };
    if (ensure_directory_tree(root, error, error_size) != 0)
        return -1;
    for (size_t index = 0;
         index < sizeof(directories) / sizeof(directories[0]); ++index) {
        char path[GC_CONFIG_PATH_MAX];
        if (join_path(path, sizeof(path), root, directories[index]) != 0 ||
            ensure_directory_tree(path, error, error_size) != 0)
            return -1;
    }

    char path[GC_CONFIG_PATH_MAX];
    if (join_path(path, sizeof(path), root, "config.ini") != 0 ||
        write_if_missing(path, k_default_runtime,
                         sizeof(k_default_runtime) - 1, error,
                         error_size) != 0)
        return -1;
    if (join_path(path, sizeof(path), root, "controllers.ini") != 0 ||
        write_if_missing(path, k_default_controllers,
                         sizeof(k_default_controllers) - 1, error,
                         error_size) != 0)
        return -1;

    GcProfile profile;
    char profile_text[GC_PROFILE_TEXT_MAX];
    gc_profile_defaults(&profile);
    int profile_length =
        gc_profile_serialize(&profile, profile_text, sizeof(profile_text));
    if (profile_length < 0 ||
        join_path(path, sizeof(path), root, "profiles/default.ini") != 0 ||
        write_if_missing(path, profile_text, (size_t)profile_length, error,
                         error_size) != 0)
        return -1;

    gc_profile_defaults(&profile);
    (void)snprintf(profile.id, sizeof(profile.id), "xbox-touchpad");
    (void)snprintf(profile.name, sizeof(profile.name),
                   "Xbox View to touchpad");
    profile.enabled = 0;
    profile.match_any_vid = 0;
    profile.match_vid = 0x045e;
    profile.match_any_pid = 1;
    (void)snprintf(profile.protocol, sizeof(profile.protocol), "xbox_gip");
    profile.button_targets[GC_BUTTON_SHARE] = SCE_PAD_BUTTON_TOUCH_PAD;
    profile_length =
        gc_profile_serialize(&profile, profile_text, sizeof(profile_text));
    if (profile_length < 0 ||
        join_path(path, sizeof(path), root,
                  "profiles/xbox-touchpad.ini") != 0 ||
        write_if_missing(path, profile_text, (size_t)profile_length, error,
                         error_size) != 0)
        return -1;
    return 0;
}

GcConfigStore *
gc_config_open(const char *root, char *error, size_t error_size)
{
    const char *selected_root = root ? root : GC_DATA_ROOT;
    if (strlen(selected_root) >= GC_CONFIG_PATH_MAX) {
        set_error(error, error_size, "Ghostcontrol data root is too long");
        return NULL;
    }
    GcConfigStore *store = calloc(1, sizeof(*store));
    if (!store) {
        set_error(error, error_size, "out of memory creating config store");
        return NULL;
    }
    if (pthread_mutex_init(&store->lock, NULL) != 0) {
        free(store);
        set_error(error, error_size, "cannot initialize config lock");
        return NULL;
    }
    if (pthread_mutex_init(&store->mutation_lock, NULL) != 0) {
        (void)pthread_mutex_destroy(&store->lock);
        free(store);
        set_error(error, error_size, "cannot initialize config mutation lock");
        return NULL;
    }
    atomic_init(&store->generation, 0);
    (void)snprintf(store->root, sizeof(store->root), "%s", selected_root);
    runtime_defaults(&store->runtime);
    if (ensure_schema(store->root, error, error_size) != 0 ||
        gc_config_reload(store, error, error_size) != 0) {
        gc_config_close(store);
        return NULL;
    }
    return store;
}

void
gc_config_close(GcConfigStore *store)
{
    if (!store)
        return;
    (void)pthread_mutex_destroy(&store->mutation_lock);
    (void)pthread_mutex_destroy(&store->lock);
    free(store);
}

static int
store_associations_valid(const GcControllerRule *rules, size_t count,
                         const GcProfile *profiles, size_t profile_count,
                         char *error, size_t error_size)
{
    for (size_t i = 0; i < count; ++i) {
        const GcControllerRule *r = &rules[i];
        if (!r->profile_id[0]) continue;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(r->profile_id, rules[j].profile_id)) goto conflict;
        for (size_t j = 0; j < profile_count; ++j) {
            const GcProfile *q = &profiles[j];
            if (strcmp(r->profile_id, q->id)) continue;
            if (strcmp(r->id, q->owner_controller_id) ||
                strcmp(r->instance_id, q->owner_instance_id)) goto conflict;
        }
    }
    return 0;
conflict:
    set_error(error, error_size, "profile_owner_conflict");
    return -1;
}

static int
reload_locked(GcConfigStore *store, char *error, size_t error_size)
{
    if (store->generation_exhausted ||
        atomic_load_explicit(&store->generation, memory_order_acquire) == UINT64_MAX) {
        set_error(error, error_size, "instance_policy_quarantined");
        return -1;
    }
    GcRuntimeConfig runtime;
    GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    GcControllerRule controllers[GC_CONFIG_MAX_CONTROLLERS];
    size_t profile_count = 0;
    size_t controller_count = 0;
    if (!profiles) {
        set_error(error, error_size, "out of memory loading profiles");
        return -1;
    }
    if (load_runtime(store->root, &runtime, error, error_size) != 0 ||
        load_profiles(store->root, profiles, &profile_count, error,
                      error_size) != 0 ||
        load_controllers(store->root, controllers, &controller_count, error,
                         error_size) != 0 ||
        store_associations_valid(controllers, controller_count, profiles,
                                 profile_count, error, error_size) != 0) {
        free(profiles);
        return -1;
    }

    pthread_mutex_lock(&store->lock);
    store->runtime = runtime;
    memcpy(store->profiles, profiles, profile_count * sizeof(profiles[0]));
    store->profile_count = profile_count;
    memcpy(store->controllers, controllers,
           controller_count * sizeof(controllers[0]));
    store->controller_count = controller_count;
    store->instance_policy_valid = 1;
    (void)atomic_fetch_add_explicit(&store->generation, 1,
                                    memory_order_release);
    pthread_mutex_unlock(&store->lock);
    free(profiles);
    return 0;
}

int
gc_config_reload(GcConfigStore *store, char *error, size_t error_size)
{
    if (!store) {
        set_error(error, error_size, "config store is required");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    int result = reload_locked(store, error, error_size);
    if (result) policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_EXTERNAL_RELOAD_FAILURE);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_snapshot(GcConfigStore *store, GcRuntimeConfig *runtime,
                   uint64_t *generation)
{
    if (!store || !runtime)
        return -1;
    pthread_mutex_lock(&store->lock);
    *runtime = store->runtime;
    if (generation)
        *generation = atomic_load_explicit(&store->generation,
                                           memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return 0;
}

size_t
gc_config_profiles(GcConfigStore *store, GcProfile *profiles,
                   size_t capacity, uint64_t *generation)
{
    if (!store)
        return 0;
    pthread_mutex_lock(&store->lock);
    size_t count = store->profile_count;
    size_t copy_count = count < capacity ? count : capacity;
    if (profiles && copy_count)
        memcpy(profiles, store->profiles, copy_count * sizeof(profiles[0]));
    if (generation)
        *generation = atomic_load_explicit(&store->generation,
                                           memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return count;
}

size_t
gc_config_controllers(GcConfigStore *store, GcControllerRule *rules,
                      size_t capacity, uint64_t *generation)
{
    if (!store)
        return 0;
    pthread_mutex_lock(&store->lock);
    size_t count = store->controller_count;
    size_t copy_count = count < capacity ? count : capacity;
    if (rules && copy_count)
        memcpy(rules, store->controllers,
               copy_count * sizeof(store->controllers[0]));
    if (generation)
        *generation = atomic_load_explicit(&store->generation,
                                           memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return count;
}

int
gc_config_find_controller_by_id(GcConfigStore *store, const char *id,
                                 GcControllerRule *out, uint64_t *generation)
{
    if (!store || !id || !out || strlen(id) >= GC_PROFILE_ID_MAX || !valid_profile_id(id)) return -1;
    int result = -1;
    pthread_mutex_lock(&store->lock);
    for (size_t i = 0; i < store->controller_count; ++i)
        if (!strcmp(id, store->controllers[i].id)) { *out = store->controllers[i]; result = 0; break; }
    if (generation) *generation = atomic_load_explicit(&store->generation, memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return result;
}

static int
instance_protocol_matches(const char *a, const char *b)
{
    return !strcasecmp(a, b) || (!strcasecmp(a, "hid") && !strcasecmp(b, "dinput")) ||
        (!strcasecmp(a, "dinput") && !strcasecmp(b, "hid"));
}

static int
controller_match_valid(const GcControllerMatch *m)
{
    if (!m || !memchr(m->session_id, 0, sizeof(m->session_id)) ||
        !memchr(m->path, 0, sizeof(m->path)) || !memchr(m->protocol, 0, sizeof(m->protocol)) ||
        !memchr(m->identity.instance_id, 0, sizeof(m->identity.instance_id)) ||
        !m->path[0] || !m->protocol[0] || strlen(m->session_id) != 32 ||
        !m->connection_generation || !m->occurrence_generation ||
        m->transport < GC_INSTANCE_TRANSPORT_UNKNOWN || m->transport > GC_INSTANCE_TRANSPORT_LE ||
        m->identity.state < GC_INSTANCE_UNKNOWN || m->identity.state > GC_INSTANCE_COLLISION) return 0;
    for (size_t i = 0; i < 32; ++i)
        if (!((m->session_id[i] >= '0' && m->session_id[i] <= '9') ||
              (m->session_id[i] >= 'a' && m->session_id[i] <= 'f'))) return 0;
    if (m->transport != GC_INSTANCE_TRANSPORT_USB &&
        (m->interface_index != -1 || m->interface_number != -1 || m->alternate_setting != -1 ||
         m->in_endpoint || m->out_endpoint)) return 0;
    if (m->interface_index < -1 || m->interface_index > 255 ||
        m->interface_number < -1 || m->interface_number > 255 ||
        m->alternate_setting < -1 || m->alternate_setting > 255) return 0;
    if (m->identity.state == GC_INSTANCE_STABLE)
        return m->transport == GC_INSTANCE_TRANSPORT_CLASSIC &&
            m->identity.proof == GC_INSTANCE_PROOF_CLASSIC_PEER &&
            valid_instance_selector(m->identity.instance_id);
    return m->identity.proof == GC_INSTANCE_PROOF_NONE && !m->identity.instance_id[0];
}

/* Exactly one snapshot joins classification, rule, profile and publication. */
int
gc_config_resolve_controller(GcConfigStore *store, const GcControllerMatch *match,
                             GcControllerResolution *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!store || !out || !controller_match_valid(match)) return -1;
    pthread_mutex_lock(&store->lock);
    out->generation = atomic_load_explicit(&store->generation, memory_order_acquire);
    if (!store->instance_policy_valid || store->generation_exhausted) {
        out->kind = GC_RESOLVE_POLICY_UNAVAILABLE; goto done;
    }
    if (match->identity.state == GC_INSTANCE_COLLISION) {
        out->kind = GC_RESOLVE_IDENTITY_COLLISION; goto done;
    }
    const GcControllerRule *rule = NULL, *model = NULL;
    int has_overrides = 0;
    for (size_t i = 0; i < store->controller_count; ++i) {
        const GcControllerRule *candidate = &store->controllers[i];
        if (candidate->vid != match->vid || candidate->pid != match->pid) continue;
        if (!candidate->instance_id[0]) model = candidate;
        else {
            has_overrides = 1;
            if (match->identity.state == GC_INSTANCE_STABLE &&
                !strcmp(candidate->instance_id, match->identity.instance_id)) rule = candidate;
        }
    }
    if (has_overrides && match->identity.state != GC_INSTANCE_STABLE) {
        out->kind = GC_RESOLVE_IDENTITY_UNAVAILABLE; goto done;
    }
    if (rule && store->product_mode != GC_INSTANCE_PRODUCT_RESIDENT_STORAGE_LIVE) {
        out->kind = GC_RESOLVE_POLICY_UNAVAILABLE; goto done;
    }
    int exact_instance = rule != NULL;
    if (!rule) rule = model;
    if (rule) {
        out->has_rule = 1; out->rule = *rule;
        if (!rule->enabled || rule->ignore) { out->kind = GC_RESOLVE_VETO; goto done; }
        if (!instance_protocol_matches(rule->protocol, match->protocol) ||
            (rule->preferred_interface_number != -1 && rule->preferred_interface_number != match->interface_number) ||
            (rule->preferred_in_endpoint && rule->preferred_in_endpoint != match->in_endpoint) ||
            (rule->preferred_out_endpoint && rule->preferred_out_endpoint != match->out_endpoint)) {
            out->kind = GC_RESOLVE_BINDING_MISMATCH; goto done;
        }
    }
    if (exact_instance) {
        out->kind = GC_RESOLVE_PROFILE_UNAVAILABLE;
        for (size_t i = 0; i < store->profile_count; ++i) {
            const GcProfile *p = &store->profiles[i];
            if (!rule->profile_id[0] || strcmp(rule->profile_id, p->id)) continue;
            if (!p->enabled || strcmp(p->owner_controller_id, rule->id) ||
                strcmp(p->owner_instance_id, rule->instance_id) || p->match_any_vid || p->match_any_pid ||
                p->match_vid != match->vid || p->match_pid != match->pid ||
                !instance_protocol_matches(p->protocol, match->protocol)) goto done;
            out->kind = GC_RESOLVE_INSTANCE; out->has_profile = 1; out->profile = *p; break;
        }
        goto done;
    }
    out->kind = rule ? GC_RESOLVE_MODEL : GC_RESOLVE_NONE;
    int selected = -1, best_score = -1;
    if (strcasecmp(store->runtime.active_profile, "auto")) {
        for (size_t i = 0; i < store->profile_count; ++i) {
            const GcProfile *p = &store->profiles[i];
            if (!strcmp(store->runtime.active_profile, p->id) && p->enabled &&
                !p->owner_controller_id[0] && !p->owner_instance_id[0]) { selected = (int)i; break; }
        }
    } else {
        for (size_t i = 0; i < store->profile_count; ++i) {
            int score = gc_profile_match_score(&store->profiles[i], match->vid, match->pid, match->protocol);
            if (score > best_score) { selected = (int)i; best_score = score; }
        }
    }
    if (selected >= 0) { out->has_profile = 1; out->profile = store->profiles[selected]; }
done:
    pthread_mutex_unlock(&store->lock);
    return 0;
}

int
gc_config_find_controller(GcConfigStore *store, uint16_t vid, uint16_t pid,
                          GcControllerRule *rule, uint64_t *generation)
{
    if (!store || !rule)
        return -1;
    int found = -1;
    pthread_mutex_lock(&store->lock);
    for (size_t index = 0; index < store->controller_count; ++index) {
        if (store->instance_policy_valid && !store->generation_exhausted &&
            !store->controllers[index].instance_id[0] &&
            store->controllers[index].enabled &&
            store->controllers[index].vid == vid &&
            store->controllers[index].pid == pid) {
            *rule = store->controllers[index];
            found = 0;
            break;
        }
    }
    if (generation)
        *generation = atomic_load_explicit(&store->generation,
                                           memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return found;
}

int
gc_config_find_controller_including_disabled(GcConfigStore *store,
                                              uint16_t vid, uint16_t pid,
                                              GcControllerRule *rule,
                                              uint64_t *generation)
{
    if (!store || !rule)
        return -1;
    int found = -1;
    pthread_mutex_lock(&store->lock);
    for (size_t index = 0; index < store->controller_count; ++index) {
        if (!store->controllers[index].instance_id[0] &&
            store->controllers[index].vid == vid &&
            store->controllers[index].pid == pid) {
            *rule = store->controllers[index];
            found = 0;
            break;
        }
    }
    if (generation)
        *generation = atomic_load_explicit(&store->generation,
                                           memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return found;
}

int
gc_config_find_exact_profile(GcConfigStore *store, uint16_t vid, uint16_t pid,
                             const char *protocol, GcProfile *profile,
                             uint64_t *generation)
{
    if (!store || !profile)
        return -1;
    int selected = -1;
    int best_score = -1;
    pthread_mutex_lock(&store->lock);
    for (size_t index = 0; index < store->profile_count; ++index) {
        const GcProfile *candidate = &store->profiles[index];
        if (!store->instance_policy_valid || store->generation_exhausted ||
            candidate->match_any_vid || candidate->match_any_pid)
            continue;
        int score = gc_profile_match_score(candidate, vid, pid, protocol);
        if (score > best_score ||
            (score == best_score && score >= 0 && selected >= 0 &&
             strcmp(candidate->id, store->profiles[selected].id) < 0)) {
            best_score = score;
            selected = (int)index;
        }
    }
    if (selected >= 0)
        *profile = store->profiles[selected];
    if (generation)
        *generation = atomic_load_explicit(&store->generation,
                                           memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return selected >= 0 ? 0 : -1;
}

int
gc_config_select_profile(GcConfigStore *store, uint16_t vid, uint16_t pid,
                         const char *protocol, GcProfile *profile,
                         uint64_t *generation)
{
    if (!store || !profile)
        return -1;
    pthread_mutex_lock(&store->lock);
    int selected = -1;
    if (!store->instance_policy_valid || store->generation_exhausted) {
        /* Retained profiles are diagnostics, not matching policy. */
    } else if (strcasecmp(store->runtime.active_profile, "auto") != 0) {
        for (size_t index = 0; index < store->profile_count; ++index) {
            if (strcmp(store->profiles[index].id,
                       store->runtime.active_profile) == 0 &&
                store->profiles[index].enabled &&
                !store->profiles[index].owner_controller_id[0] &&
                !store->profiles[index].owner_instance_id[0]) {
                selected = (int)index;
                break;
            }
        }
    } else {
        int best_score = -1;
        for (size_t index = 0; index < store->profile_count; ++index) {
            int score = gc_profile_match_score(&store->profiles[index], vid,
                                               pid, protocol);
            if (score > best_score ||
                (score == best_score && score >= 0 && selected >= 0 &&
                 strcmp(store->profiles[index].id,
                        store->profiles[selected].id) < 0)) {
                best_score = score;
                selected = (int)index;
            }
        }
    }
    if (selected >= 0)
        *profile = store->profiles[selected];
    if (generation)
        *generation = atomic_load_explicit(&store->generation,
                                           memory_order_acquire);
    pthread_mutex_unlock(&store->lock);
    return selected >= 0 ? 0 : -1;
}

int
gc_config_save_runtime(GcConfigStore *store,
                       const GcRuntimeConfig *runtime,
                       char *error, size_t error_size)
{
    if (!store || !runtime)
        return -1;
    char text[2048];
    int length = serialize_runtime(runtime, text, sizeof(text));
    char path[GC_CONFIG_PATH_MAX];
    if (length < 0 ||
        join_path(path, sizeof(path), store->root, "config.ini") != 0) {
        set_error(error, error_size, "invalid runtime config");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    if (policy_check_held(store, error, error_size)) {
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    for (size_t i = 0; i < store->profile_count; ++i)
        if (!strcmp(runtime->active_profile, store->profiles[i].id) &&
            (store->profiles[i].owner_controller_id[0] || store->profiles[i].owner_instance_id[0])) {
            set_error(error, error_size, "profile_owned_mutation_forbidden");
            pthread_mutex_unlock(&store->mutation_lock); return -1;
        }
    char *old = NULL; size_t old_length = 0; int existed = 0;
    int result = snapshot_optional_file(path, &old, &old_length, &existed, error, error_size);
    if (!result) result = atomic_write(path, text, (size_t)length, error, error_size);
    if (!result && reload_locked(store, error, error_size)) {
        if (restore_file(path, old, old_length, existed, NULL, 0)) {
            policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
            set_error(error, error_size, "instance_policy_quarantined");
        }
        result = -1;
    }
    free(old);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

/* Serializes and writes one profile under mutation_lock. Split out so a
 * read-modify-write edit can reuse it while holding the same transaction,
 * and so save and edit cannot interleave. */
static int
save_profile_locked(GcConfigStore *store, const char *id,
                    const GcProfile *profile, char *error, size_t error_size)
{
    if (policy_check_held(store, error, error_size)) return -1;
    char text[GC_PROFILE_TEXT_MAX];
    int length = gc_profile_serialize(profile, text, sizeof(text));
    GcProfile checked;
    if (length < 0 || gc_profile_parse(text, (size_t)length, &checked, error, error_size)) {
        set_error(error, error_size, "invalid profile");
        return -1;
    }
    char relative[GC_PROFILE_ID_MAX + 16];
    char path[GC_CONFIG_PATH_MAX];
    int relative_length = snprintf(relative, sizeof(relative),
                                   "profiles/%s.ini", id);
    if (relative_length < 0 ||
        (size_t)relative_length >= sizeof(relative) ||
        join_path(path, sizeof(path), store->root, relative) != 0) {
        set_error(error, error_size, "profile path is too long");
        return -1;
    }
    char *old = NULL;
    size_t old_length = 0;
    int existed = 0;
    int result = snapshot_optional_file(path, &old, &old_length, &existed,
                                        error, error_size);
    if (result == 0)
        result = atomic_write(path, text, (size_t)length, error, error_size);
    if (result == 0 && reload_locked(store, error, error_size) != 0) {
        char failure[256] = "profile reload failed";
        if (error && error_size && *error)
            snprintf(failure, sizeof(failure), "%s", error);
        char rollback_error[256] = "";
        if (restore_file(path, old, old_length, existed, rollback_error,
                         sizeof(rollback_error)) != 0) {
            policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
            set_error(error, error_size, "instance_policy_quarantined");
        } else {
            set_error(error, error_size, "%s; profile was not changed",
                      failure);
        }
        result = -1;
    }
    free(old);
    return result;
}

int
gc_config_save_profile(GcConfigStore *store, const GcProfile *profile,
                       char *error, size_t error_size)
{
    if (!store || !profile || !memchr(profile->id, 0, sizeof(profile->id)))
        return -1;
    pthread_mutex_lock(&store->mutation_lock);
    if (policy_check_held(store, error, error_size)) {
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    int protected_target = 0, owned_existing = 0;
    if (!memchr(profile->name, 0, sizeof(profile->name)) ||
        !memchr(profile->protocol, 0, sizeof(profile->protocol)) ||
        !memchr(profile->owner_controller_id, 0, sizeof(profile->owner_controller_id)) ||
        !memchr(profile->owner_instance_id, 0, sizeof(profile->owner_instance_id))) {
        set_error(error, error_size, "invalid profile");
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    for (size_t i = 0; i < store->profile_count; ++i) {
        const GcProfile *old = &store->profiles[i];
        if (strcmp(profile->id, old->id)) continue;
        if (old->owner_controller_id[0] || old->owner_instance_id[0]) {
            owned_existing = 1;
            protected_target = strcmp(profile->owner_controller_id, old->owner_controller_id) ||
                strcmp(profile->owner_instance_id, old->owner_instance_id) ||
                profile->schema != old->schema || strcmp(profile->name, old->name) ||
                profile->match_any_vid != old->match_any_vid || profile->match_any_pid != old->match_any_pid ||
                profile->match_vid != old->match_vid || profile->match_pid != old->match_pid ||
                strcmp(profile->protocol, old->protocol);
        }
    }
    if (!owned_existing) {
        protected_target = profile->owner_controller_id[0] || profile->owner_instance_id[0];
        for (size_t i = 0; i < store->controller_count; ++i)
            protected_target |= store->controllers[i].profile_id[0] &&
                !strcmp(profile->id, store->controllers[i].profile_id);
    }
    if (owned_existing && gc_config_instance_check_held(store, GC_INSTANCE_REQUIRE_STORAGE, error, error_size)) {
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    if (protected_target) {
        set_error(error, error_size, "profile_owned_mutation_forbidden");
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    int result = save_profile_locked(store, profile->id, profile, error,
                                     error_size);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_edit_profile(GcConfigStore *store, const char *id, const char *key,
                       const char *value, char *error, size_t error_size)
{
    if (!store || !valid_profile_id(id) || strlen(id) >= GC_PROFILE_ID_MAX) {
        set_error(error, error_size, "invalid profile id");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    if (policy_check_held(store, error, error_size)) {
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles) {
        set_error(error, error_size, "out of memory reading profiles");
        pthread_mutex_unlock(&store->mutation_lock);
        return -1;
    }
    size_t count = gc_config_profiles(store, profiles,
                                      GC_CONFIG_MAX_PROFILES, NULL);
    size_t index = count;
    for (size_t candidate = 0; candidate < count; ++candidate)
        if (strcmp(profiles[candidate].id, id) == 0) {
            index = candidate;
            break;
        }
    int result = -1;
    if (index == count) {
        set_error(error, error_size, "profile not found");
    } else {
        /* Edit a copy and write it back under the same lock, so a failed
         * parse leaves the saved file untouched. */
        GcProfile edited = profiles[index];
        if ((edited.owner_controller_id[0] || edited.owner_instance_id[0]) &&
            gc_config_instance_check_held(store, GC_INSTANCE_REQUIRE_STORAGE, error, error_size)) {
            free(profiles); pthread_mutex_unlock(&store->mutation_lock); return -1;
        }
        if (gc_profile_apply_key(&edited, key, value, error, error_size) == 0)
            result = save_profile_locked(store, id, &edited, error,
                                         error_size);
    }
    free(profiles);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

/* Caller owns mutation_lock: readback and generation belong to the same
 * publication, rather than unrelated snapshots taken by an IPC adapter. */
static int
controller_readback_locked(GcConfigStore *store, const char *id,
                           GcControllerRule *saved, uint64_t *generation)
{
    int result = -1;
#ifdef GC_HOST_TEST_SETUP_READBACK_FAILURE
    /* Host fixture link seam only. Fail the real post-commit readback without
     * modifying the writer, published state, outputs, or lock ownership.
     * No symbol/call exists in production or default host builds. */
    extern int gc_config_host_test_setup_readback_failure(void);
    if (gc_config_host_test_setup_readback_failure()) return -1;
#endif
    pthread_mutex_lock(&store->lock);
    for (size_t i = 0; i < store->controller_count; ++i) {
        if (!strcmp(store->controllers[i].id, id)) {
            if (saved) *saved = store->controllers[i];
            if (generation)
                *generation = atomic_load_explicit(&store->generation,
                                                  memory_order_acquire);
            result = 0;
            break;
        }
    }
    pthread_mutex_unlock(&store->lock);
    return result;
}

static int
controller_rule_equal(const GcControllerRule *a, const GcControllerRule *b)
{
    return !strcmp(a->id, b->id) && !strcmp(a->name, b->name) &&
        a->enabled == b->enabled && a->ignore == b->ignore && a->vid == b->vid && a->pid == b->pid &&
        !strcmp(a->protocol, b->protocol) && a->preferred_in_endpoint == b->preferred_in_endpoint &&
        a->preferred_out_endpoint == b->preferred_out_endpoint &&
        a->preferred_interface_number == b->preferred_interface_number && a->output_type == b->output_type &&
        !strcmp(a->instance_id, b->instance_id) && !strcmp(a->profile_id, b->profile_id);
}

static int
instance_mutate_held(GcConfigStore *store, const GcControllerInstanceMutation *intent,
                     GcControllerInstanceMutationResult *published,
                     char *error, size_t error_size)
{
    const GcControllerRule *input = &intent->rule;
    GcControllerInstanceMutationMode mode = intent->mode;
    if (mode < GC_INSTANCE_MUTATION_FULL || mode > GC_INSTANCE_MUTATION_OUTPUT_SET ||
        (intent->output_present != 0 && intent->output_present != 1) ||
        !memchr(input->id, 0, sizeof(input->id)) || !valid_profile_id(input->id) ||
        !strcmp(input->id, "auto") || !strcmp(input->id, "default")) goto invalid;
    if (mode == GC_INSTANCE_MUTATION_OUTPUT_SET) {
        if (intent->output_present != 1 || !gc_controller_output_name(input->output_type) ||
            input->name[0] || input->enabled || input->ignore || input->vid || input->pid ||
            input->protocol[0] || input->preferred_in_endpoint || input->preferred_out_endpoint ||
            input->preferred_interface_number || input->instance_id[0] || input->profile_id[0]) goto invalid;
    } else {
        int explicit_output = mode == GC_INSTANCE_MUTATION_FULL || mode == GC_INSTANCE_MUTATION_LEGACY11;
        if (intent->output_present != explicit_output ||
            (!explicit_output && input->output_type != 0) ||
            (mode == GC_INSTANCE_MUTATION_LEGACY9 && input->preferred_interface_number != 0) ||
            (mode != GC_INSTANCE_MUTATION_FULL && (input->instance_id[0] || input->profile_id[0])) ||
            !controller_rule_valid(input) ||
            (mode == GC_INSTANCE_MUTATION_FULL && !input->instance_id[0])) goto invalid;
    }
    if (gc_config_instance_check_held(store, GC_INSTANCE_REQUIRE_STORAGE, error, error_size)) return -1;
    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS];
    size_t count = store->controller_count, index;
    memcpy(rules, store->controllers, count * sizeof(rules[0]));
    for (index = 0; index < count; ++index) if (!strcmp(rules[index].id, input->id)) break;
    GcControllerRule merged;
    if (index == count) {
        if (mode != GC_INSTANCE_MUTATION_FULL) goto invalid;
        if (input->profile_id[0]) { set_error(error, error_size, "profile_owner_conflict"); return -1; }
        if (count == GC_CONFIG_MAX_CONTROLLERS) { set_error(error, error_size, "controller_limit"); return -1; }
        merged = *input; ++count;
    } else {
        const GcControllerRule *old = &rules[index];
        if (!old->instance_id[0]) goto retarget;
        if (mode == GC_INSTANCE_MUTATION_OUTPUT_SET) {
            merged = *old; merged.output_type = input->output_type;
        } else {
            if (old->vid != input->vid || old->pid != input->pid ||
                (mode == GC_INSTANCE_MUTATION_FULL &&
                 (strcmp(old->instance_id, input->instance_id) || strcmp(old->profile_id, input->profile_id)))) goto retarget;
            merged = *input;
            snprintf(merged.instance_id, sizeof(merged.instance_id), "%s", old->instance_id);
            snprintf(merged.profile_id, sizeof(merged.profile_id), "%s", old->profile_id);
            if (mode == GC_INSTANCE_MUTATION_LEGACY9) merged.preferred_interface_number = -1;
            if (!intent->output_present) merged.output_type = old->output_type;
            if (old->profile_id[0] && (strcmp(old->protocol, merged.protocol) ||
                old->preferred_interface_number != merged.preferred_interface_number ||
                old->preferred_in_endpoint != merged.preferred_in_endpoint ||
                old->preferred_out_endpoint != merged.preferred_out_endpoint)) goto retarget;
        }
    }
    rules[index] = merged;
    if (store_associations_valid(rules, count, store->profiles, store->profile_count, error, error_size)) return -1;
    char text[GC_CONFIG_FILE_MAX], path[GC_CONFIG_PATH_MAX];
    int length = serialize_controllers(rules, count, text, sizeof(text), error, error_size);
    if (length < 0) { set_error(error, error_size, "duplicate_instance_rule"); return -1; }
    if (join_path(path, sizeof(path), store->root, "controllers.ini")) goto invalid;
    char *old_text = NULL; size_t old_length = 0; int existed = 0;
    if (snapshot_optional_file(path, &old_text, &old_length, &existed, error, error_size)) {
        set_error(error, error_size, "publication_failed"); return -1;
    }
    int result = atomic_write(path, text, (size_t)length, error, error_size);
    /* Failure classification must not depend on a bounded caller diagnostic. */
    const char *failure = "publication_failed";
    if (!result && reload_locked(store, error, error_size)) {
        if (restore_file(path, old_text, old_length, existed, NULL, 0)) {
            policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
            failure = "instance_policy_quarantined";
        }
        result = -1;
    }
    free(old_text);
    if (result) {
        set_error(error, error_size, failure);
        return -1;
    }
    published->committed = 1;
    GcControllerRule saved; uint64_t generation = 0;
    if (controller_readback_locked(store, merged.id, &saved, &generation) ||
        !controller_rule_equal(&saved, &merged)) {
        set_error(error, error_size, "publication_readback_failed"); return -1;
    }
    pthread_mutex_lock(&store->lock);
    published->rule = saved; published->generation = generation;
    for (size_t i = 0; i < store->profile_count; ++i)
        if (saved.profile_id[0] && !strcmp(saved.profile_id, store->profiles[i].id)) {
            published->profile = store->profiles[i]; published->has_profile = 1; break;
        }
    published->verified = 1;
    pthread_mutex_unlock(&store->lock);
    if (error && error_size) error[0] = 0;
    return 0;
invalid:
    set_error(error, error_size, "invalid_instance_request"); return -1;
retarget:
    set_error(error, error_size, "instance_retarget_forbidden"); return -1;
}

int
gc_config_mutate_controller_instance(GcConfigStore *store,
    const GcControllerInstanceMutation *intent, GcControllerInstanceMutationResult *published,
    char *error, size_t error_size)
{
    if (published) memset(published, 0, sizeof(*published));
    if (!store || !intent || !published) {
        set_error(error, error_size, "invalid_instance_request"); return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    int result = instance_mutate_held(store, intent, published, error, error_size);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

/* Writer-owned MODEL-only persistence. Optional typed output is used ONLY by
 * the additive compatibility route; legacy callers keep their old projection. */
static int
save_model_controller_held(GcConfigStore *store, const GcControllerRule *rule,
    int preserve_output_type, GcControllerRule *saved, uint64_t *generation,
    GcControllerInstanceMutationResult *published, char *error, size_t error_size)
{
    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS];
    size_t count = gc_config_controllers(store, rules, GC_CONFIG_MAX_CONTROLLERS, NULL);
    size_t index;
    for (index = 0; index < count; ++index)
        if (!strcmp(rules[index].id, rule->id)) break;
    if (index < count &&
        (rules[index].vid != rule->vid || rules[index].pid != rule->pid)) {
        set_error(error, error_size,
                  "controller rule id already belongs to another VID:PID");
        return -1;
    }
    GcControllerRule merged = *rule;
    if (preserve_output_type && index < count)
        merged.output_type = rules[index].output_type;
    if (index == count) {
        if (count >= GC_CONFIG_MAX_CONTROLLERS) {
            set_error(error, error_size, "controller rule limit reached");
            return -1;
        }
        ++count;
    }
    rules[index] = merged;
    char text[GC_CONFIG_FILE_MAX];
    int length = serialize_controllers(rules, count, text, sizeof(text), error,
                                       error_size);
    char path[GC_CONFIG_PATH_MAX];
    if (length < 0 ||
        join_path(path, sizeof(path), store->root, "controllers.ini") != 0) {
        if (!error || !error_size || !*error)
            set_error(error, error_size, "invalid controller rules");
        return -1;
    }
    char *old = NULL;
    size_t old_length = 0;
    int existed = 0;
    int result = snapshot_optional_file(path, &old, &old_length, &existed,
                                        error, error_size);
    if (result == 0)
        result = atomic_write(path, text, (size_t)length, error, error_size);
    if (result == 0 && reload_locked(store, error, error_size) != 0) {
        char failure[256] = "controller reload failed";
        char rollback_error[256] = "";
        if (error && error_size && *error)
            snprintf(failure, sizeof(failure), "%s", error);
        if (restore_file(path, old, old_length, existed, rollback_error,
                         sizeof(rollback_error)) != 0) {
            policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
            set_error(error, error_size, "instance_policy_quarantined");
        } else
            set_error(error, error_size, "%s; controller rules were not changed",
                      failure);
        /* Failed reload publishes neither rules nor generation. Restoring the
         * file is sufficient; a second reload would advance the generation. */
        result = -1;
    }
    free(old);
    if (result == 0) {
        if (!published) {
            /* Preserve the old APIs' exact output/error projection. */
            result = controller_readback_locked(store, merged.id, saved, generation);
        } else {
            /* Actual write + reload publication succeeded. No later diagnostic
             * or failed readback can turn this committed save into rejection. */
            published->committed = 1;
            GcControllerRule verified_rule;
            uint64_t verified_generation = 0;
            if (controller_readback_locked(store, merged.id, &verified_rule,
                                           &verified_generation) ||
                !controller_rule_equal(&verified_rule, &merged)) {
                set_error(error, error_size, "publication_readback_failed");
                return -1;
            }
            published->rule = verified_rule;
            published->generation = verified_generation;
            published->verified = 1;
            if (error && error_size) error[0] = 0;
        }
    }
    return result;
}

static int
save_controller_locked(GcConfigStore *store, const GcControllerRule *rule,
                       int preserve_output_type, GcControllerRule *saved,
                       uint64_t *generation, char *error, size_t error_size)
{
    if (policy_check_held(store, error, error_size)) return -1;
    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS];
    size_t count = gc_config_controllers(store, rules,
                                         GC_CONFIG_MAX_CONTROLLERS, NULL);
    size_t index;
    for (index = 0; index < count; ++index)
        if (strcmp(rules[index].id, rule->id) == 0)
            break;
    if (rule->instance_id[0] || (index < count && rules[index].instance_id[0])) {
        GcControllerInstanceMutation intent = {0};
        intent.rule = *rule;
        intent.mode = rule->instance_id[0] ? GC_INSTANCE_MUTATION_FULL :
            (preserve_output_type ? GC_INSTANCE_MUTATION_LEGACY10 : GC_INSTANCE_MUTATION_LEGACY11);
        intent.output_present = !preserve_output_type;
        if (intent.mode == GC_INSTANCE_MUTATION_FULL) {
            intent.output_present = 1;
            if (preserve_output_type && index < count) intent.rule.output_type = rules[index].output_type;
        }
        if (preserve_output_type && intent.mode != GC_INSTANCE_MUTATION_FULL) intent.rule.output_type = 0;
        GcControllerInstanceMutationResult publication = {0};
        int result = instance_mutate_held(store, &intent, &publication, error, error_size);
        if (!result && publication.verified) {
            if (saved) *saved = publication.rule;
            if (generation) *generation = publication.generation;
        }
        return result;
    }
    return save_model_controller_held(store, rule, preserve_output_type,
                                      saved, generation, NULL, error, error_size);
}

int
gc_config_save_controller_verified(GcConfigStore *store,
    const GcControllerRule *rule, int preserve_output_type,
    GcControllerRule *saved, uint64_t *generation, char *error, size_t error_size)
{
    if (!store || !controller_rule_valid(rule) ||
        (preserve_output_type != 0 && preserve_output_type != 1)) {
        set_error(error, error_size, "invalid controller rule");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    int result = save_controller_locked(store, rule, preserve_output_type,
                                        saved, generation, error, error_size);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_save_controller(GcConfigStore *store, const GcControllerRule *rule,
                          char *error, size_t error_size)
{
    return gc_config_save_controller_verified(store, rule, 0, NULL, NULL,
                                              error, error_size);
}

int
gc_config_set_controller_output(GcConfigStore *store, const char *id,
    GcControllerOutputType output_type, GcControllerRule *saved,
    uint64_t *generation, char *error, size_t error_size)
{
    if (!store || !valid_profile_id(id) || strlen(id) >= GC_PROFILE_ID_MAX ||
        !gc_controller_output_name(output_type)) {
        set_error(error, error_size, "invalid controller output preference");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    GcControllerRule rule;
    int result = controller_readback_locked(store, id, &rule, NULL);
    if (result == 0) {
        if (rule.instance_id[0]) {
            GcControllerInstanceMutation intent = { .mode = GC_INSTANCE_MUTATION_OUTPUT_SET, .output_present = 1 };
            snprintf(intent.rule.id, sizeof(intent.rule.id), "%s", id);
            intent.rule.output_type = output_type;
            GcControllerInstanceMutationResult publication = {0};
            result = instance_mutate_held(store, &intent, &publication, error, error_size);
            if (!result && publication.verified) {
                if (saved) *saved = publication.rule;
                if (generation) *generation = publication.generation;
            }
        } else {
            rule.output_type = output_type;
            result = save_controller_locked(store, &rule, 0, saved, generation,
                                             error, error_size);
        }
    } else set_error(error, error_size, "controller rule not found");
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

/* Check only used fields, with bounded string validation supplied by the
 * existing rule parser. Unused strings must be empty, not zero-filled padding. */
static int
compat_intent_valid(const GcControllerInstanceMutation *intent)
{
    const GcControllerRule *r = &intent->rule;
    if (intent->output_present != 0 && intent->output_present != 1) return 0;
    if (intent->mode == GC_INSTANCE_MUTATION_OUTPUT_SET) {
        return intent->output_present == 1 && gc_controller_output_name(r->output_type) &&
            !r->name[0] && !r->enabled && !r->ignore && !r->vid && !r->pid &&
            !r->protocol[0] && !r->preferred_in_endpoint && !r->preferred_out_endpoint &&
            !r->preferred_interface_number && !r->instance_id[0] && !r->profile_id[0];
    }
    int explicit_output = intent->mode == GC_INSTANCE_MUTATION_LEGACY11;
    return intent->output_present == explicit_output &&
        (explicit_output || r->output_type == 0) &&
        (intent->mode != GC_INSTANCE_MUTATION_LEGACY9 || r->preferred_interface_number == 0) &&
        !r->instance_id[0] && !r->profile_id[0] && controller_rule_valid(r);
}

int
gc_config_mutate_controller_compat(GcConfigStore *store,
    const GcControllerInstanceMutation *intent, GcControllerMutationScope *scope,
    GcControllerInstanceMutationResult *published, char *error, size_t error_size)
{
    if (scope) *scope = GC_CONTROLLER_MUTATION_SCOPE_NONE;
    if (published) memset(published, 0, sizeof(*published));
    if (!store || !intent || !scope || !published ||
        intent->mode < GC_INSTANCE_MUTATION_LEGACY9 ||
        intent->mode > GC_INSTANCE_MUTATION_OUTPUT_SET ||
        !memchr(intent->rule.id, 0, sizeof(intent->rule.id)) ||
        !valid_profile_id(intent->rule.id)) {
        set_error(error, error_size, "invalid_instance_request");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    /* Classify the latest retained publication, not fallible readback or disk.
     * Quarantined retained rows select diagnostic families, never admission. */
    size_t index;
    for (index = 0; index < store->controller_count; ++index)
        if (!strcmp(store->controllers[index].id, intent->rule.id)) break;
    int found = index < store->controller_count;
    if (found)
        *scope = store->controllers[index].instance_id[0] ?
            GC_CONTROLLER_MUTATION_SCOPE_INSTANCE : GC_CONTROLLER_MUTATION_SCOPE_MODEL;
    else if (intent->mode != GC_INSTANCE_MUTATION_OUTPUT_SET)
        *scope = GC_CONTROLLER_MUTATION_SCOPE_MODEL;

    int result = -1;
    if (!compat_intent_valid(intent)) {
        set_error(error, error_size, "%s", *scope == GC_CONTROLLER_MUTATION_SCOPE_INSTANCE ?
            "invalid_instance_request" : intent->mode == GC_INSTANCE_MUTATION_OUTPUT_SET ?
            "invalid controller output preference" : "invalid controller rule");
        goto release;
    }
    if (!found && intent->mode == GC_INSTANCE_MUTATION_OUTPUT_SET) {
        set_error(error, error_size, "controller rule not found");
        goto release;
    }
    if (policy_check_held(store, error, error_size)) goto release;
    if (*scope == GC_CONTROLLER_MUTATION_SCOPE_INSTANCE) {
        result = instance_mutate_held(store, intent, published, error, error_size);
    } else {
        GcControllerRule merged = intent->rule;
        if (intent->mode == GC_INSTANCE_MUTATION_OUTPUT_SET) {
            merged = store->controllers[index];
            merged.output_type = intent->rule.output_type;
        } else if (intent->mode == GC_INSTANCE_MUTATION_LEGACY9) {
            merged.preferred_interface_number = -1;
        }
        /* Fixed private diagnostics keep rollback classification independent
         * of caller capacity. Never infer commit truth from error text. */
        char diagnostic[256] = "";
        result = save_model_controller_held(store, &merged,
            intent->mode == GC_INSTANCE_MUTATION_LEGACY9 ||
            intent->mode == GC_INSTANCE_MUTATION_LEGACY10,
            NULL, NULL, published, diagnostic, sizeof(diagnostic));
        set_error(error, error_size, "%s", diagnostic);
    }
release:
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_policy_acquire(GcConfigStore *store, uint64_t generation)
{
    if (!store) return -1;
    pthread_mutex_lock(&store->mutation_lock);
    if (!store->instance_policy_valid || store->generation_exhausted ||
        atomic_load_explicit(&store->generation, memory_order_acquire) != generation) {
        pthread_mutex_unlock(&store->mutation_lock);
        return -1;
    }
    return 0;
}

void
gc_config_policy_release(GcConfigStore *store)
{
    if (store) pthread_mutex_unlock(&store->mutation_lock);
}

static int
save_setup_locked(GcConfigStore *store, const GcProfile *profile,
                  const GcControllerRule *rule,
                  char *error, size_t error_size)
{
    if (!store || !profile || !controller_rule_valid(rule)) {
        set_error(error, error_size, "invalid completed controller setup");
        return -1;
    }

    if (policy_check_held(store, error, error_size)) return -1;
    if (profile->owner_controller_id[0] || profile->owner_instance_id[0] || rule->instance_id[0]) {
        set_error(error, error_size, "profile_owned_mutation_forbidden"); return -1;
    }
    char profile_text[GC_PROFILE_TEXT_MAX];
    int profile_length =
        gc_profile_serialize(profile, profile_text, sizeof(profile_text));
    if (profile_length < 0) {
        set_error(error, error_size, "invalid completed setup profile");
        return -1;
    }

    char profile_relative[GC_PROFILE_ID_MAX + 16];
    char profile_path[GC_CONFIG_PATH_MAX];
    char controller_path[GC_CONFIG_PATH_MAX];
    char runtime_path[GC_CONFIG_PATH_MAX];
    int relative_length = snprintf(profile_relative,
                                   sizeof(profile_relative),
                                   "profiles/%s.ini", profile->id);
    if (relative_length < 0 ||
        (size_t)relative_length >= sizeof(profile_relative) ||
        join_path(profile_path, sizeof(profile_path), store->root,
                  profile_relative) != 0 ||
        join_path(controller_path, sizeof(controller_path), store->root,
                  "controllers.ini") != 0 ||
        join_path(runtime_path, sizeof(runtime_path), store->root,
                  "config.ini") != 0) {
        set_error(error, error_size, "completed setup path is too long");
        return -1;
    }

    int result = -1;
    char *old_profile = NULL, *old_controllers = NULL, *old_runtime = NULL;
    size_t old_profile_length = 0, old_controllers_length = 0;
    size_t old_runtime_length = 0;
    int profile_existed = 0, controllers_existed = 0, runtime_existed = 0;
    int profile_changed = 0, controllers_changed = 0, runtime_changed = 0;
    char failure[256] = "completed setup could not be saved";
    GcProfile *profiles = NULL;

    /* Revalidate profile identity and capacity inside the same mutation
     * transaction that publishes it. SETUP_BEGIN's checks are useful UX, but
     * cannot be the authority because another IPC client may save meanwhile. */
    profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles) {
        set_error(error, error_size, "out of memory checking profiles");
        goto done;
    }
    size_t profile_count = gc_config_profiles(
        store, profiles, GC_CONFIG_MAX_PROFILES, NULL);
    int replacing_profile = 0;
    for (size_t profile_index = 0; profile_index < profile_count;
         ++profile_index) {
        const GcProfile *candidate = &profiles[profile_index];
        if (strcmp(candidate->id, profile->id) == 0) {
            if (candidate->owner_controller_id[0] || candidate->owner_instance_id[0]) {
                set_error(error, error_size, "profile_owned_mutation_forbidden"); goto done;
            }
            replacing_profile = 1;
            if (!gc_profile_matches_exact_identity(
                    candidate, profile->match_vid, profile->match_pid,
                    profile->protocol)) {
                set_error(error, error_size,
                          "profile id already belongs to another controller");
                goto done;
            }
        } else if (gc_profile_matches_exact_identity(
                       candidate, profile->match_vid, profile->match_pid,
                       profile->protocol)) {
            set_error(error, error_size,
                      "controller already belongs to profile %s",
                      candidate->id);
            goto done;
        }
    }
    if (!replacing_profile && profile_count >= GC_CONFIG_MAX_PROFILES) {
        set_error(error, error_size, "profile limit reached");
        goto done;
    }

    /* Snapshot the latest runtime only after mutation_lock is held and change
     * the one setup-owned field. A caller snapshot here would clobber a
     * concurrent SETTINGS_SAVE with stale unrelated values. */
    GcRuntimeConfig runtime;
    if (gc_config_snapshot(store, &runtime, NULL) != 0) {
        set_error(error, error_size,
                  "runtime configuration is unavailable");
        goto done;
    }
    snprintf(runtime.active_profile, sizeof(runtime.active_profile), "auto");
    char runtime_text[2048];
    int runtime_length =
        serialize_runtime(&runtime, runtime_text, sizeof(runtime_text));
    if (runtime_length < 0) {
        set_error(error, error_size,
                  "runtime configuration could not be serialized");
        goto done;
    }

    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS];
    size_t count = gc_config_controllers(store, rules,
                                         GC_CONFIG_MAX_CONTROLLERS, NULL);
    size_t index;
    for (index = 0; index < count; ++index) {
        if (strcmp(rules[index].id, rule->id) == 0)
            break;
        if (!rules[index].instance_id[0] && rules[index].vid == rule->vid && rules[index].pid == rule->pid) {
            set_error(error, error_size,
                      "VID:PID already belongs to controller rule %s",
                      rules[index].id);
            goto done;
        }
    }
    if (index < count &&
        (rules[index].instance_id[0] || rules[index].vid != rule->vid || rules[index].pid != rule->pid)) {
        set_error(error, error_size,
                  "controller rule id already belongs to another VID:PID");
        goto done;
    }
    if (index == count) {
        if (count >= GC_CONFIG_MAX_CONTROLLERS) {
            set_error(error, error_size, "controller rule limit reached");
            goto done;
        }
        ++count;
    }
    rules[index] = *rule;

    char controller_text[GC_CONFIG_FILE_MAX];
    int controller_length = serialize_controllers(
        rules, count, controller_text, sizeof(controller_text), error,
        error_size);
    if (controller_length < 0)
        goto done;

    if (snapshot_optional_file(profile_path, &old_profile,
                               &old_profile_length, &profile_existed,
                               error, error_size) != 0 ||
        snapshot_optional_file(controller_path, &old_controllers,
                               &old_controllers_length,
                               &controllers_existed, error,
                               error_size) != 0 ||
        snapshot_optional_file(runtime_path, &old_runtime,
                               &old_runtime_length, &runtime_existed,
                               error, error_size) != 0)
        goto done;

    if (atomic_write(profile_path, profile_text, (size_t)profile_length,
                     error, error_size) != 0)
        goto rollback;
    profile_changed = 1;
    if (atomic_write(controller_path, controller_text,
                     (size_t)controller_length, error, error_size) != 0)
        goto rollback;
    controllers_changed = 1;
    if (atomic_write(runtime_path, runtime_text, (size_t)runtime_length,
                     error, error_size) != 0)
        goto rollback;
    runtime_changed = 1;
    if (reload_locked(store, error, error_size) != 0)
        goto rollback;

    result = 0;
    goto done;

rollback:
    if (error && error_size && *error)
        (void)snprintf(failure, sizeof(failure), "%s", error);
    {
        char rollback_error[256] = "";
        int rollback_failed = 0;
        if (runtime_changed &&
            restore_file(runtime_path, old_runtime, old_runtime_length,
                         runtime_existed, rollback_error,
                         sizeof(rollback_error)) != 0)
            rollback_failed = 1;
        if (controllers_changed &&
            restore_file(controller_path, old_controllers,
                         old_controllers_length, controllers_existed,
                         rollback_error, sizeof(rollback_error)) != 0)
            rollback_failed = 1;
        if (profile_changed &&
            restore_file(profile_path, old_profile, old_profile_length,
                         profile_existed, rollback_error,
                         sizeof(rollback_error)) != 0)
            rollback_failed = 1;
        if (rollback_failed) {
            policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
            set_error(error, error_size, "instance_policy_quarantined");
        }
        else
            set_error(error, error_size, "%s; no setup changes were kept",
                      failure);
    }

done:
    free(profiles);
    free(old_profile);
    free(old_controllers);
    free(old_runtime);
    return result;
}

int
gc_config_save_setup_for_controller_guarded(GcConfigStore *store,
    const GcProfile *draft, const GcControllerMatch *captured,
    const char *controller_id, const char *profile_id, GcControllerResolution *published,
    const GcConfigSetupGuard *guard, int *committed, char *error, size_t error_size)
{
    if (committed) *committed = 0;
    if (published) memset(published, 0, sizeof(*published));
    if (!store || !draft || !published || !committed || !guard || !guard->acquire || !guard->release ||
        !controller_match_valid(captured) || captured->identity.state != GC_INSTANCE_STABLE ||
        !controller_id || !profile_id || strlen(controller_id) >= GC_PROFILE_ID_MAX ||
        strlen(profile_id) >= GC_PROFILE_ID_MAX || !valid_profile_id(controller_id) || !valid_profile_id(profile_id) ||
        !strcmp(controller_id, "auto") || !strcmp(controller_id, "default") ||
        !strcmp(profile_id, "auto") || !strcmp(profile_id, "default") ||
        !memchr(draft->id, 0, sizeof(draft->id)) ||
        !memchr(draft->name, 0, sizeof(draft->name)) ||
        !memchr(draft->protocol, 0, sizeof(draft->protocol)) ||
        !memchr(draft->owner_controller_id, 0, sizeof(draft->owner_controller_id)) ||
        !memchr(draft->owner_instance_id, 0, sizeof(draft->owner_instance_id)) ||
        (draft->owner_controller_id[0] && strcmp(draft->owner_controller_id, controller_id)) ||
        (draft->owner_instance_id[0] && strcmp(draft->owner_instance_id, captured->identity.instance_id)) ||
        strcmp(draft->id, profile_id) || draft->match_any_vid || draft->match_any_pid ||
        draft->match_vid != captured->vid || draft->match_pid != captured->pid ||
        !instance_protocol_matches(draft->protocol, captured->protocol)) {
        set_error(error, error_size, "invalid_instance_request"); return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    int result = -1, held_guard = 0;
    char *old_profile = NULL, *old_controllers = NULL;
    size_t old_profile_length = 0, old_controllers_length = 0;
    int profile_existed = 0, controllers_existed = 0, profile_changed = 0, controllers_changed = 0;
    GcProfile *profiles = NULL;
    if (gc_config_instance_check_held(store, GC_INSTANCE_REQUIRE_LIVE, error, error_size)) goto done;
    if (guard->acquire(guard->context)) {
        set_error(error, error_size, "source_binding_mismatch"); goto done;
    }
    held_guard = 1;
    GcControllerRule rules[GC_CONFIG_MAX_CONTROLLERS];
    size_t count = store->controller_count, index, model_index = count;
    memcpy(rules, store->controllers, count * sizeof(rules[0]));
    for (index = 0; index < count; ++index) if (!strcmp(rules[index].id, controller_id)) break;
    for (size_t i = 0; i < count; ++i)
        if (!rules[i].instance_id[0] && rules[i].vid == captured->vid && rules[i].pid == captured->pid) model_index = i;
    GcControllerRule rule;
    if (index < count) {
        rule = rules[index];
        if (!rule.instance_id[0] || rule.vid != captured->vid || rule.pid != captured->pid ||
            strcmp(rule.instance_id, captured->identity.instance_id) ||
            (rule.profile_id[0] && strcmp(rule.profile_id, profile_id))) {
            set_error(error, error_size, "instance_retarget_forbidden"); goto done;
        }
    } else {
        if (count == GC_CONFIG_MAX_CONTROLLERS) { set_error(error, error_size, "controller_limit"); goto done; }
        if (model_index < count) rule = rules[model_index];
        else {
            memset(&rule, 0, sizeof(rule)); rule.enabled = 1; rule.preferred_interface_number = -1;
            rule.vid = captured->vid; rule.pid = captured->pid;
            snprintf(rule.protocol, sizeof(rule.protocol), "%s", captured->protocol);
            snprintf(rule.name, sizeof(rule.name), "%s", draft->name);
        }
        snprintf(rule.id, sizeof(rule.id), "%s", controller_id);
        snprintf(rule.instance_id, sizeof(rule.instance_id), "%s", captured->identity.instance_id);
        rule.profile_id[0] = 0; ++count;
    }
    if (!instance_protocol_matches(rule.protocol, captured->protocol) ||
        (rule.preferred_interface_number != -1 && rule.preferred_interface_number != captured->interface_number) ||
        (rule.preferred_in_endpoint && rule.preferred_in_endpoint != captured->in_endpoint) ||
        (rule.preferred_out_endpoint && rule.preferred_out_endpoint != captured->out_endpoint)) {
        set_error(error, error_size, "source_binding_mismatch"); goto done;
    }
    profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles) { set_error(error, error_size, "publication_failed"); goto done; }
    size_t profile_count = store->profile_count, profile_index;
    memcpy(profiles, store->profiles, profile_count * sizeof(profiles[0]));
    for (profile_index = 0; profile_index < profile_count; ++profile_index)
        if (!strcmp(profiles[profile_index].id, profile_id)) break;
    GcProfile profile = *draft;
    if (rule.profile_id[0]) {
        if (profile_index == profile_count ||
            strcmp(profiles[profile_index].owner_controller_id, rule.id) ||
            strcmp(profiles[profile_index].owner_instance_id, rule.instance_id)) {
            set_error(error, error_size, "profile_association_unavailable"); goto done;
        }
        const GcProfile *existing = &profiles[profile_index];
        if (existing->match_vid != captured->vid || existing->match_pid != captured->pid ||
            !instance_protocol_matches(existing->protocol, captured->protocol)) {
            set_error(error, error_size, "profile_association_unavailable"); goto done;
        }
        snprintf(profile.name, sizeof(profile.name), "%s", existing->name);
        snprintf(profile.protocol, sizeof(profile.protocol), "%s", existing->protocol);
    } else {
        if (profile_index != profile_count) { set_error(error, error_size, "profile_owner_conflict"); goto done; }
        if (profile_count == GC_CONFIG_MAX_PROFILES) { set_error(error, error_size, "profile_limit"); goto done; }
        ++profile_count;
    }
    profile.schema = GC_PROFILE_SCHEMA_INSTANCE;
    snprintf(profile.owner_controller_id, sizeof(profile.owner_controller_id), "%s", rule.id);
    snprintf(profile.owner_instance_id, sizeof(profile.owner_instance_id), "%s", rule.instance_id);
    snprintf(rule.profile_id, sizeof(rule.profile_id), "%s", profile_id);
    rules[index] = rule; profiles[profile_index] = profile;
    if (store_associations_valid(rules, count, profiles, profile_count, error, error_size)) goto done;
    char profile_text[GC_PROFILE_TEXT_MAX], controller_text[GC_CONFIG_FILE_MAX];
    int profile_length = gc_profile_serialize(&profile, profile_text, sizeof(profile_text));
    GcProfile parsed;
    if (profile_length < 0) {
        set_error(error, error_size, "invalid_instance_request"); goto done;
    }
    if (gc_profile_parse(profile_text, (size_t)profile_length, &parsed, error, error_size)) {
        set_error(error, error_size, "invalid_instance_request"); goto done;
    }
    int controller_length = serialize_controllers(rules, count, controller_text, sizeof(controller_text), error, error_size);
    if (controller_length < 0) {
        set_error(error, error_size, "invalid_instance_request"); goto done;
    }
    char relative[GC_PROFILE_ID_MAX + 16], profile_path[GC_CONFIG_PATH_MAX], controller_path[GC_CONFIG_PATH_MAX];
    snprintf(relative, sizeof(relative), "profiles/%s.ini", profile_id);
    if (join_path(profile_path, sizeof(profile_path), store->root, relative) ||
        join_path(controller_path, sizeof(controller_path), store->root, "controllers.ini")) {
        set_error(error, error_size, "invalid_instance_request"); goto done;
    }
    if (snapshot_optional_file(profile_path, &old_profile, &old_profile_length, &profile_existed, error, error_size) ||
        snapshot_optional_file(controller_path, &old_controllers, &old_controllers_length, &controllers_existed, error, error_size)) {
        set_error(error, error_size, "publication_failed"); goto done;
    }
    if (atomic_write(profile_path, profile_text, (size_t)profile_length, error, error_size)) {
        set_error(error, error_size, "publication_failed"); goto done;
    }
    profile_changed = 1;
    if (atomic_write(controller_path, controller_text, (size_t)controller_length, error, error_size)) goto rollback;
    controllers_changed = 1;
    if (gc_config_instance_check_held(store, GC_INSTANCE_REQUIRE_LIVE, error, error_size) ||
        reload_locked(store, error, error_size)) goto rollback;
    *committed = 1;
    GcControllerRule saved; uint64_t generation;
    if (controller_readback_locked(store, controller_id, &saved, &generation) ||
        !controller_rule_equal(&saved, &rule)) {
        set_error(error, error_size, "publication_readback_failed"); goto done;
    }
    pthread_mutex_lock(&store->lock);
    for (size_t i = 0; i < store->profile_count; ++i)
        if (!strcmp(store->profiles[i].id, profile_id) &&
            !memcmp(&store->profiles[i], &parsed, sizeof(parsed))) {
            published->rule = saved; published->profile = store->profiles[i];
            published->has_rule = published->has_profile = 1;
            published->generation = generation; published->kind = GC_RESOLVE_INSTANCE; result = 0; break;
        }
    pthread_mutex_unlock(&store->lock);
    if (result) { memset(published, 0, sizeof(*published)); set_error(error, error_size, "publication_readback_failed"); }
    else if (error && error_size) error[0] = 0;
    goto done;
rollback:
    {
        int failed = 0;
        if (controllers_changed && restore_file(controller_path, old_controllers, old_controllers_length, controllers_existed, NULL, 0)) failed = 1;
        if (profile_changed && restore_file(profile_path, old_profile, old_profile_length, profile_existed, NULL, 0)) failed = 1;
        if (failed) {
            policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
            set_error(error, error_size, "instance_policy_quarantined");
        } else set_error(error, error_size, "publication_failed");
    }
done:
    free(profiles); free(old_profile); free(old_controllers);
    if (held_guard) guard->release(guard->context);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_save_setup(GcConfigStore *store, const GcProfile *profile,
                     const GcControllerRule *rule, char *error, size_t error_size)
{
    if (!store) return -1;
    pthread_mutex_lock(&store->mutation_lock);
    int result = save_setup_locked(store, profile, rule, error, error_size);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_save_setup_preserving_rule_guarded(GcConfigStore *store,
    const GcProfile *profile, GcControllerRule *saved, uint64_t *generation,
    const GcConfigSetupGuard *guard, int *committed, char *error, size_t error_size)
{
    if (committed) *committed = 0;
    if (!store || !profile || profile->match_any_vid || profile->match_any_pid ||
        !profile->match_vid) {
        set_error(error, error_size, "setup requires an exact controller identity");
        return -1;
    }
    if (guard && (!guard->acquire || !guard->release)) {
        set_error(error, error_size, "setup guard requires acquire and release callbacks");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    if (policy_check_held(store, error, error_size)) {
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    if (guard && guard->acquire(guard->context) != 0) {
        set_error(error, error_size, "setup source connection changed or input unavailable");
        pthread_mutex_unlock(&store->mutation_lock);
        return -1;
    }
    GcControllerRule rule;
    if (gc_config_find_controller_including_disabled(store, profile->match_vid,
                                                     profile->match_pid, &rule, NULL)) {
        memset(&rule, 0, sizeof rule);
        snprintf(rule.id, sizeof rule.id, "%s", profile->id);
        snprintf(rule.name, sizeof rule.name, "%s", profile->name[0] ? profile->name : profile->id);
        snprintf(rule.protocol, sizeof rule.protocol, "%s", profile->protocol);
        rule.vid = profile->match_vid; rule.pid = profile->match_pid;
        rule.enabled = 1; rule.preferred_interface_number = -1;
    }
    int result = save_setup_locked(store, profile, &rule, error, error_size);
    if (result == 0) {
        if (committed) *committed = 1;
        result = controller_readback_locked(store, rule.id, saved, generation);
    }
    if (guard) guard->release(guard->context);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_save_setup_preserving_rule(GcConfigStore *store,
    const GcProfile *profile, GcControllerRule *saved, uint64_t *generation,
    char *error, size_t error_size)
{
    return gc_config_save_setup_preserving_rule_guarded(store, profile, saved,
        generation, NULL, NULL, error, error_size);
}

int
gc_config_create_profile(GcConfigStore *store, const char *id,
                         char *error, size_t error_size)
{
    if (!store || !valid_profile_id(id) || strlen(id) >= GC_PROFILE_ID_MAX) {
        set_error(error, error_size, "invalid profile id");
        return -1;
    }
    pthread_mutex_lock(&store->mutation_lock);
    if (policy_check_held(store, error, error_size)) {
        pthread_mutex_unlock(&store->mutation_lock); return -1;
    }
    GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles) {
        set_error(error, error_size, "out of memory checking profiles");
        pthread_mutex_unlock(&store->mutation_lock);
        return -1;
    }
    size_t count = gc_config_profiles(store, profiles,
                                      GC_CONFIG_MAX_PROFILES, NULL);
    if (count >= GC_CONFIG_MAX_PROFILES) {
        set_error(error, error_size, "profile limit reached");
        free(profiles);
        pthread_mutex_unlock(&store->mutation_lock);
        return -1;
    }
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(profiles[index].id, id) == 0) {
            set_error(error, error_size, "profile already exists");
            free(profiles);
            pthread_mutex_unlock(&store->mutation_lock);
            return -1;
        }
    }
    free(profiles);
    GcProfile profile;
    gc_profile_defaults(&profile);
    (void)snprintf(profile.id, sizeof(profile.id), "%s", id);
    (void)snprintf(profile.name, sizeof(profile.name), "%s", id);
    char text[GC_PROFILE_TEXT_MAX];
    int length = gc_profile_serialize(&profile, text, sizeof(text));
    char relative[GC_PROFILE_ID_MAX + 16];
    char path[GC_CONFIG_PATH_MAX];
    int relative_length = snprintf(relative, sizeof(relative),
                                   "profiles/%s.ini", profile.id);
    int result = -1;
    if (length < 0 || relative_length < 0 ||
        (size_t)relative_length >= sizeof(relative) ||
        join_path(path, sizeof(path), store->root, relative) != 0) {
        set_error(error, error_size, "profile path is too long");
    } else if (atomic_write(path, text, (size_t)length,
                            error, error_size) == 0) {
        result = reload_locked(store, error, error_size);
        if (result != 0) {
            int saved = errno;
            if (unlink(path) != 0) {
                policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
                set_error(error, error_size, "instance_policy_quarantined");
            }
            sync_parent_directory(path);
            errno = saved;
        }
    }
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_rename_profile(GcConfigStore *store, const char *old_id,
                         const char *new_id, char *error, size_t error_size)
{
    if (!store || !valid_profile_id(old_id) || !valid_profile_id(new_id) ||
        strlen(old_id) >= GC_PROFILE_ID_MAX ||
        strlen(new_id) >= GC_PROFILE_ID_MAX ||
        !strcmp(old_id, "default") || !strcasecmp(new_id, "auto")) {
        set_error(error, error_size, "invalid profile rename");
        return -1;
    }

    int result = -1;
    GcProfile *profiles = NULL;
    char *old_file = NULL, *new_file = NULL, *old_runtime = NULL;
    size_t old_length = 0, new_length = 0, runtime_length = 0;
    int old_existed = 0, new_existed = 0, runtime_existed = 0;
    char old_relative[GC_PROFILE_ID_MAX + 16];
    char new_relative[GC_PROFILE_ID_MAX + 16];
    char old_path[GC_CONFIG_PATH_MAX], new_path[GC_CONFIG_PATH_MAX];
    char runtime_path[GC_CONFIG_PATH_MAX];
    char failure[256] = "profile rename failed";

    pthread_mutex_lock(&store->mutation_lock);
    if (policy_check_held(store, error, error_size)) goto done;
    profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles) {
        set_error(error, error_size, "out of memory checking profiles");
        goto done;
    }
    const size_t count = gc_config_profiles(store, profiles,
                                             GC_CONFIG_MAX_PROFILES, NULL);
    GcProfile renamed;
    int found = 0;
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(old_id, new_id) && !strcmp(profiles[index].id, new_id)) {
            set_error(error, error_size, "profile already exists");
            goto done;
        }
        if (!strcmp(profiles[index].id, old_id)) {
            renamed = profiles[index];
            found = 1;
        }
    }
    if (!found) {
        set_error(error, error_size, "profile not found");
        goto done;
    }
    if (renamed.owner_controller_id[0] || renamed.owner_instance_id[0]) {
        set_error(error, error_size, "profile_owned_mutation_forbidden"); goto done;
    }
    if (!strcmp(old_id, new_id)) { result = 0; goto done; }
    if (!strcmp(renamed.name, old_id))
        snprintf(renamed.name, sizeof(renamed.name), "%s", new_id);
    snprintf(renamed.id, sizeof(renamed.id), "%s", new_id);

    char profile_text[GC_PROFILE_TEXT_MAX];
    const int profile_length = gc_profile_serialize(
        &renamed, profile_text, sizeof(profile_text));
    if (profile_length < 0 ||
        snprintf(old_relative, sizeof(old_relative), "profiles/%s.ini",
                 old_id) >= (int)sizeof(old_relative) ||
        snprintf(new_relative, sizeof(new_relative), "profiles/%s.ini",
                 new_id) >= (int)sizeof(new_relative) ||
        join_path(old_path, sizeof(old_path), store->root, old_relative) != 0 ||
        join_path(new_path, sizeof(new_path), store->root, new_relative) != 0 ||
        join_path(runtime_path, sizeof(runtime_path), store->root,
                  "config.ini") != 0) {
        set_error(error, error_size, "profile rename path is too long");
        goto done;
    }

    GcRuntimeConfig runtime;
    if (gc_config_snapshot(store, &runtime, NULL) != 0) {
        set_error(error, error_size, "runtime configuration is unavailable");
        goto done;
    }
    const int change_runtime = !strcmp(runtime.active_profile, old_id);
    char runtime_text[2048];
    int runtime_text_length = 0;
    if (change_runtime) {
        snprintf(runtime.active_profile, sizeof(runtime.active_profile), "%s",
                 new_id);
        runtime_text_length = serialize_runtime(
            &runtime, runtime_text, sizeof(runtime_text));
        if (runtime_text_length < 0) {
            set_error(error, error_size,
                      "runtime configuration could not be serialized");
            goto done;
        }
    }

    if (snapshot_optional_file(old_path, &old_file, &old_length, &old_existed,
                               error, error_size) != 0 || !old_existed ||
        snapshot_optional_file(new_path, &new_file, &new_length, &new_existed,
                               error, error_size) != 0 || new_existed ||
        (change_runtime && snapshot_optional_file(
             runtime_path, &old_runtime, &runtime_length, &runtime_existed,
             error, error_size) != 0)) {
        if (!error || !error_size || !*error)
            set_error(error, error_size, new_existed ? "profile already exists" :
                      "profile file is unavailable");
        goto done;
    }

    if (atomic_write(new_path, profile_text, (size_t)profile_length,
                     error, error_size) != 0)
        goto rollback;
    if (change_runtime && atomic_write(
            runtime_path, runtime_text, (size_t)runtime_text_length,
            error, error_size) != 0)
        goto rollback;
    if (unlink(old_path) != 0) {
        set_error(error, error_size, "cannot remove old profile: %s",
                  strerror(errno));
        goto rollback;
    }
    sync_parent_directory(old_path);
    if (reload_locked(store, error, error_size) != 0)
        goto rollback;
    result = 0;
    goto done;

rollback:
    if (error && error_size && *error)
        snprintf(failure, sizeof(failure), "%s", error);
    {
        char rollback_error[256] = "";
        int rollback_failed = 0;
        if (restore_file(old_path, old_file, old_length, old_existed,
                         rollback_error, sizeof(rollback_error)) != 0)
            rollback_failed = 1;
        if (restore_file(new_path, new_file, new_length, new_existed,
                         rollback_error, sizeof(rollback_error)) != 0)
            rollback_failed = 1;
        if (change_runtime && restore_file(
                runtime_path, old_runtime, runtime_length, runtime_existed,
                rollback_error, sizeof(rollback_error)) != 0)
            rollback_failed = 1;
        if (rollback_failed) {
            policy_invalidate_held(store, GC_INSTANCE_INVALIDATE_INCOMPLETE_INTERNAL_ROLLBACK);
            set_error(error, error_size, "instance_policy_quarantined");
        }
        else
            set_error(error, error_size, "%s; profile was not renamed",
                      failure);
    }

done:
    free(profiles);
    free(old_file);
    free(new_file);
    free(old_runtime);
    pthread_mutex_unlock(&store->mutation_lock);
    return result;
}

int
gc_config_export_profile(GcConfigStore *store, const char *id,
                         char *error, size_t error_size)
{
    if (!store || !valid_profile_id(id)) {
        set_error(error, error_size, "invalid profile id");
        return -1;
    }
    GcProfile *profiles = calloc(GC_CONFIG_MAX_PROFILES, sizeof(*profiles));
    if (!profiles) {
        set_error(error, error_size, "out of memory checking profiles");
        return -1;
    }
    size_t count = gc_config_profiles(store, profiles,
                                      GC_CONFIG_MAX_PROFILES, NULL);
    GcProfile profile;
    int found = 0;
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(profiles[index].id, id) == 0) {
            profile = profiles[index];
            found = 1;
            break;
        }
    }
    free(profiles);
    if (!found) {
        set_error(error, error_size, "profile not found");
        return -1;
    }
    char text[GC_PROFILE_TEXT_MAX];
    int length = gc_profile_serialize(&profile, text, sizeof(text));
    char relative[GC_PROFILE_ID_MAX + 24];
    char path[GC_CONFIG_PATH_MAX];
    int written = snprintf(relative, sizeof(relative), "exports/%s.ini", id);
    if (length < 0 || written < 0 ||
        (size_t)written >= sizeof(relative) ||
        join_path(path, sizeof(path), store->root, relative) != 0) {
        set_error(error, error_size, "profile export path is too long");
        return -1;
    }
    return atomic_write(path, text, (size_t)length, error, error_size);
}

int
gc_config_import_profile(GcConfigStore *store, const char *id,
                         char *error, size_t error_size)
{
    if (!store || !valid_profile_id(id)) {
        set_error(error, error_size, "invalid profile id");
        return -1;
    }
    char relative[GC_PROFILE_ID_MAX + 24];
    char path[GC_CONFIG_PATH_MAX];
    int written = snprintf(relative, sizeof(relative), "imports/%s.ini", id);
    if (written < 0 || (size_t)written >= sizeof(relative) ||
        join_path(path, sizeof(path), store->root, relative) != 0) {
        set_error(error, error_size, "profile import path is too long");
        return -1;
    }
    size_t length = 0;
    char *text = read_file(path, &length, error, error_size);
    if (!text)
        return -1;
    GcProfile profile;
    int result = gc_profile_parse(text, length, &profile, error, error_size);
    free(text);
    if (result != 0 || strcmp(profile.id, id) != 0) {
        if (result == 0)
            set_error(error, error_size,
                      "imported profile id must match filename");
        return -1;
    }
    if (profile.owner_controller_id[0] || profile.owner_instance_id[0]) {
        set_error(error, error_size, "profile_owned_mutation_forbidden"); return -1;
    }
    return gc_config_save_profile(store, &profile, error, error_size);
}

/* Collects the .ini basenames under <root>/<subdir> whose stem is a valid
 * profile id, sorted. A missing directory means nothing has been shared yet
 * and is not an error. Bounded by `capacity`; extras are dropped rather than
 * growing the reply without limit. */
static int
list_ini_basenames(const char *root, const char *subdir,
                   char names[][GC_PROFILE_ID_MAX], size_t capacity,
                   size_t *count)
{
    *count = 0;
    char directory[GC_CONFIG_PATH_MAX];
    if (join_path(directory, sizeof(directory), root, subdir) != 0)
        return -1;
    DIR *stream = opendir(directory);
    if (!stream)
        return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length <= 4 || strcasecmp(entry->d_name + length - 4, ".ini") != 0)
            continue;
        size_t base = length - 4;
        if (base == 0 || base >= GC_PROFILE_ID_MAX)
            continue;
        char id[GC_PROFILE_ID_MAX];
        memcpy(id, entry->d_name, base);
        id[base] = '\0';
        if (!valid_profile_id(id))
            continue;
        if (*count >= capacity)
            break;
        (void)snprintf(names[*count], GC_PROFILE_ID_MAX, "%s", id);
        ++*count;
    }
    (void)closedir(stream);
    qsort(names, *count, GC_PROFILE_ID_MAX, profile_name_compare);
    return 0;
}

static int
append_name_array(char **cursor, size_t *left,
                  char names[][GC_PROFILE_ID_MAX], size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        int written = snprintf(*cursor, *left, "%s\"%s\"",
                               index ? "," : "", names[index]);
        if (written < 0 || (size_t)written >= *left)
            return -1;
        *cursor += written;
        *left -= (size_t)written;
    }
    return 0;
}

int
gc_config_profile_files_json(GcConfigStore *store, char *response,
                             size_t response_size)
{
    if (!store || !response || response_size == 0)
        return -1;
    char imports[GC_CONFIG_MAX_PROFILES][GC_PROFILE_ID_MAX];
    char exports[GC_CONFIG_MAX_PROFILES][GC_PROFILE_ID_MAX];
    size_t import_count = 0;
    size_t export_count = 0;
    if (list_ini_basenames(store->root, "imports", imports,
                           GC_CONFIG_MAX_PROFILES, &import_count) != 0 ||
        list_ini_basenames(store->root, "exports", exports,
                           GC_CONFIG_MAX_PROFILES, &export_count) != 0)
        return -1;
    char *cursor = response;
    size_t left = response_size;
    int written = snprintf(cursor, left, "{\"ok\":true,\"imports\":[");
    if (written < 0 || (size_t)written >= left)
        return -1;
    cursor += written;
    left -= (size_t)written;
    if (append_name_array(&cursor, &left, imports, import_count) != 0)
        return -1;
    written = snprintf(cursor, left, "],\"exports\":[");
    if (written < 0 || (size_t)written >= left)
        return -1;
    cursor += written;
    left -= (size_t)written;
    if (append_name_array(&cursor, &left, exports, export_count) != 0)
        return -1;
    written = snprintf(cursor, left, "]}\n");
    if (written < 0 || (size_t)written >= left)
        return -1;
    return 0;
}

const char *
gc_config_root(const GcConfigStore *store)
{
    return store ? store->root : NULL;
}

uint64_t
gc_config_generation(const GcConfigStore *store)
{
    return store ? atomic_load_explicit(&store->generation,
                                        memory_order_acquire)
                 : 0;
}

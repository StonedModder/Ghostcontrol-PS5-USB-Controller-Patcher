#include "gc_runtime_commands.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>

static size_t gc_bounded_length(const char *s, size_t maximum)
{
    size_t n = 0;
    while (n < maximum && s[n]) ++n;
    return n;
}

static int parse_ipc_bool(const char *value, int *output)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *output = 1;
        return 0;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *output = 0;
        return 0;
    }
    return -1;
}

int gc_runtime_command_get(GcConfigStore *store, const GcGhostpadStatus *snapshot, char *response, size_t response_size)
{
    GcRuntimeConfig value;
    GcGhostpadStatus ghostpad = {0};
    uint64_t generation;
    if (gc_config_snapshot(store, &value, &generation) != 0) return -1;
    if (snapshot) ghostpad = *snapshot;
    struct json_object *o = json_object_new_object();
    if (!o) return -1;
#define ADD(k, v) json_object_object_add(o, k, v)
    ADD("ok", json_object_new_boolean(1));
    ADD("generation", json_object_new_int64((int64_t)generation));
    ADD("autostart", json_object_new_boolean(value.autostart));
    ADD("run_in_background", json_object_new_boolean(value.run_in_background));
    ADD("daemon_mode", json_object_new_boolean(value.daemon_mode));
    ADD("auto_restart", json_object_new_boolean(value.auto_restart));
    ADD("update_enabled", json_object_new_boolean(value.update_enabled));
    ADD("include_prereleases", json_object_new_boolean(value.include_prereleases));
    ADD("xmb_enabled", json_object_new_boolean(value.xmb_enabled));
    ADD("wireless_ds4", json_object_new_boolean(value.wireless_ds4));
    ADD("ghostpad_enabled", json_object_new_boolean(value.ghostpad_enabled));
    ADD("dualsense_fx_enabled", json_object_new_boolean(value.dualsense_fx_enabled));
    ADD("health_interval_ms", json_object_new_int64(value.health_interval_ms));
    ADD("disconnect_restart_ms", json_object_new_int64(value.disconnect_restart_ms));
    ADD("input_stale_release_ms", json_object_new_int64(value.input_stale_release_ms));
    ADD("ipc_port", json_object_new_int64(value.ipc_port));
    ADD("ghostpad_port", json_object_new_int64(value.ghostpad_port));
    ADD("ghostpad_stale_ms", json_object_new_int64(value.ghostpad_stale_ms));
    ADD("dualsense_fx_effect", json_object_new_int64(value.dualsense_fx_effect));
    ADD("dualsense_fx_period_ms", json_object_new_int64(value.dualsense_fx_period_ms));
    ADD("dualsense_fx_step_ms", json_object_new_int64(value.dualsense_fx_step_ms));
    ADD("dualsense_fx_r", json_object_new_int64(value.dualsense_fx_r));
    ADD("dualsense_fx_g", json_object_new_int64(value.dualsense_fx_g));
    ADD("dualsense_fx_b", json_object_new_int64(value.dualsense_fx_b));
    ADD("active_profile", json_object_new_string(value.active_profile));
    ADD("homeui_tile_owner", json_object_new_string(value.homeui_tile_owner));
    ADD("settings_entry_owner", json_object_new_string(value.settings_entry_owner));
    ADD("ghostpad_listening", json_object_new_boolean(ghostpad.listening));
    ADD("ghostpad_connected", json_object_new_boolean(ghostpad.connected));
    ADD("ghostpad_packets", json_object_new_int64(ghostpad.packets));
    ADD("ghostpad_coalesced_packets", json_object_new_int64(ghostpad.coalesced_packets));
    ADD("ghostpad_rejected_packets", json_object_new_int64(ghostpad.rejected_packets));
    ADD("ghostpad_stale_releases", json_object_new_int64(ghostpad.stale_releases));
    ADD("ghostpad_last_error", json_object_new_int64(ghostpad.last_error));
    ADD("ghostpad_client", json_object_new_string_len(ghostpad.client, (int)gc_bounded_length(ghostpad.client, sizeof(ghostpad.client))));
    ADD("autostart_effect", json_object_new_string("startup_preference"));
    ADD("run_in_background_effect", json_object_new_string("persisted_only"));
    ADD("daemon_mode_effect", json_object_new_string("persisted_only"));
    ADD("effect_route", json_object_new_string("unavailable"));
    struct json_object *caps = json_object_new_object();
    json_object_object_add(caps, "effects", json_object_new_boolean(0));
    json_object_object_add(caps, "wireless_ds4", json_object_new_boolean(0));
    json_object_object_add(caps, "ghostpad_output", json_object_new_boolean(0));
    json_object_object_add(caps, "xmb", json_object_new_boolean(0));
    json_object_object_add(caps, "background_lifecycle", json_object_new_boolean(0));
    ADD("capabilities", caps);
#undef ADD
    const char *text = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
    int written = response ? snprintf(response, response_size, "%s\n", text) : -1;
    json_object_put(o);
    return written >= 0 && (size_t)written < response_size ? 0 : -1;
}

static int parse_ipc_uint(const char *value, unsigned minimum,
                          unsigned maximum, unsigned *output)
{
    if (!value || !*value) return -1;
    for (const char *p = value; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !*value || *end || parsed < minimum || parsed > maximum)
        return -1;
    *output = (unsigned)parsed;
    return 0;
}

int gc_runtime_command_set(GcConfigStore *store, const char *argument, char *response,
                           size_t response_size)
{
    if (!response || response_size == 0) return -1;
    if (!argument || response_size < 512) goto rejected;
    const char *separator = strchr(argument, '=');
    if (!separator || separator == argument || !separator[1])
        goto rejected;
    char key[64];
    size_t key_length = (size_t)(separator - argument);
    if (key_length >= sizeof(key))
        goto rejected;
    memcpy(key, argument, key_length);
    key[key_length] = '\0';
    const char *value = separator + 1;
    GcRuntimeConfig runtime;
    if (gc_config_snapshot(store, &runtime, NULL) != 0)
        goto rejected;
    int result = -1;
    unsigned number = 0;
#define SET_BOOL(field) result = parse_ipc_bool(value, &runtime.field)
    if (strcmp(key, "autostart") == 0) SET_BOOL(autostart);
    else if (strcmp(key, "run_in_background") == 0) SET_BOOL(run_in_background);
    else if (strcmp(key, "daemon_mode") == 0) SET_BOOL(daemon_mode);
    else if (strcmp(key, "auto_restart") == 0) SET_BOOL(auto_restart);
    else if (strcmp(key, "update_enabled") == 0) SET_BOOL(update_enabled);
    else if (strcmp(key, "include_prereleases") == 0) SET_BOOL(include_prereleases);
    else if (strcmp(key, "xmb_enabled") == 0) SET_BOOL(xmb_enabled);
    else if (strcmp(key, "wireless_ds4") == 0) SET_BOOL(wireless_ds4);
    else if (strcmp(key, "ghostpad_enabled") == 0) SET_BOOL(ghostpad_enabled);
    else if (strcmp(key, "ghostpad_port") == 0 &&
             parse_ipc_uint(value, 1024, 65535, &number) == 0) {
        runtime.ghostpad_port = (uint16_t)number; result = 0;
    } else if (strcmp(key, "ghostpad_stale_ms") == 0 &&
               parse_ipc_uint(value, 50, 5000, &number) == 0) {
        runtime.ghostpad_stale_ms = number; result = 0;
    }
    else if (strcmp(key, "dualsense_fx_enabled") == 0) SET_BOOL(dualsense_fx_enabled);
    else if (strcmp(key, "dualsense_fx_effect") == 0 &&
             parse_ipc_uint(value, 0, 4, &number) == 0) {
        runtime.dualsense_fx_effect = (int)number; result = 0;
    } else if (strcmp(key, "dualsense_fx_period_ms") == 0 &&
               parse_ipc_uint(value, 100, 60000, &number) == 0) {
        runtime.dualsense_fx_period_ms = number; result = 0;
    } else if (strcmp(key, "dualsense_fx_step_ms") == 0 &&
               parse_ipc_uint(value, 0, 1000, &number) == 0) {
        runtime.dualsense_fx_step_ms = number; result = 0;
    } else if (strcmp(key, "dualsense_fx_r") == 0 &&
               parse_ipc_uint(value, 0, 255, &number) == 0) {
        runtime.dualsense_fx_r = number; result = 0;
    } else if (strcmp(key, "dualsense_fx_g") == 0 &&
               parse_ipc_uint(value, 0, 255, &number) == 0) {
        runtime.dualsense_fx_g = number; result = 0;
    } else if (strcmp(key, "dualsense_fx_b") == 0 &&
               parse_ipc_uint(value, 0, 255, &number) == 0) {
        runtime.dualsense_fx_b = number; result = 0;
    } else if (strcmp(key, "health_interval_ms") == 0 &&
             parse_ipc_uint(value, 250, 60000, &number) == 0) {
        runtime.health_interval_ms = number; result = 0;
    } else if (strcmp(key, "disconnect_restart_ms") == 0 &&
               parse_ipc_uint(value, 0, 600000, &number) == 0) {
        runtime.disconnect_restart_ms = number; result = 0;
    } else if (strcmp(key, "input_stale_release_ms") == 0 &&
               parse_ipc_uint(value, 0, 5000, &number) == 0 &&
               (number == 0 || number >= 50)) {
        runtime.input_stale_release_ms = number; result = 0;
    } else if (strcmp(key, "homeui_tile_owner") == 0 ||
               strcmp(key, "settings_entry_owner") == 0) {
        if (strcmp(value, "auto") == 0 ||
            strcmp(value, "ghostcontrol") == 0 ||
            strcmp(value, "onionhen") == 0) {
            char *field = strcmp(key, "homeui_tile_owner") == 0
                              ? runtime.homeui_tile_owner
                              : runtime.settings_entry_owner;
            snprintf(field, sizeof(runtime.homeui_tile_owner), "%s",
                     value);
            result = 0;
        }
    } else if (strcmp(key, "active_profile") == 0 &&
               strlen(value) < sizeof(runtime.active_profile)) {
        result = 0;
        for (const char *p = value; *p; ++p)
            if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                   *p == '.'))
                result = -1;
        if (result == 0)
            snprintf(runtime.active_profile, sizeof(runtime.active_profile),
                     "%s", value);
    }
#undef SET_BOOL
    if (result == 0) {
        char error[256] = "";
        result = gc_config_save_runtime(store, &runtime, error, sizeof(error));
        if (result == 0) {
            const char *effect = "configuration_pending";
            /* Only consumers with a normal configuration contract are
             * labelled supported. This is not an execution acknowledgement. */
            int supported = 1;
            int restart_required = 0;
            if (!strcmp(key, "autostart")) {
                effect = "startup_preference";
                restart_required = 1;
            } else if (!strcmp(key, "run_in_background") || !strcmp(key, "daemon_mode")) {
                effect = "persisted_only";
                supported = 0;
            } else if (!strncmp(key, "dualsense_fx_", 13) ||
                       !strcmp(key, "wireless_ds4") || !strncmp(key, "ghostpad_", 9) ||
                       !strcmp(key, "xmb_enabled") ||
                       !strcmp(key, "homeui_tile_owner") || !strcmp(key, "settings_entry_owner")) {
                effect = "unsupported_runtime";
                supported = 0;
            }
            snprintf(response, response_size,
                     "{\"ok\":true,\"key\":\"%s\",\"generation\":%llu,"
                     "\"saved\":true,\"applied\":false,\"pending\":true,"
                     "\"runtime_supported\":%s,\"restart_required\":%s,\"effect\":\"%s\"}\n",
                     key, (unsigned long long)gc_config_generation(store),
                     supported ? "true" : "false", restart_required ? "true" : "false", effect);
            return 0;
        }
    }
rejected:
    snprintf(response, response_size,
             "{\"ok\":false,\"error\":\"runtime_value_rejected\"}\n");
    return -1;
}

int gc_runtime_command_fx_status(const GcDualSenseFxStatus *snapshot, char *response, size_t response_size)
{
    if (!response || !response_size) return -1;
    GcDualSenseFxStatus value;
    memset(&value, 0, sizeof(value));
    if (snapshot) value = *snapshot;
    int written = snprintf(response, response_size,
                    "{\"ok\":true,\"configured_only\":true,\"runtime_supported\":false,\"route\":\"unavailable\",\"applied\":false,\"running\":%s,\"enabled\":%s,"
                    "\"effect\":%d,\"period_ms\":%u,\"step_ms\":%u,"
                    "\"r\":%u,\"g\":%u,\"b\":%u,"
                    "\"current_r\":%u,\"current_g\":%u,\"current_b\":%u,"
                    "\"controllers\":%u,\"writes\":%llu,\"errors\":%llu,"
                    "\"resets\":%llu,\"last_error\":%d,"
                    "\"rumble_active\":%s,\"rumble_large\":%u,"
                    "\"rumble_small\":%u,\"rumble_on_ms\":%u,"
                    "\"rumble_off_ms\":%u,\"rumble_repeats\":%u,"
                    "\"motion_valid\":%s,"
                    "\"quat\":[%.5f,%.5f,%.5f,%.5f],"
                    "\"gravity\":[%.5f,%.5f,%.5f],"
                    "\"gyro\":[%.5f,%.5f,%.5f]}\n",
                    value.running ? "true" : "false",
                    value.enabled ? "true" : "false", value.effect,
                    value.period_ms, value.step_ms, value.r, value.g, value.b,
                    value.current_r, value.current_g, value.current_b,
                    value.controllers, (unsigned long long)value.writes,
                    (unsigned long long)value.errors,
                    (unsigned long long)value.resets, value.last_error,
                    value.rumble_active ? "true" : "false",
                    value.rumble_large, value.rumble_small,
                    value.rumble_on_ms, value.rumble_off_ms,
                    value.rumble_repeats,
                    value.motion_valid ? "true" : "false",
                    value.quat[0], value.quat[1], value.quat[2], value.quat[3],
                    value.gravity[0], value.gravity[1], value.gravity[2],
                    value.gyro[0], value.gyro[1], value.gyro[2]);
    return written >= 0 && (size_t)written < response_size ? 0 : -1;
}

int gc_runtime_command_rumble(const char *argument, GcRuntimeRumbleSink sink, char *response,
                                size_t response_size)
{
    char text[96];
    if (!response || response_size == 0) return -1;
    if (!argument || response_size < 512 || strlen(argument) >= sizeof(text)) goto rejected;
    snprintf(text, sizeof(text), "%s", argument);
    char *fields[5], *cursor = text;
    for (size_t i = 0; i < 5; ++i) {
        fields[i] = cursor;
        char *separator = strchr(cursor, '|');
        if (i < 4) {
            if (!separator) goto rejected;
            *separator = '\0';
            cursor = separator + 1;
        } else if (separator) goto rejected;
    }
    unsigned large, small, on_ms, off_ms, repeats;
    if (parse_ipc_uint(fields[0], 0, 255, &large) ||
        parse_ipc_uint(fields[1], 0, 255, &small) ||
        parse_ipc_uint(fields[2], 1, 60000, &on_ms) ||
        parse_ipc_uint(fields[3], 0, 60000, &off_ms) ||
        parse_ipc_uint(fields[4], 0, 1000, &repeats))
        goto rejected;
    if (!sink) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"applied\":false,\"runtime_supported\":false,\"error\":\"effect_route_unavailable\"}\n");
        return -1;
    }
    if (sink(large, small, on_ms, off_ms, repeats) != 0) {
        snprintf(response, response_size,
                 "{\"ok\":false,\"applied\":false,\"error\":\"effect_route_failed\"}\n");
        return -1;
    }
    return snprintf(response, response_size,
                    "{\"ok\":true,\"command\":\"DUALSENSE_RUMBLE\",\"accepted\":true,\"pending\":true,\"applied\":false}\n") < 0
        ? -1 : 0;
rejected:
    snprintf(response, response_size,
             "{\"ok\":false,\"error\":\"invalid_rumble_settings\"}\n");
    return -1;
}


/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_profile.h"

#include "gc_ini.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const uint32_t k_button_masks[GC_BUTTON_COUNT] = {
    SCE_PAD_BUTTON_SHARE, SCE_PAD_BUTTON_L3, SCE_PAD_BUTTON_R3,
    SCE_PAD_BUTTON_OPTIONS, SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_RIGHT,
    SCE_PAD_BUTTON_DOWN, SCE_PAD_BUTTON_LEFT, SCE_PAD_BUTTON_L2,
    SCE_PAD_BUTTON_R2, SCE_PAD_BUTTON_L1, SCE_PAD_BUTTON_R1,
    SCE_PAD_BUTTON_TRIANGLE, SCE_PAD_BUTTON_CIRCLE, SCE_PAD_BUTTON_CROSS,
    SCE_PAD_BUTTON_SQUARE, SCE_PAD_BUTTON_PS, SCE_PAD_BUTTON_TOUCH_PAD,
    0x00020000u, 0x00040000u, 0x00080000u, 0x00200000u,
};

static const char *const k_button_names[GC_BUTTON_COUNT] = {
    "SHARE", "L3", "R3", "OPTIONS", "UP", "RIGHT", "DOWN", "LEFT",
    "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE",
    "PS", "TOUCHPAD",
    "INPUT15", "INPUT16", "INPUT17", "INPUT18",
};

static const char *const k_axis_names[GC_AXIS_COUNT] = {
    "LX", "LY", "RX", "RY", "L2", "R2",
};

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

const char *
gc_button_name(GcButton button)
{
    return button >= 0 && button < GC_BUTTON_COUNT
               ? k_button_names[button]
               : "UNKNOWN";
}

uint32_t gc_button_mask(GcButton button)
{
    return button >= 0 && button < GC_BUTTON_COUNT ? k_button_masks[button] : 0;
}

int
gc_button_from_name(const char *name, GcButton *button)
{
    if (!name || !button)
        return -1;
    for (int index = 0; index < GC_BUTTON_COUNT; ++index) {
        if (strcasecmp(name, k_button_names[index]) == 0) {
            *button = (GcButton)index;
            return 0;
        }
    }
    if (strcasecmp(name, "CREATE") == 0) {
        *button = GC_BUTTON_SHARE;
        return 0;
    }
    if (strcasecmp(name, "TOUCH_PAD") == 0) {
        *button = GC_BUTTON_TOUCHPAD;
        return 0;
    }
    return -1;
}

const char *
gc_axis_name(GcAxis axis)
{
    return axis >= 0 && axis < GC_AXIS_COUNT ? k_axis_names[axis] : "UNKNOWN";
}

int
gc_axis_from_name(const char *name, GcAxis *axis)
{
    if (!name || !axis)
        return -1;
    for (int index = 0; index < GC_AXIS_COUNT; ++index) {
        if (strcasecmp(name, k_axis_names[index]) == 0) {
            *axis = (GcAxis)index;
            return 0;
        }
    }
    return -1;
}

void
gc_profile_defaults(GcProfile *profile)
{
    if (!profile)
        return;
    memset(profile, 0, sizeof(*profile));
    profile->schema = GC_PROFILE_SCHEMA;
    (void)snprintf(profile->id, sizeof(profile->id), "default");
    (void)snprintf(profile->name, sizeof(profile->name), "Default identity");
    profile->enabled = 1;
    profile->match_any_vid = 1;
    profile->match_any_pid = 1;
    (void)snprintf(profile->protocol, sizeof(profile->protocol), "any");
    for (int index = 0; index < GC_BUTTON_COUNT; ++index)
        profile->button_targets[index] = index < GC_BUTTON_INPUT15 ? k_button_masks[index] : 0;
    profile->left_deadzone = 0.0f;
    profile->right_deadzone = 0.0f;
    profile->left_sensitivity = 1.0f;
    profile->right_sensitivity = 1.0f;
    profile->left_trigger_threshold = 0;
    profile->right_trigger_threshold = 0;
}

static int
parse_bool(const char *text, int *value)
{
    if (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0 ||
        strcasecmp(text, "yes") == 0 || strcasecmp(text, "on") == 0) {
        *value = 1;
        return 0;
    }
    if (strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0 ||
        strcasecmp(text, "no") == 0 || strcasecmp(text, "off") == 0) {
        *value = 0;
        return 0;
    }
    return -1;
}

static int
parse_float_range(const char *text, float minimum, float maximum, float *value)
{
    char *end = NULL;
    errno = 0;
    float parsed = strtof(text, &end);
    if (errno || end == text || *end || !__builtin_isfinite(parsed) ||
        parsed < minimum || parsed > maximum)
        return -1;
    *value = parsed;
    return 0;
}

static int
parse_u8(const char *text, uint8_t *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 0);
    if (errno || end == text || *end || parsed < 0 || parsed > 255)
        return -1;
    *value = (uint8_t)parsed;
    return 0;
}

static int
parse_match_id(const char *text, int *is_any, uint16_t *value)
{
    if (strcasecmp(text, "any") == 0 || strcmp(text, "*") == 0) {
        *is_any = 1;
        *value = 0;
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno || end == text || *end || parsed > 0xffffu)
        return -1;
    *is_any = 0;
    *value = (uint16_t)parsed;
    return 0;
}

static int
valid_hid_source(unsigned report, unsigned offset, unsigned bits,
                 unsigned page, unsigned usage, long long minimum,
                 long long maximum, GcHidSource *source)
{
    if (report > 255 || offset > 8191 || bits < 1 || bits > 32 ||
        page > 0xffff || usage > 0xffff || minimum < INT32_MIN ||
        maximum > INT32_MAX || minimum >= maximum)
        return -1;
    *source = (GcHidSource){
        .offset = (uint16_t)offset,
        .page = (uint16_t)page,
        .usage = (uint16_t)usage,
        .bits = (uint8_t)bits,
        .report = (uint8_t)report,
        .minimum = (int32_t)minimum,
        .maximum = (int32_t)maximum,
    };
    return 0;
}

static int
parse_hid_button_binding(const char *text, GcHidButtonBinding *binding)
{
    unsigned report, offset, bits, page, usage;
    long long minimum, maximum, active;
    char trailing;
    if (sscanf(text, "%u,%u,%u,%u,%u,%lld,%lld,%lld%c", &report,
               &offset, &bits, &page, &usage, &minimum, &maximum, &active,
               &trailing) != 8 || active < INT32_MIN || active > INT32_MAX ||
        active < minimum || active > maximum ||
        valid_hid_source(report, offset, bits, page, usage, minimum, maximum,
                         &binding->source) != 0)
        return -1;
    binding->active_value = (int32_t)active;
    binding->present = 1;
    return 0;
}

static int
parse_hid_axis_binding(const char *text, GcHidAxisBinding *binding)
{
    unsigned report, offset, bits, page, usage;
    long long minimum, maximum, center;
    int polarity;
    char trailing;
    if (sscanf(text, "%u,%u,%u,%u,%u,%lld,%lld,%lld,%d%c", &report,
               &offset, &bits, &page, &usage, &minimum, &maximum, &center,
               &polarity, &trailing) != 9 || center < minimum ||
        center > maximum || (polarity != -1 && polarity != 1) ||
        valid_hid_source(report, offset, bits, page, usage, minimum, maximum,
                         &binding->source) != 0)
        return -1;
    binding->center = (int32_t)center;
    binding->polarity = polarity;
    binding->present = 1;
    return 0;
}

static int
parse_axis_binding(const char *text, GcAxisBinding *binding)
{
    char source_name[16], trailing;
    long long center, minimum, maximum;
    int polarity;
    GcAxis source;
    if (sscanf(text, "%15[^,],%lld,%lld,%lld,%d%c", source_name, &center,
               &minimum, &maximum, &polarity, &trailing) != 5 ||
        gc_axis_from_name(source_name, &source) != 0 ||
        minimum < INT32_MIN || maximum > INT32_MAX || minimum >= maximum ||
        center < minimum || center > maximum ||
        (polarity != -1 && polarity != 1))
        return -1;
    *binding = (GcAxisBinding){
        .source = source,
        .center = (int32_t)center,
        .minimum = (int32_t)minimum,
        .maximum = (int32_t)maximum,
        .polarity = polarity,
        .present = 1,
    };
    return 0;
}

static int
valid_id(const char *id)
{
    if (!id || !*id)
        return 0;
    for (const unsigned char *cursor = (const unsigned char *)id; *cursor;
         ++cursor) {
        if (!((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= '0' && *cursor <= '9')) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.')
            return 0;
    }
    return strcmp(id, ".") != 0 && strcmp(id, "..") != 0;
}

static int
parse_button_expression(const char *text, uint32_t *mask)
{
    if (strcasecmp(text, "NONE") == 0) {
        *mask = 0;
        return 0;
    }
    char copy[256];
    if (strlen(text) >= sizeof(copy))
        return -1;
    (void)snprintf(copy, sizeof(copy), "%s", text);
    uint32_t result = 0;
    char *cursor = copy;
    while (cursor && *cursor) {
        char *plus = strchr(cursor, '+');
        if (plus)
            *plus = '\0';
        while (isspace((unsigned char)*cursor))
            ++cursor;
        char *end = cursor + strlen(cursor);
        while (end > cursor && isspace((unsigned char)end[-1]))
            *--end = '\0';
        GcButton button;
        if (!*cursor || gc_button_from_name(cursor, &button) != 0 || button >= GC_BUTTON_INPUT15)
            return -1;
        result |= k_button_masks[button];
        cursor = plus ? plus + 1 : NULL;
    }
    *mask = result;
    return 0;
}

static int
profile_entry(void *context, const char *section, const char *key,
              const char *value, char *error, size_t error_size)
{
    GcProfile *profile = context;
    if (*section && strcasecmp(section, "profile") != 0) {
        set_error(error, error_size, "unsupported profile section '%s'",
                  section);
        return -1;
    }
    if (strcasecmp(key, "schema") == 0) {
        char *end = NULL;
        long schema = strtol(value, &end, 10);
        if (end == value || *end ||
            (schema != GC_PROFILE_SCHEMA_LEGACY &&
             schema != GC_PROFILE_SCHEMA_V2 &&
             schema != GC_PROFILE_SCHEMA &&
             schema != GC_PROFILE_SCHEMA_INSTANCE)) {
            set_error(error, error_size, "unsupported profile schema '%s'",
                      value);
            return -1;
        }
        profile->schema = (int)schema;
        return 0;
    }
    if (strcasecmp(key, "id") == 0) {
        if (!valid_id(value) || strlen(value) >= sizeof(profile->id)) {
            set_error(error, error_size, "invalid profile id");
            return -1;
        }
        (void)snprintf(profile->id, sizeof(profile->id), "%s", value);
        return 0;
    }
    if (strcasecmp(key, "owner_controller_id") == 0) {
        if (!valid_id(value) || strlen(value) >= sizeof(profile->owner_controller_id) ||
            !strcmp(value, "auto") || !strcmp(value, "default")) goto invalid_value;
        snprintf(profile->owner_controller_id, sizeof(profile->owner_controller_id), "%s", value);
        return 0;
    }
    if (strcasecmp(key, "owner_instance_id") == 0) {
        if (strlen(value) >= sizeof(profile->owner_instance_id)) goto invalid_value;
        snprintf(profile->owner_instance_id, sizeof(profile->owner_instance_id), "%s", value);
        return 0;
    }
    if (strcasecmp(key, "name") == 0) {
        if (!*value || strlen(value) >= sizeof(profile->name)) {
            set_error(error, error_size, "invalid profile name");
            return -1;
        }
        (void)snprintf(profile->name, sizeof(profile->name), "%s", value);
        return 0;
    }
    if (strcasecmp(key, "enabled") == 0) {
        if (parse_bool(value, &profile->enabled) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "match.vid") == 0) {
        if (parse_match_id(value, &profile->match_any_vid,
                           &profile->match_vid) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "match.pid") == 0) {
        if (parse_match_id(value, &profile->match_any_pid,
                           &profile->match_pid) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "match.protocol") == 0) {
        if (!*value || strlen(value) >= sizeof(profile->protocol))
            goto invalid_value;
        (void)snprintf(profile->protocol, sizeof(profile->protocol), "%s",
                       value);
        return 0;
    }
    if (strncasecmp(key, "map.", 4) == 0) {
        GcButton source_button;
        uint32_t target;
        if (gc_button_from_name(key + 4, &source_button) != 0 ||
            parse_button_expression(value, &target) != 0)
            goto invalid_value;
        profile->button_targets[source_button] = target;
        return 0;
    }
    if (strncasecmp(key, "pattern.", 8) == 0) {
        GcButton target_button;
        uint32_t source_mask;
        if (gc_button_from_name(key + 8, &target_button) != 0 ||
            target_button >= GC_BUTTON_INPUT15 ||
            parse_button_expression(value, &source_mask) != 0 ||
            source_mask == 0 || !(source_mask & (source_mask - 1)))
            goto invalid_value;
        profile->button_patterns[target_button].source_mask = source_mask;
        profile->button_patterns[target_button].present = 1;
        return 0;
    }
    if (strncasecmp(key, "hid.button.", 11) == 0) {
        GcButton target;
        if (gc_button_from_name(key + 11, &target) != 0 ||
            target >= GC_BUTTON_INPUT15 ||
            parse_hid_button_binding(value,
                                     &profile->hid_buttons[target]) != 0)
            goto invalid_value;
        return 0;
    }
    if (strncasecmp(key, "hid.axis.", 9) == 0) {
        GcAxis target;
        if (gc_axis_from_name(key + 9, &target) != 0 ||
            parse_hid_axis_binding(value, &profile->hid_axes[target]) != 0)
            goto invalid_value;
        return 0;
    }
    if (strncasecmp(key, "axis.", 5) == 0) {
        GcAxis target;
        if (gc_axis_from_name(key + 5, &target) != 0 ||
            parse_axis_binding(value, &profile->axes[target]) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "left.deadzone") == 0) {
        if (parse_float_range(value, 0.0f, 0.95f,
                              &profile->left_deadzone) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "right.deadzone") == 0) {
        if (parse_float_range(value, 0.0f, 0.95f,
                              &profile->right_deadzone) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "left.sensitivity") == 0) {
        if (parse_float_range(value, 0.10f, 3.0f,
                              &profile->left_sensitivity) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "right.sensitivity") == 0) {
        if (parse_float_range(value, 0.10f, 3.0f,
                              &profile->right_sensitivity) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "left.invert_x") == 0) {
        if (parse_bool(value, &profile->invert_left_x) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "left.invert_y") == 0) {
        if (parse_bool(value, &profile->invert_left_y) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "right.invert_x") == 0) {
        if (parse_bool(value, &profile->invert_right_x) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "right.invert_y") == 0) {
        if (parse_bool(value, &profile->invert_right_y) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "left.trigger_threshold") == 0) {
        if (parse_u8(value, &profile->left_trigger_threshold) != 0)
            goto invalid_value;
        return 0;
    }
    if (strcasecmp(key, "right.trigger_threshold") == 0) {
        if (parse_u8(value, &profile->right_trigger_threshold) != 0)
            goto invalid_value;
        return 0;
    }
    set_error(error, error_size, "unknown profile key '%s'", key);
    return -1;

invalid_value:
    set_error(error, error_size, "invalid value for profile key '%s'", key);
    return -1;
}

int
gc_profile_apply_key(GcProfile *profile, const char *key, const char *value,
                     char *error, size_t error_size)
{
    if (!profile || !key || !value || !*key) {
        set_error(error, error_size, "profile, key and value are required");
        return -1;
    }
    /* Identity is fixed at creation: a field edit must never move a saved
     * profile onto another controller's identity or downgrade its schema, both
     * of which would silently invalidate the persisted file contract. */
    if (strcasecmp(key, "owner_controller_id") == 0 ||
        strcasecmp(key, "owner_instance_id") == 0 ||
        ((profile->owner_controller_id[0] || profile->owner_instance_id[0]) &&
         (strcasecmp(key, "id") == 0 || strcasecmp(key, "schema") == 0 ||
          strcasecmp(key, "name") == 0 || strncasecmp(key, "match.", 6) == 0))) {
        set_error(error, error_size, "profile_owned_mutation_forbidden");
        return -1;
    }
    if (strcasecmp(key, "id") == 0 || strcasecmp(key, "schema") == 0) {
        set_error(error, error_size, "profile key '%s' cannot be edited", key);
        return -1;
    }
    /* Empty section: any key applies against [profile], exactly as it would
     * when reloading the file, so save and reload stay a fixed point. */
    return profile_entry(profile, "", key, value, error, error_size);
}

static int
edit_id_valid(const char *id)
{
    if (!id || !*id || strlen(id) >= GC_PROFILE_ID_MAX)
        return 0;
    if (strcmp(id, "auto") == 0 || strcmp(id, ".") == 0 ||
        strcmp(id, "..") == 0)
        return 0;
    for (const char *cursor = id; *cursor; ++cursor) {
        char c = *cursor;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

static int
edit_key_valid(const char *key)
{
    if (!key || !*key || strlen(key) > GC_PROFILE_EDIT_KEY_MAX)
        return 0;
    if (strcasecmp(key, "id") == 0 || strcasecmp(key, "schema") == 0)
        return 0;
    for (const char *cursor = key; *cursor; ++cursor) {
        char c = *cursor;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

int
gc_profile_edit_split(const char *argument, char *id, size_t id_size,
                      char *key, size_t key_size, char *value,
                      size_t value_size, char *error, size_t error_size)
{
    if (!argument || !id || !key || !value) {
        set_error(error, error_size, "profile edit parts are required");
        return -1;
    }
    const char *first = strchr(argument, '|');
    const char *second = first ? strchr(first + 1, '|') : NULL;
    if (!first || !second) {
        set_error(error, error_size, "profile edit needs id|key|value");
        return -1;
    }
    size_t id_length = (size_t)(first - argument);
    size_t key_length = (size_t)(second - first - 1);
    const char *value_start = second + 1;
    size_t value_length = strlen(value_start);
    if (id_length == 0 || id_length >= id_size ||
        key_length == 0 || key_length >= key_size ||
        value_length >= value_size) {
        set_error(error, error_size, "profile edit field is too long");
        return -1;
    }
    if (value_length > GC_PROFILE_EDIT_VALUE_MAX) {
        set_error(error, error_size, "profile edit value is too long");
        return -1;
    }
    memcpy(id, argument, id_length);
    id[id_length] = '\0';
    memcpy(key, first + 1, key_length);
    key[key_length] = '\0';
    memcpy(value, value_start, value_length);
    value[value_length] = '\0';
    for (size_t index = 0; index < value_length; ++index)
        if (value[index] == '\r' || value[index] == '\n') {
            set_error(error, error_size,
                      "profile edit value has a control character");
            return -1;
        }
    if (!edit_id_valid(id)) {
        set_error(error, error_size, "invalid profile id");
        return -1;
    }
    if (!edit_key_valid(key)) {
        set_error(error, error_size, "invalid profile key");
        return -1;
    }
    return 0;
}

static int
profile_owner_valid(const GcProfile *p)
{
    if (!memchr(p->owner_controller_id, 0, sizeof(p->owner_controller_id)) ||
        !memchr(p->owner_instance_id, 0, sizeof(p->owner_instance_id))) return 0;
    if (!p->owner_controller_id[0] && !p->owner_instance_id[0]) return 1;
    if (!valid_id(p->owner_controller_id) || !strcmp(p->owner_controller_id, "auto") ||
        !strcmp(p->owner_controller_id, "default") ||
        p->match_any_vid || p->match_any_pid || !p->match_vid ||
        strlen(p->owner_instance_id) != 23 ||
        memcmp(p->owner_instance_id, "bt-classic:", 11)) return 0;
    int nonzero = 0, nonff = 0;
    for (size_t i = 11; i < 23; ++i) {
        char c = p->owner_instance_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
        nonzero |= c != '0'; nonff |= c != 'F';
    }
    static const char *const protocols[] = {
        "nintendo", "xbox_gip", "xusb", "ds3", "ds4", "logitech", "hid", "dinput"
    };
    int protocol_ok = 0;
    for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i)
        protocol_ok |= !strcmp(p->protocol, protocols[i]);
    return nonzero && nonff && protocol_ok;
}

typedef struct {
    GcProfile profile;
    char keys[128][64];
    size_t count;
    unsigned owner_keys;
} ProfileParseContext;

static int
profile_unique_entry(void *context, const char *section, const char *key,
                     const char *value, char *error, size_t error_size)
{
    ProfileParseContext *p = context;
    char canonical[64];
    if (strlen(key) >= sizeof(canonical)) goto invalid;
    snprintf(canonical, sizeof(canonical), "%s", key);
    /* Aliases such as map.CREATE and map.SHARE address the same field. */
    static const char *const button_prefixes[] = {"map.", "pattern.", "hid.button."};
    for (size_t i = 0; i < sizeof(button_prefixes) / sizeof(button_prefixes[0]); ++i) {
        size_t n = strlen(button_prefixes[i]); GcButton b;
        if (!strncasecmp(key, button_prefixes[i], n) &&
            !gc_button_from_name(key + n, &b))
            snprintf(canonical, sizeof(canonical), "%s%s", button_prefixes[i], gc_button_name(b));
    }
    for (size_t i = 0; i < p->count; ++i)
        if (!strcasecmp(canonical, p->keys[i])) goto invalid;
    if (p->count >= sizeof(p->keys) / sizeof(p->keys[0])) goto invalid;
    if (profile_entry(&p->profile, section, key, value, error, error_size)) return -1;
    snprintf(p->keys[p->count++], sizeof(p->keys[0]), "%s", canonical);
    if (!strcasecmp(key, "owner_controller_id")) p->owner_keys |= 1;
    if (!strcasecmp(key, "owner_instance_id")) p->owner_keys |= 2;
    return 0;
invalid:
    set_error(error, error_size, "duplicate or oversized profile key");
    return -1;
}

int
gc_profile_parse(const char *text, size_t length, GcProfile *profile,
                 char *error, size_t error_size)
{
    if (!profile) {
        set_error(error, error_size, "profile output is required");
        return -1;
    }
    if (!text || length >= GC_PROFILE_TEXT_MAX || memchr(text, 0, length)) {
        set_error(error, error_size, "invalid or oversized profile text");
        return -1;
    }
    ProfileParseContext context = {0};
    gc_profile_defaults(&context.profile);
    if (gc_ini_parse(text, length, profile_unique_entry, &context, error, error_size))
        return -1;
    GcProfile parsed = context.profile;
    if ((parsed.schema == GC_PROFILE_SCHEMA_INSTANCE &&
         (context.owner_keys != 3 || !parsed.owner_controller_id[0] ||
          !parsed.owner_instance_id[0])) ||
        (parsed.schema != GC_PROFILE_SCHEMA_INSTANCE && context.owner_keys) ||
        !profile_owner_valid(&parsed)) {
        set_error(error, error_size, "profile_owner_conflict");
        return -1;
    }
    if (!valid_id(parsed.id)) {
        set_error(error, error_size, "profile id is required");
        return -1;
    }
    if (parsed.schema == GC_PROFILE_SCHEMA_LEGACY) {
        for (int index = 0; index < GC_BUTTON_INPUT15; ++index)
            if (parsed.hid_buttons[index].present) {
                set_error(error, error_size,
                          "physical HID bindings require profile schema 2");
                return -1;
            }
        for (int index = 0; index < GC_AXIS_COUNT; ++index)
            if (parsed.hid_axes[index].present) {
                set_error(error, error_size,
                          "physical HID bindings require profile schema 2");
                return -1;
            }
    }
    if (parsed.schema < GC_PROFILE_SCHEMA) {
        for (int index = 0; index < GC_BUTTON_INPUT15; ++index)
            if (parsed.button_patterns[index].present) {
                set_error(error, error_size,
                          "multi-bit button patterns require profile schema 3");
                return -1;
            }
    }
    *profile = parsed;
    return 0;
}

static int
append_text(char *output, size_t output_size, size_t *offset,
            const char *format, ...)
{
    if (*offset >= output_size)
        return -1;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(output + *offset, output_size - *offset, format,
                            args);
    va_end(args);
    if (written < 0 || (size_t)written >= output_size - *offset)
        return -1;
    *offset += (size_t)written;
    return 0;
}

static int
append_button_expression(char *output, size_t output_size, size_t *offset,
                         uint32_t mask)
{
    if (mask == 0)
        return append_text(output, output_size, offset, "NONE");
    int count = 0;
    for (int index = 0; index < GC_BUTTON_COUNT; ++index) {
        if ((mask & k_button_masks[index]) == 0)
            continue;
        if (append_text(output, output_size, offset, "%s%s",
                        count++ ? "+" : "", k_button_names[index]) != 0)
            return -1;
    }
    return count ? 0 : -1;
}

int
gc_profile_serialize(const GcProfile *profile, char *output,
                     size_t output_size)
{
    if (!profile || !output || output_size == 0 ||
        !memchr(profile->id, 0, sizeof(profile->id)) ||
        !memchr(profile->name, 0, sizeof(profile->name)) ||
        !memchr(profile->protocol, 0, sizeof(profile->protocol)) ||
        !profile_owner_valid(profile) || !valid_id(profile->id) ||
        (profile->enabled != 0 && profile->enabled != 1) ||
        (profile->match_any_vid != 0 && profile->match_any_vid != 1) ||
        (profile->match_any_pid != 0 && profile->match_any_pid != 1) ||
        profile->schema < GC_PROFILE_SCHEMA_LEGACY || profile->schema > GC_PROFILE_SCHEMA_INSTANCE ||
        (profile->schema == GC_PROFILE_SCHEMA_INSTANCE &&
         (!profile->owner_controller_id[0] || !profile->owner_instance_id[0])) ||
        ((profile->owner_controller_id[0] || profile->owner_instance_id[0]) &&
         profile->schema != GC_PROFILE_SCHEMA_INSTANCE))
        return -1;
    for (const unsigned char *c = (const unsigned char *)profile->name; *c; ++c)
        if (*c < 0x20 || *c == 0x7f) return -1;
    for (const unsigned char *c = (const unsigned char *)profile->protocol; *c; ++c)
        if (*c < 0x20 || *c == 0x7f) return -1;
    size_t offset = 0;
    if (append_text(output, output_size, &offset,
                    "[profile]\n"
                    "schema=%d\n"
                    "id=%s\n"
                    "name=\"%s\"\n"
                    "enabled=%s\n"
                    "match.vid=%s",
                    (profile->owner_controller_id[0] || profile->owner_instance_id[0])
                        ? GC_PROFILE_SCHEMA_INSTANCE : GC_PROFILE_SCHEMA,
                    profile->id, profile->name,
                    profile->enabled ? "true" : "false",
                    profile->match_any_vid ? "any" : "") != 0)
        return -1;
    if (!profile->match_any_vid &&
        append_text(output, output_size, &offset, "0x%04x",
                    profile->match_vid) != 0)
        return -1;
    if (append_text(output, output_size, &offset, "\nmatch.pid=%s",
                    profile->match_any_pid ? "any" : "") != 0)
        return -1;
    if (!profile->match_any_pid &&
        append_text(output, output_size, &offset, "0x%04x",
                    profile->match_pid) != 0)
        return -1;
    if (append_text(output, output_size, &offset,
                    "\nmatch.protocol=\"%s\"\n", profile->protocol) != 0)
        return -1;
    if (profile->owner_controller_id[0] &&
        append_text(output, output_size, &offset,
            "owner_controller_id=%s\nowner_instance_id=%s\n",
            profile->owner_controller_id, profile->owner_instance_id) != 0) return -1;
    for (int index = 0; index < GC_BUTTON_COUNT; ++index) {
        if (append_text(output, output_size, &offset, "map.%s=",
                        k_button_names[index]) != 0 ||
            append_button_expression(output, output_size, &offset,
                                     profile->button_targets[index]) != 0 ||
            append_text(output, output_size, &offset, "\n") != 0)
            return -1;
    }
    for (int index = 0; index < GC_BUTTON_INPUT15; ++index) {
        const GcButtonPatternBinding *binding = &profile->button_patterns[index];
        if (!binding->present)
            continue;
        if (append_text(output, output_size, &offset, "pattern.%s=",
                        k_button_names[index]) != 0 ||
            append_button_expression(output, output_size, &offset,
                                     binding->source_mask) != 0 ||
            append_text(output, output_size, &offset, "\n") != 0)
            return -1;
    }
    for (int index = 0; index < GC_BUTTON_INPUT15; ++index) {
        const GcHidButtonBinding *binding = &profile->hid_buttons[index];
        const GcHidSource *source = &binding->source;
        if (binding->present &&
            append_text(output, output_size, &offset,
                        "hid.button.%s=%u,%u,%u,%u,%u,%d,%d,%d\n",
                        k_button_names[index], (unsigned)source->report,
                        (unsigned)source->offset, (unsigned)source->bits,
                        (unsigned)source->page, (unsigned)source->usage,
                        source->minimum, source->maximum,
                        binding->active_value) != 0)
            return -1;
    }
    for (int index = 0; index < GC_AXIS_COUNT; ++index) {
        const GcAxisBinding *binding = &profile->axes[index];
        if (binding->present &&
            append_text(output, output_size, &offset,
                        "axis.%s=%s,%d,%d,%d,%d\n", k_axis_names[index],
                        gc_axis_name(binding->source), binding->center,
                        binding->minimum, binding->maximum,
                        binding->polarity) != 0)
            return -1;
    }
    for (int index = 0; index < GC_AXIS_COUNT; ++index) {
        const GcHidAxisBinding *binding = &profile->hid_axes[index];
        const GcHidSource *source = &binding->source;
        if (binding->present &&
            append_text(output, output_size, &offset,
                        "hid.axis.%s=%u,%u,%u,%u,%u,%d,%d,%d,%d\n",
                        k_axis_names[index], (unsigned)source->report,
                        (unsigned)source->offset, (unsigned)source->bits,
                        (unsigned)source->page, (unsigned)source->usage,
                        source->minimum, source->maximum, binding->center,
                        binding->polarity) != 0)
            return -1;
    }
    if (append_text(output, output_size, &offset,
                    "left.deadzone=%.3f\n"
                    "right.deadzone=%.3f\n"
                    "left.sensitivity=%.3f\n"
                    "right.sensitivity=%.3f\n"
                    "left.invert_x=%s\n"
                    "left.invert_y=%s\n"
                    "right.invert_x=%s\n"
                    "right.invert_y=%s\n"
                    "left.trigger_threshold=%u\n"
                    "right.trigger_threshold=%u\n",
                    profile->left_deadzone, profile->right_deadzone,
                    profile->left_sensitivity, profile->right_sensitivity,
                    profile->invert_left_x ? "true" : "false",
                    profile->invert_left_y ? "true" : "false",
                    profile->invert_right_x ? "true" : "false",
                    profile->invert_right_y ? "true" : "false",
                    (unsigned)profile->left_trigger_threshold,
                    (unsigned)profile->right_trigger_threshold) != 0)
        return -1;
    return (int)offset;
}

static int
protocol_matches(const char *configured, const char *detected)
{
    if (!configured || !detected)
        return 0;
    if (strcasecmp(configured, detected) == 0)
        return 1;
    /* DirectInput is the Windows-facing name for ordinary USB HID gamepad
     * mode. Keep old "hid" profiles compatible with the explicit dinput
     * name exposed by the controller toolbox. */
    return (strcasecmp(configured, "hid") == 0 &&
            strcasecmp(detected, "dinput") == 0) ||
           (strcasecmp(configured, "dinput") == 0 &&
            strcasecmp(detected, "hid") == 0);
}

int
gc_profile_matches_exact_identity(const GcProfile *profile, uint16_t vid,
                                  uint16_t pid, const char *protocol)
{
    return profile && protocol && !profile->owner_controller_id[0] &&
           !profile->owner_instance_id[0] && !profile->match_any_vid &&
           !profile->match_any_pid && profile->match_vid == vid &&
           profile->match_pid == pid &&
           strcasecmp(profile->protocol, "any") != 0 &&
           protocol_matches(profile->protocol, protocol);
}

int
gc_profile_match_score(const GcProfile *profile, uint16_t vid,
                       uint16_t pid, const char *protocol)
{
    if (!profile || !profile->enabled || profile->owner_controller_id[0] ||
        profile->owner_instance_id[0] ||
        (!profile->match_any_vid && profile->match_vid != vid) ||
        (!profile->match_any_pid && profile->match_pid != pid))
        return -1;
    int protocol_specific = strcasecmp(profile->protocol, "any") != 0;
    if (protocol_specific && !protocol_matches(profile->protocol, protocol))
        return -1;
    return (!profile->match_any_vid ? 4 : 0) +
           (!profile->match_any_pid ? 2 : 0) + protocol_specific;
}

static uint8_t
scale_trigger(uint8_t value, uint8_t threshold)
{
    if (value < threshold)
        return 0;
    if (threshold == 255)
        return value == 255 ? 255 : 0;
    unsigned scaled =
        ((unsigned)(value - threshold) * 255u) / (255u - threshold);
    if (scaled == 0 && value != 0)
        scaled = 1;
    return (uint8_t)(scaled > 255u ? 255u : scaled);
}

static float
axis_to_unit(uint8_t value)
{
    int centered = (int)value - 128;
    return centered >= 0 ? centered / 127.0f : centered / 128.0f;
}

static uint8_t
unit_to_axis(float value)
{
    if (value > 1.0f)
        value = 1.0f;
    else if (value < -1.0f)
        value = -1.0f;
    float raw = value >= 0 ? 128.0f + value * 127.0f
                           : 128.0f + value * 128.0f;
    int rounded = (int)(raw + 0.5f);
    return (uint8_t)(rounded < 0 ? 0 : rounded > 255 ? 255 : rounded);
}

static uint8_t
bound_axis_value(const GcAxisBinding *binding, GcAxis target, int32_t value)
{
    int64_t directed = (int64_t)binding->polarity *
                       ((int64_t)value - binding->center);
    int64_t positive = binding->polarity > 0
                           ? (int64_t)binding->maximum - binding->center
                           : (int64_t)binding->center - binding->minimum;
    if (target == GC_AXIS_L2 || target == GC_AXIS_R2) {
        if (directed <= 0 || positive <= 0)
            return 0;
        if (directed >= positive)
            return 255;
        return (uint8_t)((directed * 255 + positive / 2) / positive);
    }
    if (directed >= 0) {
        if (positive <= 0)
            return 128;
        if (directed >= positive)
            return 255;
        return (uint8_t)(128 + (directed * 127 + positive / 2) / positive);
    }
    int64_t negative = binding->polarity > 0
                           ? (int64_t)binding->center - binding->minimum
                           : (int64_t)binding->maximum - binding->center;
    directed = -directed;
    if (negative <= 0)
        return 128;
    if (directed >= negative)
        return 0;
    return (uint8_t)(128 - (directed * 128 + negative / 2) / negative);
}

static void
apply_axis_bindings(const GcProfile *profile, ScePadData *pad)
{
    const uint8_t source[GC_AXIS_COUNT] = {
        pad->leftStick.x, pad->leftStick.y,
        pad->rightStick.x, pad->rightStick.y,
        pad->analogButtons.l2, pad->analogButtons.r2,
    };
    uint8_t output[GC_AXIS_COUNT];
    memcpy(output, source, sizeof(output));
    for (int target = 0; target < GC_AXIS_COUNT; ++target) {
        const GcAxisBinding *binding = &profile->axes[target];
        if (binding->present && binding->source >= 0 &&
            binding->source < GC_AXIS_COUNT)
            output[target] = bound_axis_value(binding, (GcAxis)target,
                                               source[binding->source]);
    }
    pad->leftStick.x = output[GC_AXIS_LX];
    pad->leftStick.y = output[GC_AXIS_LY];
    pad->rightStick.x = output[GC_AXIS_RX];
    pad->rightStick.y = output[GC_AXIS_RY];
    pad->analogButtons.l2 = output[GC_AXIS_L2];
    pad->analogButtons.r2 = output[GC_AXIS_R2];
}

static void
apply_stick(uint8_t *x_value, uint8_t *y_value, float deadzone,
            float sensitivity, int invert_x, int invert_y)
{
    float x = axis_to_unit(*x_value);
    float y = axis_to_unit(*y_value);
    if (invert_x)
        x = -x;
    if (invert_y)
        y = -y;
    float magnitude = sqrtf(x * x + y * y);
    if (magnitude <= deadzone || magnitude == 0.0f) {
        *x_value = 128;
        *y_value = 128;
        return;
    }
    float output_magnitude =
        ((magnitude - deadzone) / (1.0f - deadzone)) * sensitivity;
    if (output_magnitude > 1.0f)
        output_magnitude = 1.0f;
    float scale = output_magnitude / magnitude;
    *x_value = unit_to_axis(x * scale);
    *y_value = unit_to_axis(y * scale);
}

void
gc_profile_apply(const GcProfile *profile, ScePadData *pad)
{
    if (!profile || !profile->enabled || !pad)
        return;

    apply_axis_bindings(profile, pad);
    uint32_t input = pad->buttons;
    pad->analogButtons.l2 = scale_trigger(pad->analogButtons.l2,
                                          profile->left_trigger_threshold);
    pad->analogButtons.r2 = scale_trigger(pad->analogButtons.r2,
                                          profile->right_trigger_threshold);
    input &= ~(SCE_PAD_BUTTON_L2 | SCE_PAD_BUTTON_R2);
    if (pad->analogButtons.l2 ||
        (profile->left_trigger_threshold == 0 &&
         (pad->buttons & SCE_PAD_BUTTON_L2)))
        input |= SCE_PAD_BUTTON_L2;
    if (pad->analogButtons.r2 ||
        (profile->right_trigger_threshold == 0 &&
         (pad->buttons & SCE_PAD_BUTTON_R2)))
        input |= SCE_PAD_BUTTON_R2;

    uint32_t known = 0;
    uint32_t output = 0;
    uint32_t consumed = 0;
    for (int index = 0; index < GC_BUTTON_COUNT; ++index)
        known |= k_button_masks[index];

    /* Some third-party XUSB pads report one physical control as a stable
     * combination of semantic bits. Apply the most-specific learned patterns
     * first and consume those source bits so their ordinary identity mappings
     * cannot leak through as extra outputs. */
    for (int width = GC_BUTTON_INPUT15; width >= 2; --width) {
        for (int target = 0; target < GC_BUTTON_INPUT15; ++target) {
            const GcButtonPatternBinding *binding =
                &profile->button_patterns[target];
            if (!binding->present || __builtin_popcount(binding->source_mask) != width ||
                (binding->source_mask & consumed) ||
                (input & binding->source_mask) != binding->source_mask)
                continue;
            output |= k_button_masks[target];
            consumed |= binding->source_mask;
        }
    }
    for (int index = 0; index < GC_BUTTON_COUNT; ++index) {
        const uint32_t source = k_button_masks[index];
        if ((input & source) && !(consumed & source))
            output |= profile->button_targets[index];
    }
    pad->buttons = output | (input & ~known);

    apply_stick(&pad->leftStick.x, &pad->leftStick.y,
                profile->left_deadzone, profile->left_sensitivity,
                profile->invert_left_x, profile->invert_left_y);
    apply_stick(&pad->rightStick.x, &pad->rightStick.y,
                profile->right_deadzone, profile->right_sensitivity,
                profile->invert_right_x, profile->invert_right_y);
}

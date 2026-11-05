/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_ini.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GC_INI_MAX_LINE 1024u
#define GC_INI_MAX_SECTION 96u

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

static char *
trim(char *text)
{
    while (*text && isspace((unsigned char)*text))
        ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

int
gc_ini_parse(const char *text, size_t length, GcIniEntryFn on_entry,
             void *context, char *error, size_t error_size)
{
    if (error && error_size)
        error[0] = '\0';
    if (!text || !on_entry) {
        set_error(error, error_size, "INI input and callback are required");
        return -1;
    }

    char section[GC_INI_MAX_SECTION] = "";
    size_t offset = 0;
    unsigned line_number = 0;
    while (offset < length) {
        char line[GC_INI_MAX_LINE];
        size_t count = 0;
        ++line_number;
        while (offset < length && text[offset] != '\n') {
            if (count + 1 >= sizeof(line)) {
                set_error(error, error_size, "INI line %u is too long",
                          line_number);
                return -1;
            }
            if (text[offset] != '\r')
                line[count++] = text[offset];
            ++offset;
        }
        if (offset < length && text[offset] == '\n')
            ++offset;
        line[count] = '\0';

        char *cursor = trim(line);
        if (!*cursor || *cursor == '#' || *cursor == ';')
            continue;

        if (*cursor == '[') {
            char *close = strchr(cursor + 1, ']');
            if (!close || trim(close + 1)[0] != '\0') {
                set_error(error, error_size, "invalid section on INI line %u",
                          line_number);
                return -1;
            }
            *close = '\0';
            char *name = trim(cursor + 1);
            size_t name_length = strlen(name);
            if (name_length == 0 || name_length >= sizeof(section)) {
                set_error(error, error_size,
                          "invalid section length on INI line %u", line_number);
                return -1;
            }
            memcpy(section, name, name_length + 1);
            continue;
        }

        char *equals = strchr(cursor, '=');
        if (!equals) {
            set_error(error, error_size, "missing '=' on INI line %u",
                      line_number);
            return -1;
        }
        *equals = '\0';
        char *key = trim(cursor);
        char *value = trim(equals + 1);
        if (!*key) {
            set_error(error, error_size, "empty key on INI line %u",
                      line_number);
            return -1;
        }
        size_t value_length = strlen(value);
        if (value_length >= 2 &&
            ((value[0] == '"' && value[value_length - 1] == '"') ||
             (value[0] == '\'' && value[value_length - 1] == '\''))) {
            value[value_length - 1] = '\0';
            ++value;
        }
        if (on_entry(context, section, key, value, error, error_size) != 0) {
            if (error && error_size && !error[0])
                set_error(error, error_size, "rejected INI line %u", line_number);
            return -1;
        }
    }
    return 0;
}


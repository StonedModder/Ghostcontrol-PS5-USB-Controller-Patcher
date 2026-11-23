/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_update_policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GC_UPDATE_URL_PREFIX \
    "https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher/releases/download/"

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    if (!error || !error_size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

/* `prerelease` receives the identifiers after '-' (up to any '+build'), or
 * NULL for a final release. */
static int parse_version(const char *value, unsigned output[3],
                         const char **prerelease)
{
    if (!value || !output || !prerelease)
        return -1;
    if (*value == 'v' || *value == 'V')
        ++value;
    for (int part = 0; part < 3; ++part) {
        if (*value < '0' || *value > '9')
            return -1;
        unsigned number = 0;
        while (*value >= '0' && *value <= '9') {
            if (number > 100000u)
                return -1;
            number = number * 10u + (unsigned)(*value++ - '0');
        }
        output[part] = number;
        if (part < 2 && *value++ != '.')
            return -1;
    }
    if (*value == '-' && value[1] && value[1] != '+') {
        *prerelease = value + 1;
        return 0;
    }
    *prerelease = NULL;
    return *value == '\0' || *value == '+' ? 0 : -1;
}

/* Length of the dot-separated identifier at `text`, stopping at '.', '+' or
 * the end. */
static size_t identifier_length(const char *text)
{
    size_t length = 0;
    while (text[length] && text[length] != '.' && text[length] != '+')
        ++length;
    return length;
}

static int identifier_is_numeric(const char *text, size_t length)
{
    if (!length)
        return 0;
    for (size_t i = 0; i < length; ++i)
        if (text[i] < '0' || text[i] > '9')
            return 0;
    return 1;
}

/* SemVer 2.0.0 section 11.4: identifiers compare left to right; numeric ones
 * by value, the rest in ASCII order, numeric below alphanumeric, and a shorter
 * list ranks lower when it is a prefix of the other. */
static int prerelease_compare(const char *left, const char *right)
{
    for (;;) {
        const int left_end = !*left || *left == '+';
        const int right_end = !*right || *right == '+';
        if (left_end || right_end)
            return left_end == right_end ? 0 : left_end ? -1 : 1;
        const size_t left_length = identifier_length(left);
        const size_t right_length = identifier_length(right);
        const int left_numeric = identifier_is_numeric(left, left_length);
        const int right_numeric = identifier_is_numeric(right, right_length);
        int order;
        if (left_numeric && right_numeric) {
            size_t a = 0, b = 0;
            while (a + 1 < left_length && left[a] == '0')
                ++a;
            while (b + 1 < right_length && right[b] == '0')
                ++b;
            if (left_length - a != right_length - b)
                order = left_length - a < right_length - b ? -1 : 1;
            else
                order = strncmp(left + a, right + b, left_length - a);
        } else if (left_numeric != right_numeric) {
            order = left_numeric ? -1 : 1;
        } else {
            const size_t shorter = left_length < right_length ? left_length : right_length;
            order = strncmp(left, right, shorter);
            if (!order && left_length != right_length)
                order = left_length < right_length ? -1 : 1;
        }
        if (order)
            return order < 0 ? -1 : 1;
        left += left_length;
        right += right_length;
        if (*left == '.')
            ++left;
        if (*right == '.')
            ++right;
    }
}

int gc_update_version_compare(const char *left, const char *right,
                              int *comparison)
{
    unsigned left_parts[3], right_parts[3];
    const char *left_pre, *right_pre;
    if (!comparison || parse_version(left, left_parts, &left_pre) != 0 ||
        parse_version(right, right_parts, &right_pre) != 0)
        return -1;
    for (size_t i = 0; i < 3; ++i) {
        if (left_parts[i] != right_parts[i]) {
            *comparison = left_parts[i] > right_parts[i] ? 1 : -1;
            return 0;
        }
    }
    /* Same x.y.z: a final release outranks a prerelease of that version, and
     * two prereleases are ordered by their identifiers (beta < beta.2 < rc). */
    if (!left_pre || !right_pre)
        *comparison = (right_pre != NULL) - (left_pre != NULL);
    else
        *comparison = prerelease_compare(left_pre, right_pre);
    return 0;
}

int gc_update_is_app_zip(const char *name)
{
    if (!name)
        return 0;
    size_t length = strlen(name);
    size_t prefix = sizeof(GC_UPDATE_ZIP_PREFIX) - 1;
    size_t suffix = sizeof(GC_UPDATE_ZIP_SUFFIX) - 1;
    if (length < prefix + suffix || length >= GC_UPDATE_ASSET_MAX)
        return 0;
    if (strncmp(name, GC_UPDATE_ZIP_PREFIX, prefix) != 0)
        return 0;
    if (strcmp(name + length - suffix, GC_UPDATE_ZIP_SUFFIX) != 0)
        return 0;
    return 1;
}

int gc_update_info_validate(const GcUpdateInfo *info, char *error,
                            size_t error_size)
{
    if (!info || !info->tag[0] || !gc_update_is_app_zip(info->asset_name) ||
        strncmp(info->url, GC_UPDATE_URL_PREFIX,
                sizeof(GC_UPDATE_URL_PREFIX) - 1) != 0) {
        set_error(error, error_size, "release metadata is not from the official asset");
        return -1;
    }
    if (info->size < GC_UPDATE_SIZE_MIN || info->size > GC_UPDATE_SIZE_MAX) {
        set_error(error, error_size, "release size is outside the payload limit");
        return -1;
    }
    if (strlen(info->sha256) != 64) {
        set_error(error, error_size, "release is missing a SHA-256 digest");
        return -1;
    }
    for (size_t i = 0; i < 64; ++i) {
        char c = info->sha256[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            set_error(error, error_size, "release SHA-256 digest is invalid");
            return -1;
        }
    }
    return 0;
}

int gc_update_zip_validate(const void *header, size_t header_size,
                           uint64_t file_size, char *error,
                           size_t error_size)
{
    const unsigned char *bytes = header;
    /* PK\x03\x04: the local file header that opens every non-empty ZIP. */
    if (!bytes || header_size < 4 || file_size < GC_UPDATE_SIZE_MIN ||
        bytes[0] != 'P' || bytes[1] != 'K' || bytes[2] != 0x03 ||
        bytes[3] != 0x04) {
        set_error(error, error_size, "download is not a valid ZIP archive");
        return -1;
    }
    return 0;
}

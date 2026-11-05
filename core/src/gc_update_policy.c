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

static int parse_version(const char *value, unsigned output[3],
                         int *prerelease)
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
    *prerelease = *value == '-';
    return *value == '\0' || *value == '-' ? 0 : -1;
}

int gc_update_version_compare(const char *left, const char *right,
                              int *comparison)
{
    unsigned left_parts[3], right_parts[3];
    int left_pre, right_pre;
    if (!comparison || parse_version(left, left_parts, &left_pre) != 0 ||
        parse_version(right, right_parts, &right_pre) != 0)
        return -1;
    for (size_t i = 0; i < 3; ++i) {
        if (left_parts[i] != right_parts[i]) {
            *comparison = left_parts[i] > right_parts[i] ? 1 : -1;
            return 0;
        }
    }
    /* Same x.y.z: a final release outranks a prerelease of that version. */
    *comparison = right_pre - left_pre;
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

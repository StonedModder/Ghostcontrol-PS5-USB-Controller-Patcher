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
    *comparison = right_pre - left_pre;
    return 0;
}

int gc_update_info_validate(const GcUpdateInfo *info, char *error,
                            size_t error_size)
{
    if (!info || !info->tag[0] ||
        strcmp(info->asset_name, "Ghostcontrol.elf") != 0 ||
        strncmp(info->url, GC_UPDATE_URL_PREFIX,
                sizeof(GC_UPDATE_URL_PREFIX) - 1) != 0) {
        set_error(error, error_size, "release metadata is not from the official asset");
        return -1;
    }
    if (info->size < 65536u || info->size > 32u * 1024u * 1024u) {
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

int gc_update_elf_validate(const void *header, size_t header_size,
                           uint64_t file_size, char *error,
                           size_t error_size)
{
    const unsigned char *bytes = header;
    if (!bytes || header_size < 64 || file_size < 65536u ||
        bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F' || bytes[4] != 2 || bytes[5] != 1 ||
        (bytes[16] != 2 && bytes[16] != 3) || bytes[17] != 0 ||
        bytes[18] != 0x3e || bytes[19] != 0) {
        set_error(error, error_size, "download is not a valid x86-64 ELF payload");
        return -1;
    }
    return 0;
}

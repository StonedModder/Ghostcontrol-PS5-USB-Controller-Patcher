/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GC_UPDATE_TAG_MAX 64
#define GC_UPDATE_URL_MAX 512
#define GC_UPDATE_DIGEST_MAX 65

typedef struct {
    int available;
    char tag[GC_UPDATE_TAG_MAX];
    char asset_name[64];
    char url[GC_UPDATE_URL_MAX];
    char sha256[GC_UPDATE_DIGEST_MAX];
    uint64_t size;
} GcUpdateInfo;

int gc_update_version_compare(const char *left, const char *right,
                              int *comparison);
int gc_update_info_validate(const GcUpdateInfo *info, char *error,
                            size_t error_size);
int gc_update_elf_validate(const void *header, size_t header_size,
                           uint64_t file_size, char *error,
                           size_t error_size);

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GC_UPDATE_TAG_MAX 64
#define GC_UPDATE_URL_MAX 512
#define GC_UPDATE_DIGEST_MAX 65
#define GC_UPDATE_ASSET_MAX 96

/* The stable product identity. The native app installs as a folder named for
 * this title ID and every release attaches one app bundle whose name carries
 * both the kit prefix and this ID, so the updater can single it out from any
 * other asset (companion ELF, checksums, media) a release may hold. */
#define GC_UPDATE_APP_TITLE "PPSA62838"
#define GC_UPDATE_ZIP_PREFIX "Ghostcontrol-Toolbox-"
#define GC_UPDATE_ZIP_SUFFIX "-" GC_UPDATE_APP_TITLE ".zip"

/* The app bundle is ~40 MB today; leave generous headroom but still bound it
 * so a hostile or wrong asset can never stage an unbounded download. */
#define GC_UPDATE_SIZE_MIN (1u * 1024u * 1024u)
#define GC_UPDATE_SIZE_MAX (256ull * 1024ull * 1024ull)

typedef struct {
    int available;
    char tag[GC_UPDATE_TAG_MAX];
    char asset_name[GC_UPDATE_ASSET_MAX];
    char url[GC_UPDATE_URL_MAX];
    char sha256[GC_UPDATE_DIGEST_MAX];
    uint64_t size;
} GcUpdateInfo;

int gc_update_version_compare(const char *left, const char *right,
                              int *comparison);
int gc_update_info_validate(const GcUpdateInfo *info, char *error,
                            size_t error_size);

/* 1 when `name` is the app bundle attached to a release: the kit prefix, the
 * stable title ID and the .zip suffix. Rejects every other asset. */
int gc_update_is_app_zip(const char *name);

/* Confirms the staged download begins with the local-file ZIP signature and
 * is large enough to be the app bundle, before anything is unpacked. */
int gc_update_zip_validate(const void *header, size_t header_size,
                           uint64_t file_size, char *error,
                           size_t error_size);

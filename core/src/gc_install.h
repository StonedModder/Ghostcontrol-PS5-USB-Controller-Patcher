/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

#define GC_INSTALL_VERSION_MAX 40
#define GC_INSTALL_PATH_MAX 512
#define GC_INSTALL_TITLE_MAX 32

/* Where a staged update lives until it is applied. */
#define GC_INSTALL_WORK GC_UPDATE_DATA_ROOT "/updates"

typedef struct {
    int armed;
    char title[GC_INSTALL_TITLE_MAX];
    char version[GC_INSTALL_VERSION_MAX];
    char target[GC_INSTALL_PATH_MAX];
    char work[GC_INSTALL_PATH_MAX];
} GcInstallState;

/* Reads assets/ghostcontrol/version.txt from an app folder. 0 on success. */
int gc_install_read_version(const char *app_dir, char *out, size_t out_size);

/* Validates the archive and unpacks it to <work>/staged. Every entry must sit
 * under "<title>/", no absolute paths, no "..", no links. On success the
 * staged folder is confirmed to be this title at expect_version. */
int gc_install_stage(const char *archive, const char *title,
                     const char *expect_version, const char *work,
                     char *staged_version, size_t staged_version_size,
                     unsigned *file_count, char *error, size_t error_size);

/* Replaces the entries of target with the top-level entries of staged, moving
 * the originals to backup first. Both must share a filesystem. */
int gc_install_swap(const char *target, const char *staged,
                    const char *backup, char *error, size_t error_size);

/* Arms the swap: a detached thread waits for the title's sandbox to go away,
 * then swaps and notifies. Returns at once so the app can close itself. */
int gc_install_arm(const char *title, const char *target, const char *work,
                   const char *version, char *error, size_t error_size);

/* 1 while a swap is still waiting for the app to close. */
int gc_install_pending(void);

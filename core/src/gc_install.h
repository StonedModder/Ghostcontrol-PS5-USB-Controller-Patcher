/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#define GC_INSTALL_VERSION_MAX 40
#define GC_INSTALL_PATH_MAX 512
#define GC_INSTALL_TITLE_MAX 32
#define GC_INSTALL_WORK GC_UPDATE_DATA_ROOT "/updates"
typedef struct {
    int armed;
    char title[GC_INSTALL_TITLE_MAX];
    char version[GC_INSTALL_VERSION_MAX];
    char target[GC_INSTALL_PATH_MAX];
    char work[GC_INSTALL_PATH_MAX];
} GcInstallState;
int gc_install_read_version(const char *app_dir, char *out, size_t out_size);
int gc_install_stage(const char *archive, const char *title,
                     const char *expect_version, const char *work,
                     char *staged_version, size_t staged_version_size,
                     unsigned *file_count, char *error, size_t error_size);
int gc_install_swap(const char *target, const char *staged,
                    const char *backup, char *error, size_t error_size);
int gc_install_arm(const char *title, const char *target, const char *work,
                   const char *version, char *error, size_t error_size);
int gc_install_pending(void);

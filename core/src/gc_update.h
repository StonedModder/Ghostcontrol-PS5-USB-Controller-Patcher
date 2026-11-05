/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

#include "gc_update_policy.h"

int gc_update_check(const char *current_version, int include_prereleases,
                    GcUpdateInfo *info, char *error, size_t error_size);
int gc_update_download(const char *root, const GcUpdateInfo *info,
                       char *error, size_t error_size);
int gc_update_rollback(const char *root, char *error, size_t error_size);

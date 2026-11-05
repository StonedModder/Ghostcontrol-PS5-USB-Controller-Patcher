/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <sys/types.h>

/* Creates missing components and rejects any existing component that is not a
 * real directory.  The path must be absolute. */
int gc_ensure_real_directory_tree(const char *path, mode_t create_mode);

/* Opens an existing or newly-created regular file without following a final
 * symlink.  The returned inode must have exactly one link and is forced to
 * mode 0600 before it is returned.  O_CREAT/O_EXCL/O_TRUNC/O_NOFOLLOW belong
 * to this helper and are rejected in flags; callers that need truncation must
 * do it only after this function has validated the open descriptor. */
int gc_open_private_regular(const char *path, int flags, int create);

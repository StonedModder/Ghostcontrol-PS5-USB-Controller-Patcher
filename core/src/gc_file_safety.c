/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "gc_file_safety.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int
ensure_one_directory(const char *path, mode_t create_mode)
{
    struct stat info;
    if (lstat(path, &info) == 0) {
        if (!S_ISLNK(info.st_mode) && S_ISDIR(info.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }
    if (errno != ENOENT)
        return -1;
    if (mkdir(path, create_mode) != 0 && errno != EEXIST)
        return -1;
    if (lstat(path, &info) != 0)
        return -1;
    if (S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int
gc_ensure_real_directory_tree(const char *path, mode_t create_mode)
{
    if (!path || path[0] != '/' || path[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    size_t length = strlen(path);
    if (length >= 4096) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *copy = malloc(length + 1);
    if (!copy)
        return -1;
    memcpy(copy, path, length + 1);

    int result = 0;
    for (size_t index = 1; index < length; ++index) {
        if (copy[index] != '/')
            continue;
        copy[index] = '\0';
        if (ensure_one_directory(copy, create_mode) != 0) {
            result = -1;
            break;
        }
        copy[index] = '/';
    }
    if (result == 0)
        result = ensure_one_directory(copy, create_mode);
    int saved = errno;
    free(copy);
    if (result != 0)
        errno = saved;
    return result;
}

int
gc_open_private_regular(const char *path, int flags, int create)
{
    if (!path || (flags & (O_CREAT | O_EXCL | O_TRUNC | O_NOFOLLOW)) != 0) {
        errno = EINVAL;
        return -1;
    }
    /* Avoid opening a known device/FIFO at all.  The descriptor checks below
     * remain authoritative because this pathname inspection can race. */
    struct stat path_info;
    if (lstat(path, &path_info) == 0) {
        if (S_ISLNK(path_info.st_mode)) {
            errno = ELOOP;
            return -1;
        }
        if (!S_ISREG(path_info.st_mode)) {
            errno = EINVAL;
            return -1;
        }
        if (path_info.st_nlink != 1) {
            errno = EMLINK;
            return -1;
        }
    } else if (errno != ENOENT || !create) {
        return -1;
    }
    /* Opening a hostile FIFO must not block before fstat can reject it. */
    int open_flags = flags | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    if (create)
        open_flags |= O_CREAT;
    int fd = open(path, open_flags, 0600);
    if (fd < 0)
        return -1;

    int saved = 0;
    struct stat info;
    if (fstat(fd, &info) != 0)
        saved = errno;
    else if (!S_ISREG(info.st_mode))
        saved = EINVAL;
    else if (info.st_nlink != 1)
        saved = EMLINK;
    else if (fchmod(fd, 0600) != 0)
        saved = errno;
    else if (fstat(fd, &info) != 0)
        saved = errno;
    else if (!S_ISREG(info.st_mode) || info.st_nlink != 1 ||
             (info.st_mode & 0777u) != 0600u)
        saved = EIO;
    else if ((flags & O_NONBLOCK) == 0) {
        int status_flags = fcntl(fd, F_GETFL);
        if (status_flags < 0 ||
            fcntl(fd, F_SETFL, status_flags & ~O_NONBLOCK) != 0)
            saved = errno;
    }

    if (!saved)
        return fd;
    (void)close(fd);
    errno = saved;
    return -1;
}

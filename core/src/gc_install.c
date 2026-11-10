/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Applies a downloaded, hash-verified app bundle in place of the installed
 * title. The archive is unpacked beside the install while the app still runs;
 * the swap itself waits until the title's sandbox is gone, because an app
 * cannot write over its own mounted files.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "gc_install.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <minizip/unzip.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "gc_notify.h"

#define GC_INSTALL_SANDBOXES "/mnt/sandbox"
#define GC_INSTALL_SETTLE_MS 1500u
#define GC_INSTALL_EXIT_WAIT_MS 120000u /* 2 min */
#define GC_INSTALL_CHUNK (256u * 1024u)
#define GC_INSTALL_TOP_MAX 64

static pthread_mutex_t g_install_lock = PTHREAD_MUTEX_INITIALIZER;
static GcInstallState g_state;

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    if (!error || !error_size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec pause = {(time_t)(milliseconds / 1000u),
                             (long)(milliseconds % 1000u) * 1000000L};
    nanosleep(&pause, NULL);
}

int gc_install_read_version(const char *app_dir, char *out, size_t out_size)
{
    char path[GC_INSTALL_PATH_MAX];
    if (!app_dir || !out || out_size < 2 ||
        snprintf(path, sizeof(path), "%s/assets/ghostcontrol/version.txt",
                 app_dir) >= (int)sizeof(path))
        return -1;
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    char buffer[GC_INSTALL_VERSION_MAX * 2];
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r' ||
                      buffer[length - 1] == ' '))
        --length;
    if (!length || length >= out_size)
        return -1;
    memcpy(out, buffer, length);
    out[length] = '\0';
    return 0;
}

/* True when an archive member would escape the staged folder, or lives outside
 * the title's own folder. */
static int name_is_unsafe(const char *name, const char *title)
{
    size_t prefix = strlen(title);
    if (!name || name[0] == '\0')
        return 1;
    if (name[0] == '/' || name[0] == '\\')
        return 1;
    if (strstr(name, "..") || strchr(name, '\\') || strchr(name, ':'))
        return 1;
    if (strncmp(name, title, prefix) != 0)
        return 1;
    if (name[prefix] != '/' && name[prefix] != '\0')
        return 1;
    return 0;
}

static int make_path(const char *path)
{
    char buffer[GC_INSTALL_PATH_MAX];
    if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer))
        return -1;
    for (char *cursor = buffer + 1; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(buffer, 0777) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    if (mkdir(buffer, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Recursively removes a tree; best effort, used to guarantee a clean stage and
 * a clean backup before they are written to. */
static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory) {
        unlink(path);
        return;
    }
    struct dirent *entry;
    char child[GC_INSTALL_PATH_MAX + 512];
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        remove_tree(child);
    }
    closedir(directory);
    rmdir(path);
}

static int write_current(unzFile archive, const char *destination,
                         uint64_t expected, char *error, size_t error_size)
{
    if (unzOpenCurrentFile(archive) != UNZ_OK) {
        set_error(error, error_size, "entry could not be opened");
        return -1;
    }
    int fd = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) {
        unzCloseCurrentFile(archive);
        set_error(error, error_size, "cannot write %s", destination);
        return -1;
    }
    /* Heap, not stack: this runs on an IPC worker thread whose stack is far
     * smaller than the main one, and a quarter-megabyte frame there overflowed
     * it and killed the service mid-extract. */
    unsigned char *buffer = malloc(GC_INSTALL_CHUNK);
    if (!buffer) {
        close(fd);
        unzCloseCurrentFile(archive);
        set_error(error, error_size, "out of memory while unpacking");
        return -1;
    }
    uint64_t written = 0;
    int failed = 0;
    for (;;) {
        int count = unzReadCurrentFile(archive, buffer, GC_INSTALL_CHUNK);
        if (count < 0) {
            failed = 1;
            break;
        }
        if (count == 0)
            break;
        ssize_t offset = 0;
        while (offset < count) {
            ssize_t put = write(fd, buffer + offset, (size_t)(count - offset));
            if (put <= 0) {
                if (put < 0 && errno == EINTR)
                    continue;
                failed = 1;
                break;
            }
            offset += put;
        }
        if (failed)
            break;
        written += (uint64_t)count;
        if (expected && written > expected) {
            failed = 1;
            break;
        }
    }
    if (close(fd) != 0)
        failed = 1;
    free(buffer);
    unzCloseCurrentFile(archive);
    if (failed || (expected && written != expected)) {
        unlink(destination);
        set_error(error, error_size, "entry did not unpack cleanly");
        return -1;
    }
    chmod(destination, 0777);
    return 0;
}

/* Reads "titleId" out of a param.json without pulling in a JSON dependency. */
static int read_param_title(const char *param_path, char *out, size_t out_size)
{
    FILE *param = fopen(param_path, "rb");
    if (!param)
        return -1;
    char text[8192];
    size_t length = fread(text, 1, sizeof(text) - 1, param);
    fclose(param);
    text[length] = '\0';
    const char *found = strstr(text, "\"titleId\"");
    if (!found || !(found = strchr(found, ':')))
        return -1;
    ++found;
    while (*found == ' ' || *found == '\t')
        ++found;
    if (*found != '"')
        return -1;
    ++found;
    size_t copy = 0;
    while (found[copy] && found[copy] != '"' && copy + 1 < out_size) {
        out[copy] = found[copy];
        ++copy;
    }
    out[copy] = '\0';
    return copy ? 0 : -1;
}

int gc_install_stage(const char *archive_path, const char *title,
                     const char *expect_version, const char *work,
                     char *staged_version, size_t staged_version_size,
                     unsigned *file_count, char *error, size_t error_size)
{
    if (!archive_path || !title || !work) {
        set_error(error, error_size, "invalid request");
        return -1;
    }
    char staged[GC_INSTALL_PATH_MAX];
    if (snprintf(staged, sizeof(staged), "%s/staged", work) >=
        (int)sizeof(staged)) {
        set_error(error, error_size, "work path too long");
        return -1;
    }
    /* A failed or interrupted stage must never merge into the next attempt. */
    remove_tree(staged);
    unzFile archive = unzOpen(archive_path);
    if (!archive) {
        set_error(error, error_size, "download is not a readable ZIP");
        return -1;
    }
    int result = -1;
    unsigned files = 0;
    char name[512];
    if (unzGoToFirstFile(archive) != UNZ_OK) {
        set_error(error, error_size, "archive is empty");
        unzClose(archive);
        return -1;
    }
    do {
        unz_file_info64 info;
        memset(&info, 0, sizeof(info));
        if (unzGetCurrentFileInfo64(archive, &info, name, sizeof(name), NULL, 0,
                                    NULL, 0) != UNZ_OK) {
            set_error(error, error_size, "archive entry cannot be read");
            goto done;
        }
        if (name_is_unsafe(name, title)) {
            set_error(error, error_size, "archive contains an unsafe path");
            goto done;
        }
        unsigned mode = (unsigned)(info.external_fa >> 16);
        if (S_ISLNK(mode)) {
            set_error(error, error_size, "archive contains a link");
            goto done;
        }
        size_t length = strlen(name);
        int directory = name[length - 1] == '/';
        /* The archive is rooted at the title folder. The staged tree mirrors the
         * installed folder itself, so that one component is dropped here: the
         * version marker, param.json and the swap all assume that layout. */
        const char *relative = name + strlen(title);
        if (*relative == '/')
            ++relative;
        char destination[GC_INSTALL_PATH_MAX + 512];
        if (snprintf(destination, sizeof(destination), "%s/%s", staged,
                     relative) >= (int)sizeof(destination)) {
            set_error(error, error_size, "archive path too long");
            goto done;
        }
        if (directory) {
            destination[strlen(destination) - 1] = '\0';
            if (make_path(destination) != 0) {
                set_error(error, error_size, "cannot create %s", destination);
                goto done;
            }
            continue;
        }
        char parent[GC_INSTALL_PATH_MAX + 512];
        snprintf(parent, sizeof(parent), "%s", destination);
        char *slash = strrchr(parent, '/');
        if (!slash) {
            set_error(error, error_size, "archive path has no parent");
            goto done;
        }
        *slash = '\0';
        if (make_path(parent) != 0) {
            set_error(error, error_size, "cannot create %s", parent);
            goto done;
        }
        if (write_current(archive, destination, info.uncompressed_size, error,
                          error_size) != 0)
            goto done;
        ++files;
    } while (unzGoToNextFile(archive) == UNZ_OK);
    if (!files) {
        set_error(error, error_size, "archive holds no files");
        goto done;
    }

    /* The staged copy must be this title, at the version the release claims. */
    char found_version[GC_INSTALL_VERSION_MAX] = "";
    if (gc_install_read_version(staged, found_version, sizeof(found_version)) !=
        0) {
        if (expect_version && expect_version[0]) {
            set_error(error, error_size, "download carries no version marker");
            goto done;
        }
    } else if (expect_version && expect_version[0] &&
               strcmp(found_version, expect_version) != 0) {
        set_error(error, error_size, "download is version %s, release says %s",
                  found_version, expect_version);
        goto done;
    }

    char param_path[GC_INSTALL_PATH_MAX + 512];
    if (snprintf(param_path, sizeof(param_path), "%s/sce_sys/param.json",
                 staged) >= (int)sizeof(param_path)) {
        set_error(error, error_size, "staged path too long");
        goto done;
    }
    char title_id[32] = "";
    if (read_param_title(param_path, title_id, sizeof(title_id)) != 0) {
        set_error(error, error_size, "download has no readable param.json");
        goto done;
    }
    if (strcmp(title_id, title) != 0) {
        set_error(error, error_size, "download is for %s, not %s", title_id,
                  title);
        goto done;
    }

    if (staged_version && staged_version_size)
        snprintf(staged_version, staged_version_size, "%s", found_version);
    if (file_count)
        *file_count = files;
    result = 0;
done:
    unzClose(archive);
    if (result != 0)
        remove_tree(staged);
    return result;
}

/* Sorted listing of a folder's direct children, without "." and "..". */
static int list_children(const char *path, char names[][256], int capacity)
{
    DIR *directory = opendir(path);
    if (!directory)
        return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        /* Refuse rather than silently swap half a folder: a truncated list
         * would look like a successful install. */
        if (count >= capacity) {
            closedir(directory);
            return -2;
        }
        snprintf(names[count], 256, "%s", entry->d_name);
        ++count;
    }
    closedir(directory);
    return count;
}

int gc_install_swap(const char *target, const char *staged, const char *backup,
                    char *error, size_t error_size)
{
    static char present[GC_INSTALL_TOP_MAX][256];
    static char incoming[GC_INSTALL_TOP_MAX][256];
    int present_count = list_children(target, present, GC_INSTALL_TOP_MAX);
    int incoming_count = list_children(staged, incoming, GC_INSTALL_TOP_MAX);
    if (present_count < 0 || incoming_count <= 0) {
        set_error(error, error_size, "folders cannot be listed");
        return -1;
    }
    /* Only the first update in a product's life would ever succeed if a
     * previous backup were reused: rename() refuses a non-empty destination. */
    remove_tree(backup);
    if (mkdir(backup, 0777) != 0 && errno != EEXIST) {
        set_error(error, error_size, "backup folder cannot be made");
        return -1;
    }
    /* Move aside only the entries the release also ships; anything else in the
     * folder belongs to the installation or the user and stays. */
    static char moved_names[GC_INSTALL_TOP_MAX][256];
    static char placed_names[GC_INSTALL_TOP_MAX][256];
    int moved = 0, placed = 0;
    char from[GC_INSTALL_PATH_MAX + 300], to[GC_INSTALL_PATH_MAX + 300];
    for (int i = 0; i < present_count; ++i) {
        int replaces = 0;
        for (int j = 0; j < incoming_count; ++j)
            if (!strcmp(present[i], incoming[j]))
                replaces = 1;
        if (!replaces)
            continue;
        snprintf(from, sizeof(from), "%s/%s", target, present[i]);
        snprintf(to, sizeof(to), "%s/%s", backup, present[i]);
        if (rename(from, to) != 0) {
            set_error(error, error_size, "cannot move %s aside", present[i]);
            goto rollback;
        }
        snprintf(moved_names[moved], 256, "%s", present[i]);
        ++moved;
    }
    for (int j = 0; j < incoming_count; ++j) {
        snprintf(from, sizeof(from), "%s/%s", staged, incoming[j]);
        snprintf(to, sizeof(to), "%s/%s", target, incoming[j]);
        if (rename(from, to) != 0) {
            set_error(error, error_size, "cannot place %s", incoming[j]);
            goto rollback;
        }
        snprintf(placed_names[placed], 256, "%s", incoming[j]);
        ++placed;
    }
    return 0;

rollback:
    for (int j = 0; j < placed; ++j) {
        snprintf(from, sizeof(from), "%s/%s", target, placed_names[j]);
        snprintf(to, sizeof(to), "%s/%s", staged, placed_names[j]);
        (void)rename(from, to);
    }
    for (int i = 0; i < moved; ++i) {
        snprintf(from, sizeof(from), "%s/%s", backup, moved_names[i]);
        snprintf(to, sizeof(to), "%s/%s", target, moved_names[i]);
        (void)rename(from, to);
    }
    return -1;
}

/* 1 running, 0 gone, -1 unknown. A leftover sandbox directory proves nothing:
 * the console keeps <title>_<n> around after the app closes, so the app0 mount
 * only counts as live when its eboot.bin is present and readable. */
static int title_running(const char *title)
{
    DIR *directory = opendir(GC_INSTALL_SANDBOXES);
    if (!directory)
        return -1;
    size_t prefix = strlen(title);
    int running = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        const char *name_text = entry->d_name;
        if (strncmp(name_text, title, prefix) != 0 || name_text[prefix] != '_')
            continue;
        char probe[512];
        snprintf(probe, sizeof(probe), "%s/%s/app0/eboot.bin",
                 GC_INSTALL_SANDBOXES, name_text);
        struct stat info;
        if (stat(probe, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size <= 0)
            continue;
        int readable = open(probe, O_RDONLY);
        if (readable < 0)
            continue;
        close(readable);
        running = 1;
        break;
    }
    closedir(directory);
    return running;
}

/* Waits out the closing app, then swaps. Runs detached. */
static void *apply_thread(void *argument)
{
    (void)argument;
    GcInstallState local;
    pthread_mutex_lock(&g_install_lock);
    local = g_state;
    pthread_mutex_unlock(&g_install_lock);

    /* The app may still be closing. Wait while its app0 mount is live; a probe
     * that says gone (0) or unknown (-1) must not hold the swap back, so an
     * unreadable /mnt/sandbox cannot block a legitimate install. */
    unsigned waited = 0;
    while (waited < GC_INSTALL_EXIT_WAIT_MS) {
        int running = title_running(local.title);
        if (running <= 0)
            break;
        sleep_ms(500);
        waited += 500;
    }
    if (title_running(local.title) == 1) {
        gc_notify_send("Ghostcontrol wasn't updated: the app didn't close. "
                       "Nothing was changed.");
        pthread_mutex_lock(&g_install_lock);
        g_state.armed = 0;
        pthread_mutex_unlock(&g_install_lock);
        return NULL;
    }
    /* Let the console finish releasing the mount before the swap. */
    sleep_ms(GC_INSTALL_SETTLE_MS);

    char staged[GC_INSTALL_PATH_MAX], backup[GC_INSTALL_PATH_MAX + 16];
    if (snprintf(staged, sizeof(staged), "%s/staged", local.work) >=
            (int)sizeof(staged) ||
        snprintf(backup, sizeof(backup), "%s/backup", local.work) >=
            (int)sizeof(backup)) {
        pthread_mutex_lock(&g_install_lock);
        g_state.armed = 0;
        pthread_mutex_unlock(&g_install_lock);
        return NULL;
    }

    char error[160] = "";
    if (gc_install_swap(local.target, staged, backup, error, sizeof(error)) !=
        0) {
        char message[200];
        snprintf(message, sizeof(message),
                 "Ghostcontrol wasn't updated: %s. Nothing was changed.",
                 error[0] ? error : "its files couldn't be replaced");
        gc_notify_send(message);
    } else {
        char message[220];
        snprintf(message, sizeof(message),
                 "Ghostcontrol was updated to %s. Open it again.",
                 local.version[0] ? local.version : "the new version");
        gc_notify_send(message);
    }
    pthread_mutex_lock(&g_install_lock);
    g_state.armed = 0;
    pthread_mutex_unlock(&g_install_lock);
    return NULL;
}

int gc_install_arm(const char *title, const char *target, const char *work,
                   const char *version, char *error, size_t error_size)
{
    if (!title || !target || !work) {
        set_error(error, error_size, "invalid request");
        return -1;
    }
    pthread_mutex_lock(&g_install_lock);
    if (g_state.armed) {
        pthread_mutex_unlock(&g_install_lock);
        set_error(error, error_size, "an update is already waiting to apply");
        return -1;
    }
    memset(&g_state, 0, sizeof(g_state));
    snprintf(g_state.title, sizeof(g_state.title), "%s", title);
    snprintf(g_state.target, sizeof(g_state.target), "%s", target);
    snprintf(g_state.work, sizeof(g_state.work), "%s", work);
    snprintf(g_state.version, sizeof(g_state.version), "%s",
             version ? version : "");
    g_state.armed = 1;
    pthread_mutex_unlock(&g_install_lock);

    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    int created = pthread_create(&thread, &attributes, apply_thread, NULL);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        pthread_mutex_lock(&g_install_lock);
        g_state.armed = 0;
        pthread_mutex_unlock(&g_install_lock);
        set_error(error, error_size, "the apply thread could not start");
        return -1;
    }
    return 0;
}

int gc_install_pending(void)
{
    int pending;
    pthread_mutex_lock(&g_install_lock);
    pending = g_state.armed;
    pthread_mutex_unlock(&g_install_lock);
    return pending;
}

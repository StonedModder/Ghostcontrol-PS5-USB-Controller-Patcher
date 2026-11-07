/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "gc_update.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <openssl/evp.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GC_UPDATE_API \
    "https://api.github.com/repos/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher"
#define GC_UPDATE_USER_AGENT "Ghostcontrol-Toolbox/1.0"
#define GC_UPDATE_JSON_LIMIT (2u * 1024u * 1024u)

extern const unsigned char gc_ca_bundle_start[];
extern const unsigned char gc_ca_bundle_end[];

typedef struct {
    unsigned char *data;
    size_t size;
    size_t limit;
} MemoryBuffer;

typedef struct {
    int fd;
    uint64_t size;
    uint64_t limit;
    int failed;
} FileBuffer;

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static CURLcode g_curl_init_result = CURLE_FAILED_INIT;

static void curl_initialize(void)
{
    g_curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    if (!error || !error_size)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

static int ensure_ca_bundle(const char *root, char path[512],
                            char *error, size_t error_size)
{
    int count = snprintf(path, 512, "%s/ca-bundle.crt", root);
    if (count < 0 || count >= 512)
        return -1;
    struct stat info;
    size_t embedded_size = (size_t)(gc_ca_bundle_end - gc_ca_bundle_start);
    if (lstat(path, &info) == 0 && S_ISREG(info.st_mode) &&
        (size_t)info.st_size == embedded_size)
        return 0;
    if (lstat(path, &info) == 0 && S_ISLNK(info.st_mode)) {
        set_error(error, error_size, "refusing symlink CA path");
        return -1;
    }
    char temporary[544];
    count = snprintf(temporary, sizeof(temporary), "%s.tmp.%d", path, getpid());
    if (count < 0 || (size_t)count >= sizeof(temporary))
        return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || write_all(fd, gc_ca_bundle_start, embedded_size) != 0 ||
        fsync(fd) != 0 || close(fd) != 0 || rename(temporary, path) != 0) {
        int saved = errno;
        if (fd >= 0)
            close(fd);
        unlink(temporary);
        errno = saved;
        set_error(error, error_size, "cannot install CA bundle: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static size_t memory_write(char *data, size_t unit, size_t count, void *context)
{
    MemoryBuffer *buffer = context;
    size_t bytes = unit * count;
    if (!bytes || buffer->size > buffer->limit - bytes)
        return 0;
    unsigned char *grown = realloc(buffer->data, buffer->size + bytes + 1);
    if (!grown)
        return 0;
    buffer->data = grown;
    memcpy(buffer->data + buffer->size, data, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = '\0';
    return bytes;
}

static size_t file_write(char *data, size_t unit, size_t count, void *context)
{
    FileBuffer *file = context;
    size_t bytes = unit * count;
    if (!bytes || file->size > file->limit - bytes ||
        write_all(file->fd, data, bytes) != 0) {
        file->failed = 1;
        return 0;
    }
    file->size += bytes;
    return bytes;
}

static CURL *curl_create(const char *url, const char *ca_path,
                         char *error, size_t error_size)
{
    pthread_once(&g_curl_once, curl_initialize);
    if (g_curl_init_result != CURLE_OK) {
        set_error(error, error_size, "curl initialization failed");
        return NULL;
    }
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, GC_UPDATE_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    return curl;
}

static int http_json(const char *url, const char *ca_path, MemoryBuffer *buffer,
                     char *error, size_t error_size)
{
    CURL *curl = curl_create(url, ca_path, error, error_size);
    if (!curl)
        return -1;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memory_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status != 200) {
        set_error(error, error_size, "GitHub request failed: HTTP %ld, %s",
                  status, curl_easy_strerror(result));
        return -1;
    }
    return 0;
}

static int copy_json_string(json_object *object, const char *key,
                            char *output, size_t output_size)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string))
        return -1;
    const char *text = json_object_get_string(value);
    int count = snprintf(output, output_size, "%s", text ? text : "");
    return count < 0 || (size_t)count >= output_size ? -1 : 0;
}

static int release_info(json_object *release, int include_prereleases,
                        GcUpdateInfo *info, char *error, size_t error_size)
{
    json_object *value = NULL;
    if (!json_object_is_type(release, json_type_object) ||
        (json_object_object_get_ex(release, "draft", &value) &&
         json_object_get_boolean(value)) ||
        (!include_prereleases &&
         json_object_object_get_ex(release, "prerelease", &value) &&
         json_object_get_boolean(value)) ||
        copy_json_string(release, "tag_name", info->tag, sizeof(info->tag)) != 0 ||
        !json_object_object_get_ex(release, "assets", &value) ||
        !json_object_is_type(value, json_type_array))
        return -1;

    size_t count = json_object_array_length(value);
    for (size_t i = 0; i < count; ++i) {
        json_object *asset = json_object_array_get_idx(value, i);
        char name[GC_UPDATE_ASSET_MAX];
        if (copy_json_string(asset, "name", name, sizeof(name)) != 0 ||
            !gc_update_is_app_zip(name))
            continue;
        memset(info, 0, sizeof(*info));
        if (copy_json_string(release, "tag_name", info->tag, sizeof(info->tag)) != 0 ||
            copy_json_string(asset, "name", info->asset_name,
                             sizeof(info->asset_name)) != 0 ||
            copy_json_string(asset, "browser_download_url", info->url,
                             sizeof(info->url)) != 0 ||
            !json_object_object_get_ex(asset, "digest", &value) ||
            !json_object_is_type(value, json_type_string))
            break;
        const char *digest = json_object_get_string(value);
        if (!digest || strncmp(digest, "sha256:", 7) != 0 ||
            snprintf(info->sha256, sizeof(info->sha256), "%s", digest + 7) != 64)
            break;
        if (!json_object_object_get_ex(asset, "size", &value) ||
            !json_object_is_type(value, json_type_int))
            break;
        info->size = (uint64_t)json_object_get_int64(value);
        return gc_update_info_validate(info, error, error_size);
    }
    set_error(error, error_size,
              "No verified Ghostcontrol app bundle (.zip) found in this release");
    return -1;
}

int gc_update_check(const char *current_version, int include_prereleases,
                    GcUpdateInfo *info, char *error, size_t error_size)
{
    if (!current_version || !info)
        return -1;
    memset(info, 0, sizeof(*info));
    char ca_path[512];
    if (ensure_ca_bundle("/data/ghostcontrol", ca_path,
                         error, error_size) != 0)
        return -1;
    /* Always scan the release list. Stable keeps prereleases out; the prerelease
     * channel accepts both. /releases/latest alone is not enough: older 1.x
     * stables only shipped a combined ELF, while 2.x ships the app ZIP. */
    char url[sizeof(GC_UPDATE_API) + 32];
    snprintf(url, sizeof(url), "%s/releases?per_page=15", GC_UPDATE_API);
    MemoryBuffer buffer = {NULL, 0, GC_UPDATE_JSON_LIMIT};
    if (http_json(url, ca_path, &buffer, error, error_size) != 0) {
        free(buffer.data);
        return -1;
    }
    json_object *root = json_tokener_parse((const char *)buffer.data);
    free(buffer.data);
    if (!root || !json_object_is_type(root, json_type_array)) {
        if (root)
            json_object_put(root);
        set_error(error, error_size, "GitHub returned invalid JSON");
        return -1;
    }
    int result = -1;
    size_t count = json_object_array_length(root);
    for (size_t i = 0; i < count && result != 0; ++i)
        result = release_info(json_object_array_get_idx(root, i),
                              include_prereleases, info, error, error_size);
    json_object_put(root);
    if (result != 0)
        return -1;
    int comparison = 0;
    if (gc_update_version_compare(info->tag, current_version, &comparison) != 0) {
        set_error(error, error_size, "release tag is not semantic versioning");
        return -1;
    }
    info->available = comparison > 0;
    return 0;
}

static int sha256_file(const char *path, char digest[65], uint64_t *file_size)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (fd < 0 || !context || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        if (fd >= 0)
            close(fd);
        EVP_MD_CTX_free(context);
        return -1;
    }
    unsigned char buffer[16384];
    uint64_t total = 0;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0 || EVP_DigestUpdate(context, buffer, (size_t)count) != 1) {
            close(fd);
            EVP_MD_CTX_free(context);
            return -1;
        }
        total += (uint64_t)count;
    }
    close(fd);
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (EVP_DigestFinal_ex(context, bytes, &length) != 1 || length != 32) {
        EVP_MD_CTX_free(context);
        return -1;
    }
    EVP_MD_CTX_free(context);
    for (size_t i = 0; i < 32; ++i)
        snprintf(digest + i * 2, 3, "%02x", bytes[i]);
    digest[64] = '\0';
    if (file_size)
        *file_size = total;
    return 0;
}

static int validate_download(const char *path, const GcUpdateInfo *info,
                             char *error, size_t error_size)
{
    char digest[65];
    uint64_t size = 0;
    if (sha256_file(path, digest, &size) != 0 || size != info->size ||
        strcmp(digest, info->sha256) != 0) {
        set_error(error, error_size, "download SHA-256 or size mismatch");
        return -1;
    }
    unsigned char header[64];
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    ssize_t count = fd >= 0 ? read(fd, header, sizeof(header)) : -1;
    if (fd >= 0)
        close(fd);
    return count == (ssize_t)sizeof(header)
               ? gc_update_zip_validate(header, sizeof(header), size,
                                        error, error_size) : -1;
}

static int write_metadata(const char *root, const GcUpdateInfo *info)
{
    char path[512], temporary[544], text[1024];
    if (snprintf(path, sizeof(path), "%s/updates/installed.ini", root) >=
            (int)sizeof(path) ||
        snprintf(temporary, sizeof(temporary), "%s.tmp.%d", path, getpid()) >=
            (int)sizeof(temporary))
        return -1;
    int length = snprintf(text, sizeof(text),
                          "schema=1\ntag=%s\nasset=%s\nsize=%llu\nsha256=%s\n",
                          info->tag, info->asset_name,
                          (unsigned long long)info->size, info->sha256);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (length < 0 || (size_t)length >= sizeof(text) || fd < 0 ||
        write_all(fd, text, (size_t)length) != 0 || fsync(fd) != 0 ||
        close(fd) != 0 || rename(temporary, path) != 0) {
        if (fd >= 0)
            close(fd);
        unlink(temporary);
        return -1;
    }
    return 0;
}

int gc_update_download(const char *root, const GcUpdateInfo *info,
                       char *error, size_t error_size)
{
    if (!root || gc_update_info_validate(info, error, error_size) != 0)
        return -1;
    char ca_path[512], staged[544], current[512], previous[544], updates[512];
    if (ensure_ca_bundle(root, ca_path, error, error_size) != 0 ||
        snprintf(updates, sizeof(updates), "%s/updates", root) >=
                 (int)sizeof(updates) ||
        snprintf(staged, sizeof(staged),
                 "%s/Ghostcontrol-Toolbox-app.zip.download.%d", updates,
                 getpid()) >= (int)sizeof(staged) ||
        snprintf(current, sizeof(current),
                 "%s/Ghostcontrol-Toolbox-app.zip", updates) >=
                 (int)sizeof(current) ||
        snprintf(previous, sizeof(previous), "%s.previous", current) >=
                 (int)sizeof(previous))
        return -1;
    (void)mkdir(updates, 0755);
    int fd = open(staged, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        set_error(error, error_size, "cannot create update staging file: %s",
                  strerror(errno));
        return -1;
    }
    FileBuffer file = {fd, 0, info->size, 0};
    CURL *curl = curl_create(info->url, ca_path, error, error_size);
    if (!curl) {
        close(fd); unlink(staged); return -1;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)info->size);
    CURLcode transfer = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    int sync_result = fsync(fd);
    int close_result = close(fd);
    if (transfer != CURLE_OK || status != 200 || file.failed ||
        file.size != info->size || sync_result != 0 || close_result != 0 ||
        validate_download(staged, info, error, error_size) != 0) {
        if (!error || !error[0])
            set_error(error, error_size, "download failed: HTTP %ld, %s",
                      status, curl_easy_strerror(transfer));
        unlink(staged);
        return -1;
    }

    struct stat current_info;
    int had_current = lstat(current, &current_info) == 0;
    if (had_current && (!S_ISREG(current_info.st_mode) ||
                        S_ISLNK(current_info.st_mode))) {
        unlink(staged);
        set_error(error, error_size, "current update path is not a regular file");
        return -1;
    }
    if (had_current) {
        unlink(previous);
        if (rename(current, previous) != 0) {
            unlink(staged);
            set_error(error, error_size, "cannot create update rollback: %s",
                      strerror(errno));
            return -1;
        }
    }
    if (rename(staged, current) != 0) {
        if (had_current)
            (void)rename(previous, current);
        unlink(staged);
        set_error(error, error_size, "cannot activate downloaded update: %s",
                  strerror(errno));
        return -1;
    }
    if (write_metadata(root, info) != 0) {
        unlink(current);
        if (had_current)
            (void)rename(previous, current);
        set_error(error, error_size, "cannot commit update metadata");
        return -1;
    }
    return 0;
}

int gc_update_rollback(const char *root, char *error, size_t error_size)
{
    char current[512], previous[544], failed[544];
    if (!root ||
        snprintf(current, sizeof(current),
                 "%s/updates/Ghostcontrol-Toolbox-app.zip", root) >=
            (int)sizeof(current) ||
        snprintf(previous, sizeof(previous), "%s.previous", current) >=
            (int)sizeof(previous) ||
        snprintf(failed, sizeof(failed), "%s.failed", current) >=
            (int)sizeof(failed))
        return -1;
    struct stat info;
    if (lstat(previous, &info) != 0 || !S_ISREG(info.st_mode) ||
        S_ISLNK(info.st_mode)) {
        set_error(error, error_size, "no verified previous update is available");
        return -1;
    }
    unlink(failed);
    if (rename(current, failed) != 0 || rename(previous, current) != 0) {
        (void)rename(failed, current);
        set_error(error, error_size, "update rollback failed: %s", strerror(errno));
        return -1;
    }
    unlink(failed);
    return 0;
}

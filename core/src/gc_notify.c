/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "gc_notify.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern int sceNotificationSend(int user_id, int is_logged,
                               const char *payload);
extern int32_t sceKernelSendNotificationRequest(int device, void *request,
                                                size_t size, int flags);

typedef struct {
    int32_t type;
    int32_t request_id;
    int32_t priority;
    int32_t message_id;
    int32_t target_id;
    int32_t user_id;
    int32_t unknown1;
    int32_t unknown2;
    int32_t app_id;
    int32_t error_number;
    int32_t unknown3;
    char use_icon_uri;
    char message[1024];
    char uri[1024];
    char unknown_string[1024];
    char padding[3];
} GcNotificationRequest;

_Static_assert(sizeof(GcNotificationRequest) == 0xc30,
               "notification request layout changed");

static unsigned g_notification_id;

static int json_escape(char *output, size_t output_size, const char *input)
{
    size_t used = 0;
    if (!output || output_size == 0)
        return -1;
    if (!input)
        input = "";
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        const char *escape = NULL;
        switch (*p) {
        case '\\': escape = "\\\\"; break;
        case '"': escape = "\\\""; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape) {
            size_t length = strlen(escape);
            if (used + length >= output_size)
                return -1;
            memcpy(output + used, escape, length);
            used += length;
        } else if (*p < 0x20) {
            if (used + 6 >= output_size)
                return -1;
            int length = snprintf(output + used, output_size - used,
                                  "\\u%04x", (unsigned)*p);
            if (length != 6)
                return -1;
            used += 6;
        } else {
            if (used + 1 >= output_size)
                return -1;
            output[used++] = (char)*p;
        }
    }
    output[used] = '\0';
    return 0;
}

int gc_notify_build_json(char *output, size_t output_size,
                         const char *detail, unsigned notification_id)
{
    char escaped[1536];
    char timestamp[32];
    time_t now = time(NULL);
    struct tm utc;
    if (!gmtime_r(&now, &utc) ||
        !strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", &utc))
        return -1;
    if (json_escape(escaped, sizeof(escaped), detail) != 0)
        return -1;
    int length = snprintf(
        output, output_size,
        "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
        "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
        "\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
        "\"viewData\":{\"icon\":{\"type\":\"Url\",\"parameters\":{"
        "\"url\":\"%s\"}},\"message\":{\"body\":\"Ghostcontrol\"},"
        "\"subMessage\":{\"body\":\"%s\"},\"actions\":[{"
        "\"actionName\":\"Open Ghostcontrol Toolbox\",\"actionType\":\"DeepLink\","
        "\"defaultFocus\":true,\"parameters\":{\"actionUrl\":\"GC\"}}]},"
        "\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"icon\":{"
        "\"type\":\"Predefined\",\"parameters\":{\"icon\":\"controller\"}},"
        "\"message\":{\"body\":\"Ghostcontrol: %s\"}}}}},"
        "\"createdDateTime\":\"%s\","
        "\"localNotificationId\":\"900071%llu%u\"}",
        GC_NOTIFY_ICON_URI, escaped, escaped, timestamp,
        (unsigned long long)now, notification_id);
    return length < 0 || (size_t)length >= output_size ? -1 : length;
}

int gc_notify_send(const char *detail)
{
    if (detail && strlen(detail) > 15) {
        GcNotificationRequest request;
        memset(&request, 0, sizeof(request));
        request.target_id = -1;
        snprintf(request.message, sizeof(request.message), "%s", detail);
        return sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    }

    char payload[4096];
    unsigned id = __atomic_add_fetch(&g_notification_id, 1u,
                                     __ATOMIC_RELAXED);
    if (gc_notify_build_json(payload, sizeof(payload), detail, id) >= 0 &&
        sceNotificationSend(0xfe, 1, payload) == 0)
        return 0;

    GcNotificationRequest request;
    memset(&request, 0, sizeof(request));
    request.target_id = -1;
    request.use_icon_uri = 1;
    snprintf(request.message, sizeof(request.message), "Ghostcontrol: %s",
             detail ? detail : "");
    snprintf(request.uri, sizeof(request.uri), "%s", GC_NOTIFY_ICON_URI);
    return sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

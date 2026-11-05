#include "gc_vda_klog.h"

#include <stdlib.h>
#include <string.h>

static int parse_hex_after(const char *line, const char *first,
                           const char *second, uint64_t *value)
{
    const char *text = strstr(line, first);
    const char *marker = first;
    if (!text && second) {
        text = strstr(line, second);
        marker = second;
    }
    if (!text)
        return 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text + strlen(marker), &end, 16);
    if (end == text + strlen(marker) || parsed == 0)
        return 0;
    *value = (uint64_t)parsed;
    return 1;
}

int gc_vda_klog_parse(const char *line, GcVdaKlogEvent *event)
{
    if (!line || !event)
        return 0;
    memset(event, 0, sizeof(*event));

    if (strstr(line, "DEVICE_ADDED") && strstr(line, "subType:22") &&
        strstr(line, "capabilityBattery:0") &&
        parse_hex_after(line, "DeviceId:0x", "deviceId=0x",
                        &event->device_id)) {
        event->type = GC_VDA_KLOG_ADDED;
        return 1;
    }

    if (strstr(line, "DEVICE_OWNER_CHANGED") &&
        parse_hex_after(line, "DeviceId:0x", "deviceId=0x",
                        &event->device_id)) {
        uint64_t user_id = 0;
        if (!parse_hex_after(line, "UserId:0x", "userId=0x", &user_id))
            return 0;
        event->type = GC_VDA_KLOG_OWNER_CHANGED;
        event->user_id = (int32_t)user_id;
        return 1;
    }
    return 0;
}

int32_t gc_vda_select_handle(int32_t result, const int32_t output[6],
                             int32_t sentinel, uint64_t device_id)
{
    if (device_id)
        return (int32_t)(device_id & 0xffffffffu);
    if (result > 0)
        return result;
    for (int index = 0; index < 6; ++index)
        if (output[index] != sentinel && output[index] > 0)
            return output[index];
    return result < 0 ? result : -1;
}

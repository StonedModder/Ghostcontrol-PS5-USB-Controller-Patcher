#pragma once

#include <stdint.h>

typedef enum {
    GC_VDA_KLOG_NONE = 0,
    GC_VDA_KLOG_ADDED,
    GC_VDA_KLOG_OWNER_CHANGED,
} GcVdaKlogEventType;

typedef struct {
    GcVdaKlogEventType type;
    uint64_t device_id;
    int32_t user_id;
} GcVdaKlogEvent;

int gc_vda_klog_parse(const char *line, GcVdaKlogEvent *event);
int32_t gc_vda_select_handle(int32_t result, const int32_t output[6],
                             int32_t sentinel, uint64_t device_id);

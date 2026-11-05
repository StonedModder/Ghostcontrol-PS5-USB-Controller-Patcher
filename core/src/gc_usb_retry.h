#pragma once
#include <stdint.h>
#include <string.h>

static inline int gc_usb_path_present(const char paths[][32], int count,
                                      const char *path)
{
    for (int index = 0; index < count; ++index)
        if (strcmp(paths[index], path) == 0) return 1;
    return 0;
}

static inline int gc_usb_setup_only(int has_rule)
{
    return !has_rule;
}

static inline int gc_usb_out_transfer_released(const void *buffers,
                                               const void *lengths)
{
    return !buffers && !lengths;
}

static inline int gc_usb_idle_packet_watchdog(int is_xbox_gip,
                                               int is_nintendo)
{
    return is_xbox_gip || is_nintendo;
}

static inline int gc_usb_retry_can_schedule(int auto_restart,
                                            unsigned attempts,
                                            unsigned limit)
{
    return auto_restart && attempts < limit;
}

static inline int gc_usb_retry_due(int pending, uint64_t now_ms,
                                   uint64_t retry_after_ms)
{
    return pending && now_ms >= retry_after_ms;
}

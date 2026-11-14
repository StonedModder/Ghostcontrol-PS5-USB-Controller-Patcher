/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include "gc_usb_decode_result.h"
#define GC_GH5_RAW 1
#define GC_GH5_NEUTRAL 2
#define GC_GH5_MAPPED 3
#ifndef GC_GH5_MODE
#define GC_GH5_MODE GC_GH5_MAPPED
#endif
#if GC_GH5_MODE != GC_GH5_RAW && GC_GH5_MODE != GC_GH5_NEUTRAL && GC_GH5_MODE != GC_GH5_MAPPED
#error Invalid GH5 test mode
#endif
#ifndef GC_BUILD_REVISION
#define GC_BUILD_REVISION "unknown"
#endif
#ifndef GC_BUILD_VARIANT
#define GC_BUILD_VARIANT "resident-unspecified"
#endif
static inline int gc_gh5_receiver(uint16_t vid, uint16_t pid)
{
    return vid == 0x12ba && pid == 0x0100;
}
static inline const char *gc_gh5_mode_name(void)
{
#if GC_GH5_MODE == GC_GH5_RAW
    return "raw-no-publication";
#elif GC_GH5_MODE == GC_GH5_NEUTRAL
    return "neutral-binding-probe";
#else
    return "mapped-controls";
#endif
}
static inline int gc_gh5_policy(uint16_t vid, uint16_t pid)
{
    if (!gc_gh5_receiver(vid, pid)) return -1;
#if GC_GH5_MODE == GC_GH5_RAW
    return GC_USB_DECODE_SKIPPED;
#elif GC_GH5_MODE == GC_GH5_NEUTRAL
    return GC_USB_DECODE_NEUTRAL_PROBE;
#else
    return -1;
#endif
}
static inline int gc_gh5_allows_fallback(uint16_t vid, uint16_t pid)
{
    return gc_gh5_policy(vid, pid) < 0;
}

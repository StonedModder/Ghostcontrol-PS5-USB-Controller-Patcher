/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_config.h"
#include "gc_dualsense_fx.h"
#include "ghostPad/gc_ghostpad.h"

/* Ordinary command seam: snapshots and explicitly supplied normal-API sinks.
 * No runtime discovery, platform calls, remote fallback or worker startup. */
int gc_runtime_command_get(GcConfigStore *store, const GcGhostpadStatus *ghostpad,
                           char *response, size_t size);
int gc_runtime_command_set(GcConfigStore *store, const char *argument,
                           char *response, size_t size);
int gc_runtime_command_fx_status(const GcDualSenseFxStatus *status,
                                 char *response, size_t size);
typedef int (*GcRuntimeRumbleSink)(unsigned, unsigned, unsigned, unsigned, unsigned);
int gc_runtime_command_rumble(const char *argument, GcRuntimeRumbleSink sink,
                              char *response, size_t size);

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_config.h"
#include <stddef.h>

/* Shared host/native controller-rule persistence handler.
 * CONTROLLERS: full persisted fields, including output_type and supported=false.
 * CONTROLLER_SAVE: hex(id|name|vid_hex|pid_hex|protocol|in|out|enabled|ignore
 *                      [|interface[|output_type]]).
 * CONTROLLER_OUTPUT_SET: id|ds4, id|ds5, id|psvr2_left or id|psvr2_right.
 * Type-only edits must preserve all other rule fields and verify persisted
 * readback. Replies must separate saved=true from applied=false.
 * Return 1 for unhandled command, 0 for success, -1 for handled error.
 * This module never invokes a virtual-pad backend or changes privileges. */
int gc_controller_command(GcConfigStore *store, const char *command,
                          const char *argument, char *response,
                          size_t response_size);

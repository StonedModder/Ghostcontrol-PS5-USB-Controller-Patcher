/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include "gc_bt.h"

/* Formats the live Bluetooth engine snapshot into the Toolbox status JSON.
 * The shape is unchanged from the previous standalone runtime so the
 * ShellUI Bluetooth page needs no change. */
int gc_bluetooth_format_status(const GcBtStatus *status, char *response,
                               size_t response_size);

/* Validates and canonicalises "AA:BB:CC:DD:EE:FF" (upper-case, 17 chars)
 * into out[18]. Returns 0 on success. */
int gc_bluetooth_canonical_address(const char *argument, char out[18]);

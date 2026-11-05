/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_config.h"
#include "gc_state.h"

int gc_ds4_bridge_start(GcConfigStore *config, GcState *state);
int gc_ds4_bridge_stop(void);
void gc_ds4_bridge_status(GcWirelessState *status);

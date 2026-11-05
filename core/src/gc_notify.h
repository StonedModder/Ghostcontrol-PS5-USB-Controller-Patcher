/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

#define GC_NOTIFY_ICON_URI "/user/data/ghostcontrol/i.png"

int gc_notify_build_json(char *output, size_t output_size,
                         const char *detail, unsigned notification_id);
int gc_notify_send(const char *detail);

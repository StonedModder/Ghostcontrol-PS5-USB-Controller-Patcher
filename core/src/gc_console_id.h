/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define GC_CONSOLE_ID_SERIAL_MAX 32
#define GC_CONSOLE_ID_MODEL_MAX 64
#define GC_CONSOLE_ID_HAS_SERIAL (1u << 0)
#define GC_CONSOLE_ID_HAS_MODEL (1u << 1)
#define GC_CONSOLE_ID_HAS_OPENPSID (1u << 2)
#define GC_CONSOLE_ID_HAS_OPENPSID_SYS (1u << 3)

typedef struct {
    uint32_t flags;
    char serial[GC_CONSOLE_ID_SERIAL_MAX];
    char model[GC_CONSOLE_ID_MODEL_MAX];
    uint8_t openpsid[16];
    uint8_t openpsid_sys[16];
} GcConsoleId;

int gc_console_id_read(GcConsoleId *out);
void gc_console_id_hex(const uint8_t id[16], char out[33]);

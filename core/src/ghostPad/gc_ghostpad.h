/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GC_GHOSTPAD_PORT 6967
#define GC_GHOSTPAD_STALE_MS 250
#define GC_GHOSTPAD_PACKET_SIZE 16

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
} GcGhostpadInput;

typedef struct {
    int enabled;
    int listening;
    int connected;
    uint64_t packets;
    uint64_t coalesced_packets;
    uint64_t rejected_packets;
    uint64_t stale_releases;
    int last_error;
    uint16_t port;
    unsigned stale_ms;
    char client[64];
} GcGhostpadStatus;

typedef struct {
    void *context;
    int (*connected)(void *context, const char *client);
    int (*input)(void *context, int session, const GcGhostpadInput *input);
    void (*disconnected)(void *context, int session);
} GcGhostpadOps;

typedef struct GcGhostpad GcGhostpad;

/* Returns 1 for GPAD input, 0 for DISC, and -1 for invalid input. */
int gc_ghostpad_decode(const uint8_t packet[GC_GHOSTPAD_PACKET_SIZE],
                       GcGhostpadInput *input);

GcGhostpad *gc_ghostpad_start(const GcGhostpadOps *ops);
void gc_ghostpad_configure(GcGhostpad *ghostpad, uint16_t port,
                           unsigned stale_ms);
void gc_ghostpad_set_enabled(GcGhostpad *ghostpad, int enabled);
void gc_ghostpad_snapshot(GcGhostpad *ghostpad, GcGhostpadStatus *status);
void gc_ghostpad_stop(GcGhostpad *ghostpad);

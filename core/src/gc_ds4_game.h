/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "gc_ds4_source.h"

#define GC_DS4_MAX_IMPORT_HOOKS 64u

typedef struct {
    intptr_t slot;
    intptr_t original;
    intptr_t gateway;
    uint32_t protection;
    uint32_t kind;
} GcDs4ImportHook;

typedef enum {
    GC_DS4_GAME_EMPTY = 0,
    GC_DS4_GAME_LIVE,
    GC_DS4_GAME_RETAINED_REUSABLE,
    GC_DS4_GAME_RETAINED_QUARANTINED
} GcDs4GameState;

typedef struct {
    pid_t pid;
    intptr_t process_identity;
    intptr_t mapping;
    size_t mapping_size;
    intptr_t args_address;
    uint32_t route_count;
    uint32_t hook_count;
    uint32_t publish_sequence;
    uint32_t routing_epoch;
    GcDs4Source sources[GC_DS4_MAX_ROUTES];
    GcDs4GameState state;
    GcDs4Route routes[GC_DS4_MAX_ROUTES];
    GcDs4ImportHook hooks[GC_DS4_MAX_IMPORT_HOOKS];
} GcDs4GameBridge;

typedef struct {
    GcDs4GameState state;
    pid_t pid;
    uint32_t route_count;
    uint32_t hook_count;
    uint32_t sequence;
    uint64_t deadline_ns;
    uint64_t publications;
    uint64_t calls[GC_DS4_LIBPAD_SYMBOLS];
    uint64_t lease_expirations;
    uint64_t snapshot_fallbacks;
} GcDs4GameStatus;

int gc_ds4_game_find_target(pid_t *pid);
int gc_ds4_game_install(GcDs4GameBridge *bridge,
                        const GcDs4SourceReader *source);
int gc_ds4_game_publish(GcDs4GameBridge *bridge,
                        const GcDs4SourceSnapshot *snapshot);
int gc_ds4_game_status(const GcDs4GameBridge *bridge,
                       GcDs4GameStatus *status);
int gc_ds4_game_remove(GcDs4GameBridge *bridge);
int gc_ds4_game_quiesce(GcDs4GameBridge *bridge);
int gc_ds4_game_validate(const GcDs4GameBridge *bridge, const GcDs4SourceReader *source);
void gc_ds4_game_observe_exit(GcDs4GameBridge *bridge);

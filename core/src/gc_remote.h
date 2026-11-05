/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    GC_REMOTE_DETACHED = 0,
    GC_REMOTE_STOPPED,
    GC_REMOTE_RUNNING_UNKNOWN,
    GC_REMOTE_EXITED
} GcRemoteTraceState;

typedef struct {
    pid_t pid;
    intptr_t trap_address;
    size_t trap_size;
    GcRemoteTraceState state;
    int needs_restore;
    uint64_t saved_registers[32];
} GcRemoteSession;

int gc_remote_available(void);
size_t gc_remote_find_pids(const char *name, pid_t *pids, size_t capacity);
int gc_remote_get_library(pid_t pid, const char *name, uint32_t *handle);
intptr_t gc_remote_library_base(pid_t pid, const char *name);
intptr_t gc_remote_resolve(pid_t pid, uint32_t handle, const char *symbol);

int gc_remote_read(pid_t pid, intptr_t address, void *output, size_t length);
int gc_remote_write(pid_t pid, intptr_t address, const void *input,
                    size_t length);
int gc_remote_map(pid_t pid, size_t length, intptr_t *address);
int gc_remote_protect(pid_t pid, intptr_t address, size_t length,
                      int protection);
int gc_remote_unmap(pid_t pid, intptr_t address, size_t length);

int gc_remote_session_open(GcRemoteSession *session, pid_t pid);
int gc_remote_session_call(GcRemoteSession *session, intptr_t function,
                           const uint64_t arguments[6], int64_t *result);
int gc_remote_session_close(GcRemoteSession *session);

/* Recovery hints only: callers must validate live process, code and arguments. */
typedef struct {
    uint32_t magic, version;
    int32_t pid, role;
    intptr_t process_identity, mapping;
    uint64_t mapping_size;
} GcRemoteRecord;
enum { GC_REMOTE_RECORD_SOURCE = 1, GC_REMOTE_RECORD_GAME = 2 };
int gc_remote_record_load(int role, GcRemoteRecord *record);
int gc_remote_record_save(int role, pid_t pid, intptr_t identity, intptr_t mapping, size_t size);
void gc_remote_record_clear(int role);

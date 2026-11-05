/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GC_IPC_TOKEN_HEX_SIZE 64
#define GC_IPC_MAX_REQUEST 32768
#define GC_IPC_MAX_RESPONSE 65536

typedef struct {
    char token[GC_IPC_TOKEN_HEX_SIZE + 1];
    char command[32];
    char argument[GC_IPC_MAX_REQUEST];
} GcIpcRequest;

typedef struct {
    void *context;
    int (*status)(void *context, char *response, size_t response_size);
    int (*command)(void *context, const char *command, const char *argument,
                   char *response, size_t response_size);
    void (*log)(void *context, const char *message);
} GcIpcOps;

typedef struct GcIpcServer GcIpcServer;

int gc_ipc_parse_request(const char *line, GcIpcRequest *request);
int gc_ipc_token_equal(const char *left, const char *right);
int gc_ipc_hex_encode(const void *input, size_t input_size,
                      char *output, size_t output_size);
int gc_ipc_hex_decode(const char *input, void *output, size_t output_size,
                      size_t *decoded_size);
GcIpcServer *gc_ipc_start(uint16_t port, const char *root,
                         const GcIpcOps *ops, char *error, size_t error_size);
void gc_ipc_stop(GcIpcServer *server);
/* Failure retains the server and its owned state; no successful release. */
int gc_ipc_stop_checked(GcIpcServer *server);
typedef struct GcIpcDeferredReply GcIpcDeferredReply;
#define GC_IPC_REPLY_DEFERRED 1
int gc_ipc_defer_reply(GcIpcServer *server, GcIpcDeferredReply **out);
int gc_ipc_complete_reply(GcIpcDeferredReply **reply, const char *line,
                          size_t length, unsigned timeout_ms);
void gc_ipc_discard_reply(GcIpcDeferredReply **reply);
const char *gc_ipc_token_path(const GcIpcServer *server);

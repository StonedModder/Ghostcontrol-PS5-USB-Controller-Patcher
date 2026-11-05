/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GC_CONTROLLER_INSTANCE_H
#define GC_CONTROLLER_INSTANCE_H

#include <stdint.h>

#define GC_INSTANCE_ID_MAX 24
#define GC_INSTANCE_SESSION_MAX 33
#define GC_INSTANCE_PATH_MAX 64
#define GC_INSTANCE_JSON_MAX 1024 /* includes NUL */
typedef enum { GC_INSTANCE_UNKNOWN=0, GC_INSTANCE_STABLE=1,
               GC_INSTANCE_COLLISION=2 } GcInstanceState;
typedef enum { GC_INSTANCE_PROOF_NONE=0,
               GC_INSTANCE_PROOF_CLASSIC_PEER=1 } GcInstanceProof;
typedef enum { GC_INSTANCE_TRANSPORT_UNKNOWN=0,
               GC_INSTANCE_TRANSPORT_USB=1,
               GC_INSTANCE_TRANSPORT_CLASSIC=2,
               GC_INSTANCE_TRANSPORT_LE=3 } GcInstanceTransport;
typedef struct {
    GcInstanceState state;
    GcInstanceProof proof;
    char instance_id[GC_INSTANCE_ID_MAX];
} GcControllerIdentity;
typedef struct {
    GcInstanceTransport transport;
    char session_id[GC_INSTANCE_SESSION_MAX];
    char path[GC_INSTANCE_PATH_MAX];
    uint64_t connection_generation, occurrence_generation;
    uint16_t vid, pid;
    char protocol[32];
    int interface_index, interface_number, alternate_setting;
    uint8_t in_endpoint, out_endpoint;
    GcControllerIdentity identity;
} GcControllerMatch;

#endif

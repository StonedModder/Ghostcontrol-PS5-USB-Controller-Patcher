/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Virtual-device identity policy: a refusal table, not a translation layer.
 *
 * The module answers one question for a caller that wants to create a console
 * virtual pad: may this identity be attempted on this platform, and if not, why
 * not, as a stable token the caller can log. Availability here means "our tree
 * already calls it and nothing in the evidence forbids it"; it never means the
 * identity was observed working, and no host gate can make it mean that.
 *
 * It does NOT create, delete, feed, bind or assign a device, does not call
 * libScePad or libSceMbus, has no platform conditional (no __PROSPERO__, no SDK
 * header), holds no state and allocates nothing.
 *
 * The only identity reported usable is the one the product already passes as
 * type 3 (core/src/gc_main.c:993, core/src/bt/ps5_vpad.c:461); the table makes
 * the surrounding refusals explicit where the code today simply never asks.
 *
 * Citations:
 *  - Ghostpad-main/src/main.c:204-209 - type 3 is their working DualSense /
 *    RemotePlay identity; type 0 is a classic DS4 type and
 *    scePadVirtualDeviceAddDevice answers 0x80920001 for it, so it must not be
 *    used as a default.
 *  - Ghostpad-main/README.md:304 advertises "TYPE 0 = DS4" while the only
 *    handler assigns it straight into the same call
 *    (Ghostpad-main/src/main.c:271-275, :1035-1042); the drop records no
 *    success for type 0 anywhere.
 *  - PSVR2 Sense left/right do not occur in the drop at all (no value, no
 *    report, no pairing evidence), so there is nothing to encode.
 *  - audit/ghostpad-intake/EVALUATION.md, "P2 - Refusal-first virtual-identity
 *    policy module", is the specification for this file.
 */
#pragma once

#include <stdint.h>

typedef enum {
    GC_VDA_PLATFORM_UNKNOWN = 0,
    GC_VDA_PLATFORM_PS4,   /* Orbis: the platform the drop's type-0 refusal was seen on */
    GC_VDA_PLATFORM_PS5    /* Prospero: the only platform this tree builds for */
} GcVdaPlatform;

/* This tree's persisted intent (core/src/gc_controller_type.h:5-15) is not a
 * device-type number; these are the identities those preferences refer to. */
typedef enum {
    GC_VDA_IDENTITY_NONE = 0,   /* no intent, or an unknown preference: not a device */
    GC_VDA_IDENTITY_DUALSENSE,
    GC_VDA_IDENTITY_DS4,
    GC_VDA_IDENTITY_PSVR2_LEFT,
    GC_VDA_IDENTITY_PSVR2_RIGHT,
    GC_VDA_IDENTITY_COUNT
} GcVdaIdentity;

/* Returned by gc_vda_type_value() where no type may be passed. Negative so a
 * caller that ignores the refusal still cannot hand it to AddDevice as a type. */
#define GC_VDA_TYPE_NONE (-1)

/* 1 when the pair may be attempted, 0 otherwise. */
int gc_vda_identity_available(GcVdaPlatform platform, GcVdaIdentity identity);

/* Non-NULL exactly when gc_vda_identity_available() is 0: one stable lowercase
 * token, one reason. The reason is a fact about the evidence, not a retry hint. */
const char *gc_vda_identity_refusal(GcVdaPlatform platform, GcVdaIdentity identity);

/* GcControllerOutputType -> GcVdaIdentity. Anything outside
 * [0, GC_CONTROLLER_OUTPUT_COUNT) maps to GC_VDA_IDENTITY_NONE, which no
 * platform makes available. Kept distinct from the type number because
 * core/src/gc_controller_type.h:5-7 warns that the preference is not a Sony
 * value. */
GcVdaIdentity gc_vda_identity_from_preference(int preference);

/* 3 for DualSense on PS5; GC_VDA_TYPE_NONE for every other pair. The reason is
 * in gc_vda_identity_refusal(); this prototype cannot return a string. */
int gc_vda_type_value(GcVdaPlatform platform, GcVdaIdentity identity);

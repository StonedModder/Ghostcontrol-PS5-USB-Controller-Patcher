/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Refusal table for virtual-device identities. See gc_vda_identity.h for the
 * scope, the non-goals and the citations. Pure: no platform header, no call.
 */
#include "gc_vda_identity.h"
#include "gc_controller_type.h"

#define REFUSAL_DS4            "refused_ds4_0x80920001"
#define REFUSAL_NO_EVIDENCE    "no_evidence"
#define REFUSAL_PLATFORM       "platform_not_implemented"
#define REFUSAL_PLATFORM_ANY   "platform_unknown"
#define REFUSAL_IDENTITY       "unknown_identity"

static int identity_known(GcVdaIdentity identity)
{
    return identity > GC_VDA_IDENTITY_NONE && identity < GC_VDA_IDENTITY_COUNT;
}

int gc_vda_identity_available(GcVdaPlatform platform, GcVdaIdentity identity)
{
    return platform == GC_VDA_PLATFORM_PS5 &&
           identity == GC_VDA_IDENTITY_DUALSENSE;
}

/* Checked in this order: the identity, then the platform, then the pair. So an
 * unknown identity is reported as such on every platform, and a known identity
 * on an unknown platform never falls through to a pair-specific reason. */
const char *gc_vda_identity_refusal(GcVdaPlatform platform,
                                    GcVdaIdentity identity)
{
    if (!identity_known(identity))
        return REFUSAL_IDENTITY;
    if (platform != GC_VDA_PLATFORM_PS4 && platform != GC_VDA_PLATFORM_PS5)
        return REFUSAL_PLATFORM_ANY;

    switch (identity) {
    case GC_VDA_IDENTITY_DUALSENSE:
        /* Type 3 is what both our call sites and the drop use; PS4 is not a
         * platform this tree implements, so it is refused as unimplemented
         * rather than as unevidenced. */
        return platform == GC_VDA_PLATFORM_PS5 ? NULL : REFUSAL_PLATFORM;
    case GC_VDA_IDENTITY_DS4:
        /* The drop's own comment records the refusal on their PS4 path
         * (Ghostpad-main/src/main.c:204-207); on PS5 their code only ever asks
         * for type 3, so there is no datapoint either way. */
        return platform == GC_VDA_PLATFORM_PS4 ? REFUSAL_DS4
                                               : REFUSAL_NO_EVIDENCE;
    case GC_VDA_IDENTITY_PSVR2_LEFT:
    case GC_VDA_IDENTITY_PSVR2_RIGHT:
        return REFUSAL_NO_EVIDENCE;
    case GC_VDA_IDENTITY_NONE:
    case GC_VDA_IDENTITY_COUNT:
        break;
    }
    return REFUSAL_IDENTITY;
}

GcVdaIdentity gc_vda_identity_from_preference(int preference)
{
    switch (preference) {
    case GC_CONTROLLER_OUTPUT_DS5: return GC_VDA_IDENTITY_DUALSENSE;
    case GC_CONTROLLER_OUTPUT_DS4: return GC_VDA_IDENTITY_DS4;
    case GC_CONTROLLER_OUTPUT_PSVR2_LEFT: return GC_VDA_IDENTITY_PSVR2_LEFT;
    case GC_CONTROLLER_OUTPUT_PSVR2_RIGHT: return GC_VDA_IDENTITY_PSVR2_RIGHT;
    default: return GC_VDA_IDENTITY_NONE;
    }
}

int gc_vda_type_value(GcVdaPlatform platform, GcVdaIdentity identity)
{
    if (!gc_vda_identity_available(platform, identity))
        return GC_VDA_TYPE_NONE;
    return 3;   /* Ghostpad-main/src/main.c:208, core/src/bt/ps5_vpad.c:65 */
}

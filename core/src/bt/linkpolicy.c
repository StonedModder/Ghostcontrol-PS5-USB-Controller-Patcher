/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bluetooth link policy. See linkpolicy.h.
 */
#include "linkpolicy.h"

#include <stddef.h>

int gc_bt_link_is_audio_sink(unsigned cod)
{
    if (GC_BT_COD_MAJOR(cod) != GC_BT_COD_MAJOR_AV) return 0;
    switch (GC_BT_COD_MINOR(cod)) {
    case 0x01: /* wearable headset */
    case 0x02: /* hands-free */
    case 0x05: /* loudspeaker */
    case 0x06: /* headphones */
    case 0x07: /* portable audio */
    case 0x0A: /* hi-fi audio device */
        return 1;
    default:
        /* microphone-only (0x04), car audio (0x08), set-top, camera, and every
         * other minor class: not an A2DP sink we may take. */
        return 0;
    }
}

GcBtLinkRole gc_bt_link_role(unsigned cod)
{
    if (gc_bt_link_is_audio_sink(cod)) return GC_BT_ROLE_AUDIO;
    /* Peripheral major with a gamepad/joystick minor, using the engine's own
     * convention (bt/profiles.c masks this nibble with 0x0F, not the 6-bit
     * minor field: the two must agree bit for bit). */
    if (GC_BT_COD_MAJOR(cod) == GC_BT_COD_MAJOR_PERIPHERAL) {
        const unsigned minor = ((cod) >> 2) & 0x0Fu;
        if (minor == 0x01 || minor == 0x02) return GC_BT_ROLE_PAD; /* joystick, gamepad */
    }
    return cod ? GC_BT_ROLE_OTHER : GC_BT_ROLE_NONE;
}

GcBtLinkDecision gc_bt_link_decide(const GcBtLinkRequest *request)
{
    if (!request) return GC_BT_LINK_LEAVE;
    /* The user's manual block wins over everything, including a saved target:
     * a blocked device is always the system's. */
    if (request->block == GC_BT_BLOCK_MANUAL) return GC_BT_LINK_LEAVE;
    /* Class before flags: a pad/phone/PC/TV must reach the console's stack even
     * if it is a saved target or auto-accept is on. */
    if (!gc_bt_link_is_audio_sink(request->cod)) return GC_BT_LINK_LEAVE;
    /* The device the user pressed is ours, even if we blocked it ourselves
     * after an earlier failed attempt. */
    if (request->saved_target) return GC_BT_LINK_TAKE;
    /* Our own auto block (never the user's): leave it alone until asked. */
    if (request->block == GC_BT_BLOCK_AUTO) return GC_BT_LINK_LEAVE;
    /* Allowed / never blocked: unattended audio is the user's choice. */
    return request->auto_accept_audio ? GC_BT_LINK_TAKE : GC_BT_LINK_LEAVE;
}

void gc_bt_credit_gap_add(GcBtCreditGap *gap, int64_t gap_ms)
{
    if (!gap) return;
    ++gap->samples;
    if (gap_ms <= 0 || gap_ms > GC_BT_CREDIT_GAP_MAX_MS) return; /* counted, not learned */
    if (gap_ms > gap->longest_ms) gap->longest_ms = gap_ms;
    /* The same 7/8 EWMA the published pool uses: a link's rhythm is stable, and
     * one slow gap must not redefine it. */
    gap->avg_ms = gap->avg_ms ? (gap->avg_ms * 7 + gap_ms) / 8 : gap_ms;
}

int64_t gc_bt_credit_idle_limit_ms(const GcBtCreditGap *gap)
{
    if (!gap || !gap->avg_ms) return GC_BT_CREDIT_IDLE_FALLBACK_MS;
    int64_t limit = gap->avg_ms * 2 + GC_BT_CREDIT_IDLE_FLOOR_MS;
    if (limit < GC_BT_CREDIT_IDLE_FLOOR_MS) limit = GC_BT_CREDIT_IDLE_FLOOR_MS;
    if (limit > GC_BT_CREDIT_IDLE_CAP_MS) limit = GC_BT_CREDIT_IDLE_CAP_MS;
    return limit;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bluetooth link policy: who may own a link, and how long a link may look idle
 * before we stop trusting it.
 *
 * Two independent lessons from the two published A2DP implementations this
 * project documents (docs/BT-WIRELESS-LESSONS.md has the citations):
 *
 *  1. A connection request is arbitrated by Class of Device BEFORE anything
 *     else, and a device that is not an audio sink is never taken -- a pad, a
 *     phone, a PC or a TV must always reach the console's own stack. The
 *     published sources go further than "check the class": they keep three
 *     block kinds (manual, auto, allowed) so a device the stack blocked
 *     automatically can be allowed by the user and never re-blocked.
 *
 *  2. "Buffers are free" is not something elapsed time can prove (both sources
 *     say so), so a credit fallback must not be a constant wall clock. The
 *     published pool learns THIS link's rhythm from the gaps between its own
 *     credit events and derives the idle limit from that average.
 *
 * Both are pure decisions on purpose: no radio, no locks, no IO. The engine
 * calls them; the host tests pin them.
 */
#ifndef GC_BT_LINKPOLICY_H
#define GC_BT_LINKPOLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Class of Device: 24 bits, major service/device classes. */
#define GC_BT_COD_MAJOR(cod) (((cod) >> 8) & 0x1Fu)
#define GC_BT_COD_MINOR(cod) (((cod) >> 2) & 0x3Fu)
#define GC_BT_COD_MAJOR_AV 0x04u  /* Audio/Video */
#define GC_BT_COD_MAJOR_PERIPHERAL 0x05u /* keyboard, pointing, gamepad */

/* What we are allowed to do with an incoming request. */
typedef enum {
    GC_BT_LINK_LEAVE = 0, /* not ours: leave it to the console's own stack */
    GC_BT_LINK_TAKE = 1   /* ours to own (the engine still applies its own
                           * "already owned" / "radio busy" rules) */
} GcBtLinkDecision;

/* How the user's blocklist knows this address. */
typedef enum {
    GC_BT_BLOCK_NONE = 0,    /* never blocked */
    GC_BT_BLOCK_MANUAL = 1,  /* the user blocked it: always the system's */
    GC_BT_BLOCK_AUTO = 2,    /* we blocked it ourselves (a failed attempt) */
    GC_BT_BLOCK_ALLOWED = 3  /* the user un-blocked an auto entry: never auto
                              * added again, and not blocked */
} GcBtBlockKind;

typedef struct {
    unsigned cod;              /* the requester's Class of Device */
    int saved_target;          /* the user picked this device in the UI */
    int auto_accept_audio;     /* the user allows unattended audio sinks */
    GcBtBlockKind block;
} GcBtLinkRequest;

/* 1 only for devices that can be an A2DP sink: Audio/Video major and an audio
 * minor class. A microphone-only device, a car head unit, a gamepad, a phone,
 * a PC or a TV is not one. */
int gc_bt_link_is_audio_sink(unsigned cod);

/* The role this class can play for us. The engine already has the pad half of
 * this question (bt/profiles.c, profile_class_is_gamepad) and this module has
 * the audio half; this enum exists so the two can be cross-checked and can never
 * both claim the same device. */
typedef enum {
    GC_BT_ROLE_NONE = 0,
    GC_BT_ROLE_PAD = 1,   /* Peripheral major, gamepad/joystick minor */
    GC_BT_ROLE_AUDIO = 2, /* A2DP sink: Audio/Video major, audio minor */
    GC_BT_ROLE_OTHER = 3  /* a real device class we must leave alone */
} GcBtLinkRole;

GcBtLinkRole gc_bt_link_role(unsigned cod);

/* The decision, in this order: manual block, class, saved target, auto block,
 * auto-accept. Never TAKE for a non-audio device, whatever the flags say. */
GcBtLinkDecision gc_bt_link_decide(const GcBtLinkRequest *request);

/* The observed rhythm of a link's credit events. */
typedef struct {
    uint32_t samples;    /* credit events folded in */
    int64_t avg_ms;      /* EWMA of the gap between them */
    int64_t longest_ms;  /* the worst gap seen, for diagnostics */
} GcBtCreditGap;

/* Folds one inter-credit gap in. A non-positive or absurd gap (> 2 s: a
 * suspend, a stall, a wrap) is counted as a sample but does not move the
 * average -- one pathological gap must not teach the link a wrong rhythm. */
void gc_bt_credit_gap_add(GcBtCreditGap *gap, int64_t gap_ms);

/* Idle limit for a link, in ms: with no samples, the conservative fallback;
 * with samples, twice the learned average plus a small constant, clamped so a
 * fast link cannot go below the floor and a slow one cannot exceed the cap. */
#define GC_BT_CREDIT_GAP_MAX_MS 2000
#define GC_BT_CREDIT_IDLE_FALLBACK_MS 200
#define GC_BT_CREDIT_IDLE_FLOOR_MS 30
#define GC_BT_CREDIT_IDLE_CAP_MS 500
int64_t gc_bt_credit_idle_limit_ms(const GcBtCreditGap *gap);

#ifdef __cplusplus
}
#endif

#endif

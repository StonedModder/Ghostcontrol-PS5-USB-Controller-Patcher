/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Per-handle link epochs (BT-WIRELESS-LESSONS.md item 6, from
 * HearBridge-PS5 src/bt/acl_track.c): every connection gets a handle AND a
 * fresh epoch, and an event that arrives afterwards only counts for the link we
 * own now if it carries the current epoch. A handle is always new -- unlike a
 * Bluetooth address, which repeats across reconnects -- so handle+epoch is the
 * cheapest way to say "this event belongs to this link, not the one that used
 * this handle two minutes ago".
 *
 * Pure: no radio, no locks, no allocation. The engine owns the table; the
 * caller owns the locking. Slots are never silently reused: a connect into an
 * occupied slot is refused and counted, because a clobbered slot would attribute
 * a live link's events to a dead one.
 */
#ifndef GC_BT_HANDLE_EPOCH_H
#define GC_BT_HANDLE_EPOCH_H

#include <stddef.h>
#include <stdint.h>

#define GC_BT_HANDLE_SLOTS 8
/* Recently released links, remembered only to classify a late event as stale
 * (we dropped that link) rather than foreign (we never owned it). Bounded: the
 * oldest entry is forgotten, after which such an event reads as foreign. */
#define GC_BT_HANDLE_RECENT 8

typedef struct {
    uint16_t handle;
    uint32_t epoch;      /* 0 while the slot is empty; monotonically increasing */
    unsigned char addr[6];
    int used;
} GcBtHandleSlot;

typedef struct {
    uint32_t next_epoch;
    unsigned assigns;            /* connects that got a slot */
    unsigned refused_busy;       /* the address already owned a link */
    unsigned refused_full;       /* no free slot: refused rather than clobbered */
    unsigned disconnects;
    unsigned unmatched_disconnects;
    unsigned stale_epoch_events; /* right handle, or a recently released one, superseded link */
    unsigned foreign_handle_events; /* a handle we have never assigned */
    unsigned recent_next;        /* next ring position; the ring wraps */
    uint16_t recent_handle[GC_BT_HANDLE_RECENT];
    uint32_t recent_epoch[GC_BT_HANDLE_RECENT]; /* 0 = empty ring entry */
    GcBtHandleSlot slots[GC_BT_HANDLE_SLOTS];
} GcBtHandleTable;

void gc_bt_handles_init(GcBtHandleTable *table);

/* Claims a slot for addr at handle and returns the new epoch in *epoch_out
 * (which is never 0). Returns 0 on success, <0 when the address already owns a
 * link or when the table is full -- in both cases nothing is modified. */
int gc_bt_handles_connect(GcBtHandleTable *table, const unsigned char addr[6],
                          uint16_t handle, uint32_t *epoch_out);

/* 1 when handle+epoch is the link that owns that handle right now. A handle for
 * a link that has been disconnected is never owned, whatever epoch is asked
 * about. Mismatches are counted by kind (stale vs foreign). */
int gc_bt_handles_owns(GcBtHandleTable *table, uint16_t handle, uint32_t epoch);

/* Frees the handle's slot and bumps its epoch out of reach. An unknown handle is
 * counted, not ignored. */
void gc_bt_handles_disconnect(GcBtHandleTable *table, uint16_t handle);

/* The current epoch for a handle, 0 when the handle is not owned. */
uint32_t gc_bt_handles_epoch_of(const GcBtHandleTable *table, uint16_t handle);

/* Fills addr_out (6 bytes) with the address a handle belongs to. Returns 0 on
 * success, <0 when the handle is unknown. */
int gc_bt_handles_addr_of(const GcBtHandleTable *table, uint16_t handle,
                          unsigned char addr_out[6]);

#endif /* GC_BT_HANDLE_EPOCH_H */

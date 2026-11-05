/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "handle_epoch.h"

#include <string.h>

static GcBtHandleSlot *find_handle(GcBtHandleTable *t, uint16_t handle)
{
    for (int i = 0; i < GC_BT_HANDLE_SLOTS; ++i) {
        if (t->slots[i].used && t->slots[i].handle == handle) return &t->slots[i];
    }
    return NULL;
}

static GcBtHandleSlot *find_addr(GcBtHandleTable *t, const unsigned char addr[6])
{
    for (int i = 0; i < GC_BT_HANDLE_SLOTS; ++i) {
        if (t->slots[i].used && memcmp(t->slots[i].addr, addr, 6) == 0) return &t->slots[i];
    }
    return NULL;
}

static GcBtHandleSlot *free_slot(GcBtHandleTable *t)
{
    for (int i = 0; i < GC_BT_HANDLE_SLOTS; ++i) {
        if (!t->slots[i].used) return &t->slots[i];
    }
    return NULL;
}

void gc_bt_handles_init(GcBtHandleTable *table)
{
    memset(table, 0, sizeof(*table));
    table->next_epoch = 1; /* 0 means "no link", so epochs start at 1 */
}

int gc_bt_handles_connect(GcBtHandleTable *table, const unsigned char addr[6],
                          uint16_t handle, uint32_t *epoch_out)
{
    if (find_addr(table, addr)) {
        ++table->refused_busy;
        return -1;
    }
    GcBtHandleSlot *slot = free_slot(table);
    if (!slot) {
        ++table->refused_full;
        return -1;
    }
    slot->used = 1;
    slot->handle = handle;
    slot->epoch = table->next_epoch++;
    if (slot->epoch == 0) slot->epoch = table->next_epoch++; /* never hand out 0 */
    memcpy(slot->addr, addr, 6);
    ++table->assigns;
    if (epoch_out) *epoch_out = slot->epoch;
    return 0;
}

int gc_bt_handles_owns(GcBtHandleTable *table, uint16_t handle, uint32_t epoch)
{
    GcBtHandleSlot *slot = find_handle(table, handle);
    if (!slot || slot->epoch != epoch) {
        /* Classify the mismatch: a handle that is live under another epoch, or
         * one we released recently, is a late packet from a link we had; a
         * handle we have never seen is somebody else's traffic. */
        for (int i = 0; i < GC_BT_HANDLE_SLOTS; ++i) {
            if (table->slots[i].used && table->slots[i].handle == handle) {
                ++table->stale_epoch_events;
                return 0;
            }
        }
        for (int i = 0; i < GC_BT_HANDLE_RECENT; ++i) {
            if (table->recent_epoch[i] != 0 && table->recent_handle[i] == handle) {
                ++table->stale_epoch_events;
                return 0;
            }
        }
        ++table->foreign_handle_events;
        return 0;
    }
    return 1;
}

void gc_bt_handles_disconnect(GcBtHandleTable *table, uint16_t handle)
{
    GcBtHandleSlot *slot = find_handle(table, handle);
    if (!slot) {
        ++table->unmatched_disconnects;
        return;
    }
    /* Remember the release before the slot is reusable, so a packet still in
     * flight for the old link classifies as stale rather than foreign. */
    const unsigned idx = table->recent_next % GC_BT_HANDLE_RECENT;
    table->recent_handle[idx] = slot->handle;
    table->recent_epoch[idx] = slot->epoch;
    ++table->recent_next;
    /* Take the epoch out of reach before the slot can be handed to anything
     * else, so a packet still in flight for the old link can never match. */
    slot->epoch = 0;
    slot->handle = 0;
    slot->used = 0;
    memset(slot->addr, 0, 6);
    ++table->disconnects;
}

uint32_t gc_bt_handles_epoch_of(const GcBtHandleTable *table, uint16_t handle)
{
    for (int i = 0; i < GC_BT_HANDLE_SLOTS; ++i) {
        if (table->slots[i].used && table->slots[i].handle == handle) return table->slots[i].epoch;
    }
    return 0;
}

int gc_bt_handles_addr_of(const GcBtHandleTable *table, uint16_t handle,
                          unsigned char addr_out[6])
{
    for (int i = 0; i < GC_BT_HANDLE_SLOTS; ++i) {
        if (table->slots[i].used && table->slots[i].handle == handle) {
            memcpy(addr_out, table->slots[i].addr, 6);
            return 0;
        }
    }
    return -1;
}

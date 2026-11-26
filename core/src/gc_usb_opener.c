/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_usb_opener.h"

#include <errno.h>

static int opener_keep_running(void *context)
{
    GcUsbOpener *opener = context;
    return !atomic_load_explicit(&opener->cancel, memory_order_acquire);
}

static void *opener_thread(void *context)
{
    GcUsbOpener *opener = context;
    opener->result = opener->work(opener->argument, opener_keep_running, opener);
    atomic_store_explicit(&opener->state, GC_USB_OPENER_DONE, memory_order_release);
    return NULL;
}

int gc_usb_opener_start(GcUsbOpener *opener, GcUsbOpenerWork work, void *argument)
{
    if (!opener || !work) return EINVAL;
    if (gc_usb_opener_active(opener)) return EBUSY;
    opener->work = work;
    opener->argument = argument;
    opener->result = 0;
    atomic_store_explicit(&opener->cancel, 0, memory_order_relaxed);
    atomic_store_explicit(&opener->state, GC_USB_OPENER_RUNNING, memory_order_release);
    const int rc = pthread_create(&opener->thread, NULL, opener_thread, opener);
    if (rc != 0)
        atomic_store_explicit(&opener->state, GC_USB_OPENER_IDLE, memory_order_release);
    return rc;
}

static int opener_reap(GcUsbOpener *opener)
{
    (void)pthread_join(opener->thread, NULL);
    atomic_store_explicit(&opener->state, GC_USB_OPENER_IDLE, memory_order_release);
    return opener->result;
}

int gc_usb_opener_poll(GcUsbOpener *opener, int *result)
{
    if (!opener) return -1;
    const int state = atomic_load_explicit(&opener->state, memory_order_acquire);
    if (state == GC_USB_OPENER_IDLE) return -1;
    if (state == GC_USB_OPENER_RUNNING) return 0;
    const int value = opener_reap(opener);
    if (result) *result = value;
    return 1;
}

int gc_usb_opener_cancel(GcUsbOpener *opener)
{
    if (!gc_usb_opener_active(opener)) return 0;
    atomic_store_explicit(&opener->cancel, 1, memory_order_release);
    return opener_reap(opener);
}

int gc_usb_opener_active(const GcUsbOpener *opener)
{
    return opener &&
           atomic_load_explicit(&opener->state, memory_order_acquire) != GC_USB_OPENER_IDLE;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <pthread.h>
#include <stdatomic.h>

/*
 * Runs one blocking device start-up (for example the Xbox GIP handshake) on a
 * worker thread, so the caller's tick never waits on it and never holds a
 * shared lock across it. The caller owns the device handle and must not touch
 * it while the opener is running; it asks with gc_usb_opener_poll() each pass.
 *
 * The work function receives a keep-running callback that turns false once
 * gc_usb_opener_cancel() is called; work that checks it ends early.
 */
typedef int (*GcUsbOpenerKeepRunning)(void *context);
typedef int (*GcUsbOpenerWork)(void *argument, GcUsbOpenerKeepRunning keep_running,
                               void *context);

enum {
    GC_USB_OPENER_IDLE = 0,
    GC_USB_OPENER_RUNNING = 1,
    GC_USB_OPENER_DONE = 2,
};

typedef struct {
    pthread_t thread;
    atomic_int state;
    atomic_int cancel;
    int result;
    GcUsbOpenerWork work;
    void *argument;
} GcUsbOpener;

/* Starts work(argument, ...) on a new thread. Returns 0, or an errno value
 * when the thread could not be created (the opener stays idle). */
int gc_usb_opener_start(GcUsbOpener *opener, GcUsbOpenerWork work, void *argument);

/* Never blocks on the work. Returns 0 while it runs. Once it has finished,
 * reaps the thread, stores the work's return value in *result, returns 1 and
 * leaves the opener idle. Returns -1 when nothing was started. */
int gc_usb_opener_poll(GcUsbOpener *opener, int *result);

/* Asks running work to stop, waits for its thread and leaves the opener idle.
 * The wait lasts as long as the work takes to notice; it is a no-op when the
 * opener is idle. Returns the work's result, or 0 when nothing was running. */
int gc_usb_opener_cancel(GcUsbOpener *opener);

/* 1 while work is started and not yet reaped. */
int gc_usb_opener_active(const GcUsbOpener *opener);

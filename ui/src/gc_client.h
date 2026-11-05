/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Client for the Ghostcontrol controller service.
 *
 * The app finds the companion service by itself: it connects to the service's
 * local port and sends "GC1 <command>". There is no token to read and no
 * file to find, so nothing here depends on reaching another process's data
 * directory from inside an app sandbox.
 *
 * The UI never drives the pad hardware; it asks the service. That split is
 * what lets the UI be an ordinary homebrew app with no ShellUI injection and
 * no firmware-specific patching anywhere in it.
 *
 * Replies are one line. Status replies are JSON, command replies are a short
 * acknowledgement, and config replies are INI-shaped; the caller knows which
 * it asked for.
 */
#ifndef GC_CLIENT_H
#define GC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* The app is C++ and this client is C, so the names must not be
 * mangled by the translation unit that includes it. */
#ifdef __cplusplus
extern "C" {
#endif

/* The native service accepts only local console peers on this port. */
#ifndef GC_CLIENT_PORT
#define GC_CLIENT_PORT 39073
#endif

/* The per-exchange deadline every call gets unless the caller names one, in
 * milliseconds. Most commands answer in well under a second, so three seconds
 * leaves room for a busy service while still bounding a UI thread that waits
 * on one. It is not a ceiling on a command's work: a caller that knows a
 * command is slow (UPDATE_CHECK reaches the network, LOG_TAIL walks the log)
 * passes its own budget to gc_client_send_timeout rather than having a
 * legitimately slow command misreported as a timeout. */
#define GC_CLIENT_DEFAULT_TIMEOUT_MS 3000
#define GC_CLIENT_COMMAND_MAX 64
#define GC_CLIENT_ARGUMENT_MAX 16384

typedef enum {
    GC_CLIENT_OK = 0,
    /** Nothing is listening: the service is not running. */
    GC_CLIENT_NO_SERVICE,
    /** Connected, but the exchange failed or timed out. */
    GC_CLIENT_IO_ERROR,
    /** The command or argument was rejected before being sent. */
    GC_CLIENT_BAD_REQUEST,
    /** The reply did not fit the supplied buffer. */
    GC_CLIENT_TRUNCATED
} GcClientResult;

/**
 * Points the client at a different port.
 *
 * The service's port is configurable (`[ipc] port`), so the app has to be
 * able to follow it rather than assuming the default forever.
 */
void gc_client_set_port(unsigned short port);
/* Host-order IPv4. Configure before polling; updates are safe during retries. */
void gc_client_set_address(uint32_t address);
uint32_t gc_client_address(void);

/** The port the client will use. */
unsigned short gc_client_port(void);

/** Human-readable form of a result, for logs and on-screen errors. */
const char *gc_client_result_text(GcClientResult result);

/** errno from the last failed connect to the service, or 0.
 *
 *  GC_CLIENT_NO_SERVICE on its own cannot distinguish a service that is not
 *  running from an address this build cannot reach; this says which. */
int gc_client_last_connect_errno(void);

/**
 * Sends one command and returns its reply, with a caller-chosen deadline.
 *
 * `argument` may be NULL. `reply` is always NUL-terminated on GC_CLIENT_OK,
 * with the trailing newline removed. `reply_length` may be NULL.
 *
 * `timeout_ms` bounds the entire exchange: connect, send and all
 * receive waits together. 0 means GC_CLIENT_DEFAULT_TIMEOUT_MS, not "wait forever" --
 * an unbounded wait from a UI thread is exactly the freeze this timeout
 * exists to prevent. A command that can genuinely take longer than the
 * default passes its own budget here.
 *
 * `reply_size` must be large enough for the reply. Status and command
 * replies are small, but a log or list reply (LOG_TAIL, PROFILES,
 * CONTROLLERS) can run past 12 KiB, so size a buffer for those at 16 KiB or
 * more; the app's poll buffer is 32 KiB. A buffer too small for its reply
 * returns GC_CLIENT_TRUNCATED rather than silently clipping.
 */
GcClientResult gc_client_send_timeout(const char *command, const char *argument,
                                      char *reply, size_t reply_size,
                                      size_t *reply_length,
                                      unsigned timeout_ms);

/** Request-local cancellation, checked within 50ms; callback must be thread-safe.
 * Deadline bounds the entire connect/send/receive exchange, not each byte. */
typedef int (*GcClientCancel)(void *context);
GcClientResult gc_client_send_cancelable(const char *command, const char *argument,
    char *reply, size_t reply_size, size_t *reply_length, unsigned timeout_ms,
    GcClientCancel cancel, void *context);

/**
 * Sends one command and returns its reply, using the default deadline
 * (GC_CLIENT_DEFAULT_TIMEOUT_MS).
 *
 * A thin wrapper over gc_client_send_timeout for the common, quick command.
 */
GcClientResult gc_client_send(const char *command, const char *argument,
                              char *reply, size_t reply_size,
                              size_t *reply_length);

/** True when the service answers STATUS. Cheap enough to poll on a screen
 *  transition, too expensive to call every frame. */
int gc_client_service_is_up(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GC_CLIENT_H */

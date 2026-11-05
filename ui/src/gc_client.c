/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "gc_client.h"
#include <stdio.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* The service drops a connection that stalls, so the client sets its own
 * deadline rather than blocking a UI thread indefinitely. The constant lives
 * in the header so callers can name it; gc_client_send passes it and
 * gc_client_send_timeout passes whatever the caller gave, converting to the
 * timeval the socket options want. A command slower than the default is a
 * caller's cue to pass a longer budget, not to guess a bigger global. */
#define GC_CLIENT_REQUEST_MAX (GC_CLIENT_ARGUMENT_MAX + GC_CLIENT_COMMAND_MAX + 8)

/* Set once at startup if the service is on a non-default port. Plain
 * assignment is enough: the app reads config before it starts querying. */
static unsigned short g_port = GC_CLIENT_PORT;
#ifdef __PROSPERO__
static atomic_uint g_address = ATOMIC_VAR_INIT(0);
#else
static atomic_uint g_address = ATOMIC_VAR_INIT(INADDR_LOOPBACK);
#endif

void gc_client_set_address(uint32_t address)
{
    atomic_store(&g_address, address);
}

uint32_t gc_client_address(void)
{
    return atomic_load(&g_address);
}

void gc_client_set_port(unsigned short port)
{
    if (port != 0)
        g_port = port;
}

unsigned short gc_client_port(void)
{
    return g_port;
}

const char *gc_client_result_text(GcClientResult result)
{
    switch (result) {
    case GC_CLIENT_OK:          return "ok";
    case GC_CLIENT_NO_SERVICE:  return "Ghosttooth API not running";
    case GC_CLIENT_IO_ERROR:    return "Ghosttooth API stopped responding";
    case GC_CLIENT_BAD_REQUEST: return "invalid request";
    case GC_CLIENT_TRUNCATED:   return "reply too large";
    default:                    return "unknown error";
    }
}

/* The last connect(2) failure, for the app's own log.
 *
 * "controller service not running" was all the app ever learned, which is
 * true but useless: a refused connect, an unroutable address and a missing
 * loopback all produce the same answer. Recording errno lets the app say
 * which, so a build that cannot reach its own service can be diagnosed from
 * a log instead of guessed at. */
/* Per thread, like errno itself: the snapshot worker and the companion
 * lifecycle worker each read the result of their own last connect. */
static _Thread_local int g_connect_errno = 0;

int gc_client_last_connect_errno(void)
{
    return g_connect_errno;
}

/* Rejects anything that could inject a second request line or confuse the
 * service's parser. Done before connecting so a bad caller costs nothing. */
static int field_is_safe(const char *text, size_t limit)
{
    if (!text)
        return 0;
    const size_t length = strlen(text);
    if (length == 0 || length > limit)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

typedef struct {
    uint64_t end_ms;
    GcClientCancel cancel;
    void *context;
} Deadline;

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Nonblocking sockets plus one monotonic deadline bound the whole exchange,
 * including trickled replies. Cancellation is per request, never global. */
static int wait_ready(int fd, short events, const Deadline *deadline)
{
    for (;;) {
        if (deadline->cancel && deadline->cancel(deadline->context)) return -1;
        uint64_t now = now_ms();
        if (now >= deadline->end_ms) return -1;
        uint64_t left = deadline->end_ms - now;
        int span = (int)(left > 50 ? 50 : left);
        struct pollfd p = {fd, events, 0};
        int result = poll(&p, 1, span);
        if (result > 0) return (p.revents & (events | POLLHUP)) ? 0 : -1;
        if (result < 0 && errno != EINTR) return -1;
    }
}

static int connect_to_service(const Deadline *deadline)
{
    const uint32_t address_value = atomic_load(&g_address);
    if (!address_value) { g_connect_errno = ENETUNREACH; return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { g_connect_errno = errno; return -1; }
#ifdef __PROSPERO__
    /* libc fcntl does not reliably set nonblocking on this console. 0x1200 is
     * the socket option a working companion client uses instead. */
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, 0x1200, &enabled, sizeof(enabled)) != 0) {
        g_connect_errno = errno; close(fd); return -1;
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        g_connect_errno = errno; close(fd); return -1;
    }
#endif
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_port = htons(g_port);
#ifdef __PROSPERO__
    address.sin_len = sizeof(address);
#endif
    address.sin_addr.s_addr = htonl(address_value);
    g_connect_errno = 0;
    /* CONNECT CONTRACT -- this is the invariant a regression broke.
     *
     * connect() returning 0 means the socket is connected, whatever the
     * socket's blocking mode. On this console a non-blocking connect to a
     * live loopback listener can return 0 immediately instead of reporting
     * EINPROGRESS; an earlier version of this function folded that case into
     * the EINPROGRESS checks and reported the service as absent. The app then
     * logged "controller service not running" forever while a raw probe
     * connected to the very same port in the same run.
     *
     * So: a zero return is success, unconditionally, and must never reach the
     * errno branches. A host cannot reproduce the console's convention -- on
     * Linux a non-blocking connect always returns EINPROGRESS -- which is why
     * this is a written invariant rather than a test. Do not restructure these
     * branches without re-reading it. */
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
        return fd;
    /* Past here connect() genuinely failed to connect immediately. */
    const int started = errno;
    if (started != EINPROGRESS) {
        /* Refused or unreachable: connect reported the reason itself. */
        g_connect_errno = started;
        close(fd); return -1;
    }
    /* In flight: wait for writability, then read the authoritative error.
     * errno here belongs to whatever ran last, so the socket error is the
     * only honest thing to report. */
    if (wait_ready(fd, POLLOUT, deadline) != 0) {
        int pending = 0; socklen_t size = sizeof(pending);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending, &size) != 0)
            pending = errno;
        g_connect_errno = pending ? pending : ETIMEDOUT;
        close(fd); return -1;
    }
    int error = 0; socklen_t size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) error = errno;
    if (error) {
        g_connect_errno = error;
        close(fd); return -1;
    }
    return fd;
}

GcClientResult gc_client_send_cancelable(const char *command, const char *argument,
    char *reply, size_t reply_size, size_t *reply_length, unsigned timeout_ms,
    GcClientCancel cancel, void *context)
{
    if (reply_length) *reply_length = 0;
    if (!reply || reply_size == 0) return GC_CLIENT_BAD_REQUEST;
    reply[0] = '\0';
    if (!field_is_safe(command, GC_CLIENT_COMMAND_MAX) ||
        (argument && !field_is_safe(argument, GC_CLIENT_ARGUMENT_MAX))) return GC_CLIENT_BAD_REQUEST;
    char request[GC_CLIENT_REQUEST_MAX];
    int written = argument && *argument ? snprintf(request, sizeof(request), "GC1 %s %s\n", command, argument) :
        snprintf(request, sizeof(request), "GC1 %s\n", command);
    if (written < 0 || (size_t)written >= sizeof(request)) return GC_CLIENT_BAD_REQUEST;
    Deadline deadline = {now_ms() + (timeout_ms ? timeout_ms : GC_CLIENT_DEFAULT_TIMEOUT_MS), cancel, context};
    int fd = connect_to_service(&deadline);
    if (fd < 0) return GC_CLIENT_NO_SERVICE;
    GcClientResult result = GC_CLIENT_IO_ERROR;
    size_t sent = 0, used = 0;
    while (sent < (size_t)written) {
        if (wait_ready(fd, POLLOUT, &deadline) != 0) goto done;
        ssize_t count = send(fd, request + sent, (size_t)written - sent, MSG_NOSIGNAL);
        if (count > 0) sent += (size_t)count;
        else if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else goto done;
    }
    while (used + 1 < reply_size) {
        if (wait_ready(fd, POLLIN, &deadline) != 0) goto done;
        ssize_t count = recv(fd, reply + used, reply_size - used - 1, 0);
        if (count > 0) {
            // Embedded NUL is not a valid one-line response.
            if (memchr(reply + used, '\0', (size_t)count)) goto done;
            used += (size_t)count; reply[used] = '\0';
            char *newline = memchr(reply, '\n', used);
            if (newline) {
                used = (size_t)(newline - reply);
                if (used && reply[used - 1] == '\r') --used;
                reply[used] = '\0';
                if (!used) goto done;
                if (reply_length) *reply_length = used;
                result = GC_CLIENT_OK; goto done;
            }
        } else if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else goto done; // EOF before newline is an incomplete exchange.
    }
    result = GC_CLIENT_TRUNCATED;
done:
    close(fd);
    if (result != GC_CLIENT_OK) reply[0] = '\0';
    return result;
}

GcClientResult gc_client_send_timeout(const char *command, const char *argument,
    char *reply, size_t reply_size, size_t *reply_length, unsigned timeout_ms)
{
    return gc_client_send_cancelable(command, argument, reply, reply_size, reply_length,
                                    timeout_ms, NULL, NULL);
}

/* The quick-command wrapper: same exchange, the default deadline. */
GcClientResult gc_client_send(const char *command, const char *argument,
                              char *reply, size_t reply_size,
                              size_t *reply_length)
{
    return gc_client_send_timeout(command, argument, reply, reply_size,
                                  reply_length, 0);
}

int gc_client_service_is_up(void)
{
    char reply[256];
    return gc_client_send("STATUS", NULL, reply, sizeof(reply), NULL) ==
           GC_CLIENT_OK;
}

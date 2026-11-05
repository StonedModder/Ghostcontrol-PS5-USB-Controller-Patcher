/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "gc_ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GC_IPC_REQUEST_DEADLINE_MS 3000u
/* Handlers that may run at once. The accept loop never serves inline, so this
 * is what keeps a pile-up of stuck requests bounded instead of unbounded. */
#define GC_IPC_MAX_HANDLERS 4u
/* Per-handler stack: the request and response framing in handle_client does not
 * fit the platform's default thread stack (the accept thread already sets this
 * size for the same reason). */
#define GC_IPC_HANDLER_STACK (256u * 1024u)

static atomic_uint g_token_temp_sequence = ATOMIC_VAR_INIT(0);

struct GcIpcServer {
    int socket_fd;
    int client_fds[GC_IPC_MAX_HANDLERS];
    atomic_int release_failed;
    atomic_int stop;
    atomic_int handlers;
    pthread_mutex_t client_lock;
    int client_lock_initialized;
    pthread_t thread;
    int thread_started;
    char token[GC_IPC_TOKEN_HEX_SIZE + 1];
    char token_path[512];
    GcIpcOps ops;
};

/* Request-local authority: no other handler or caller can transfer this fd. */
struct client_job {
    GcIpcServer *server;
    int client;
    unsigned slot;
    int callback_active;
};
static _Thread_local struct client_job *current_request;
struct GcIpcDeferredReply { int fd; };

static char *skip_space(char *value)
{
    while (*value == ' ' || *value == '\t')
        ++value;
    return value;
}

static char *next_word(char *value)
{
    while (*value && *value != ' ' && *value != '\t')
        ++value;
    if (*value)
        *value++ = '\0';
    return skip_space(value);
}

/* True only for a word of exactly token shape: 64 hex characters followed by
 * a separator or the end of the line. Used to tell "GC1 <token> <command>"
 * from "GC1 <command>" without guessing.
 *
 * This reads the word in place, before next_word() has terminated it, so it
 * measures up to the separator itself rather than calling strlen. */
static int word_is_token(const char *word)
{
    if (!word)
        return 0;
    for (size_t i = 0; i < GC_IPC_TOKEN_HEX_SIZE; ++i) {
        const char c = word[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    const char after = word[GC_IPC_TOKEN_HEX_SIZE];
    return after == '\0' || after == ' ' || after == '\t';
}

int gc_ipc_parse_request(const char *line, GcIpcRequest *request)
{
    if (!line || !request || strlen(line) >= GC_IPC_MAX_REQUEST)
        return -1;
    char buffer[GC_IPC_MAX_REQUEST];
    snprintf(buffer, sizeof(buffer), "%s", line);
    char *end = strpbrk(buffer, (const char[]){13, 10, 0});
    if (end)
        *end = '\0';
    char *version = skip_space(buffer);

    /* Two request shapes are accepted:
     *
     *   GC1 <token> <command> [argument]   a client holding the token
     *   GC1 <command> [argument]           a local client that does not
     *
     * The companion app uses the second form: it finds the service by
     * connecting to the loopback port, with no file to read out of the
     * service's data directory first.
     *
     * They are told apart by shape. A token is exactly 64 hex characters,
     * and no command is, so a word of that shape in that position can only
     * be a token -- commands are short uppercase words and anything 32
     * characters or longer is rejected below regardless. */
    /* next_word terminates the word it is given and returns the next one, so
     * the protocol word can only be compared after this call. */
    char *second = next_word(version);
    if (strcmp(version, "GC1") != 0)
        return -1;
    char *token = NULL;
    char *command = NULL;
    if (word_is_token(second)) {
        token = second;
        command = next_word(token);
    } else {
        command = second;
    }
    char *argument = next_word(command);
    if (!*command || strlen(command) >= sizeof(request->command))
        return -1;

    memset(request, 0, sizeof(*request));
    if (token)
        snprintf(request->token, sizeof(request->token), "%s", token);
    snprintf(request->command, sizeof(request->command), "%s", command);
    snprintf(request->argument, sizeof(request->argument), "%s", argument);
    return 0;
}

int gc_ipc_token_equal(const char *left, const char *right)
{
    if (!left || !right)
        return 0;
    unsigned difference = 0;
    for (size_t i = 0; i < GC_IPC_TOKEN_HEX_SIZE; ++i)
        difference |= (unsigned char)left[i] ^ (unsigned char)right[i];
    difference |= (unsigned char)left[GC_IPC_TOKEN_HEX_SIZE];
    difference |= (unsigned char)right[GC_IPC_TOKEN_HEX_SIZE];
    return difference == 0;
}

int gc_ipc_hex_encode(const void *input, size_t input_size,
                      char *output, size_t output_size)
{
    static const char hex[] = "0123456789abcdef";
    if ((!input && input_size) || !output || output_size == 0 ||
        input_size > (output_size - 1) / 2)
        return -1;
    const unsigned char *bytes = input;
    for (size_t i = 0; i < input_size; ++i) {
        output[i * 2] = hex[bytes[i] >> 4];
        output[i * 2 + 1] = hex[bytes[i] & 15];
    }
    output[input_size * 2] = '\0';
    return 0;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

int gc_ipc_hex_decode(const char *input, void *output, size_t output_size,
                      size_t *decoded_size)
{
    if (!input || !output)
        return -1;
    size_t length = strlen(input);
    if ((length & 1u) || length / 2 > output_size)
        return -1;
    unsigned char *bytes = output;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_value(input[i]);
        int low = hex_value(input[i + 1]);
        if (high < 0 || low < 0)
            return -1;
        bytes[i / 2] = (unsigned char)((high << 4) | low);
    }
    if (decoded_size)
        *decoded_size = length / 2;
    return 0;
}

static int write_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t count = send(fd, data, size, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        data += count;
        size -= (size_t)count;
    }
    return 0;
}

static int write_file_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t count = write(fd, data, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        data += count;
        size -= (size_t)count;
    }
    return 0;
}

static int read_exact(int fd, void *data, size_t size)
{
    unsigned char *cursor = data;
    while (size) {
        ssize_t count = read(fd, cursor, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        cursor += count;
        size -= (size_t)count;
    }
    return 0;
}

static void sync_token_parent(const char *path)
{
    char parent[512];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return;
    size_t length = (size_t)(slash - path);
    if (length >= sizeof(parent))
        return;
    memcpy(parent, path, length);
    parent[length] = '\0';
    int directory = open(parent, O_RDONLY | O_NOFOLLOW);
    if (directory >= 0) {
        (void)fsync(directory);
        (void)close(directory);
    }
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

int gc_ipc_defer_reply(GcIpcServer *server, GcIpcDeferredReply **out)
{
    struct client_job *job = current_request;
    if (!out || *out || !server || !job || job->server != server ||
        !job->callback_active || job->client < 0) return -1;
    GcIpcDeferredReply *reply = malloc(sizeof(*reply));
    if (!reply) return -1;
    if (pthread_mutex_lock(&server->client_lock)) { free(reply); return -1; }
    if (atomic_load(&server->stop) || job->client < 0 ||
        server->client_fds[job->slot] != job->client) {
        pthread_mutex_unlock(&server->client_lock); free(reply); return -1;
    }
    reply->fd = job->client;
    job->client = -1;
    server->client_fds[job->slot] = -1;
    *out = reply;
    /* Handoff is irrevocable even if unlocking fails. */
    if (pthread_mutex_unlock(&server->client_lock)) atomic_store(&server->release_failed, 1);
    return 0;
}

void gc_ipc_discard_reply(GcIpcDeferredReply **owner)
{
    if (!owner || !*owner) return;
    GcIpcDeferredReply *reply = *owner;
    *owner = NULL;
    (void)close(reply->fd); /* never retry ambiguous close on a reused FD */
    free(reply);
}

int gc_ipc_complete_reply(GcIpcDeferredReply **owner, const char *line,
                          size_t length, unsigned timeout_ms)
{
    if (!owner || !*owner) return -1;
    GcIpcDeferredReply *reply = *owner;
    *owner = NULL;
    int rc = -1;
    uint64_t start = monotonic_ms();
    if (!line || !length || length > GC_IPC_MAX_RESPONSE || !timeout_ms ||
        !start || line[length - 1] != '\n' ||
        memchr(line, '\n', length - 1) || memchr(line, 13, length) ||
        memchr(line, 0, length)) goto done;
    if (timeout_ms > 3000u) timeout_ms = 3000u;
    const uint64_t end = start + timeout_ms;
#ifdef __PROSPERO__
    int nonblocking = 1;
    if (setsockopt(reply->fd, SOL_SOCKET, 0x1200, &nonblocking, sizeof(nonblocking))) goto done;
#else
    int flags = fcntl(reply->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(reply->fd, F_SETFL, flags | O_NONBLOCK)) goto done;
#endif
    size_t used = 0;
    while (used < length) {
        uint64_t now = monotonic_ms();
        if (!now || now >= end) goto done;
        struct pollfd fd = {reply->fd, POLLOUT, 0};
        int ready = poll(&fd, 1, (int)(end - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(fd.revents & POLLOUT) ||
            (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) goto done;
        now = monotonic_ms();
        if (!now || now >= end) goto done;
        ssize_t count = send(reply->fd, line + used, length - used, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) goto done;
        used += (size_t)count;
    }
    rc = 0;
done:
    if (close(reply->fd)) rc = -1;
    free(reply);
    return rc;
}

static void handle_client(GcIpcServer *server, int client)
{
    char input[GC_IPC_MAX_REQUEST];
    size_t used = 0;
    const uint64_t started_ms = monotonic_ms();
    const uint64_t deadline_ms = started_ms + GC_IPC_REQUEST_DEADLINE_MS;
    while (used + 1 < sizeof(input)) {
        if (atomic_load(&server->stop))
            return;
        uint64_t now_ms = monotonic_ms();
        if (!started_ms || !now_ms || now_ms >= deadline_ms)
            return;
        uint64_t remaining_ms = deadline_ms - now_ms;
        struct pollfd descriptor = {client, POLLIN, 0};
        int wait_ms = (int)(remaining_ms > 100u ? 100u : remaining_ms);
        int ready = poll(&descriptor, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            return;
        if (!(descriptor.revents & POLLIN))
            continue;
        ssize_t count = recv(client, input + used, sizeof(input) - used - 1, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return;
        used += (size_t)count;
        if (memchr(input, '\n', used))
            break;
    }
    input[used] = '\0';
    if (server->ops.log) server->ops.log(server->ops.context, "ipc: handle_client read line");
    GcIpcRequest request;
    char response[GC_IPC_MAX_RESPONSE] = "";
    int result = gc_ipc_parse_request(input, &request);
    if (result != 0) {
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"error\":\"bad_request\"}\n");
    } else if (request.token[0] != '\0' &&
               !gc_ipc_token_equal(request.token, server->token)) {
        /* A request that supplies a token is still held to it, so a stale or
         * wrong token is reported rather than quietly ignored. A request that
         * supplies none is a local client: default builds bind loopback,
         * and LAN builds validate the peer against the local interface. */
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"error\":\"unauthorized\"}\n");
    } else if (strcmp(request.command, "RESIDENT_EXIT_IF") == 0 &&
               (!used || input[used - 1] != '\n' || memchr(input, '\n', used - 1) ||
                memchr(input, 13, used) || memchr(input, 0, used))) {
        /* The additive exact-instance argument cannot be shortened by legacy
         * CR/NUL line parsing into an apparently valid identity request. */
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"error\":\"invalid_resident_exit_request\"}\n");
    } else if (strcmp(request.command, "STATUS") == 0) {
        result = server->ops.status(server->ops.context, response,
                                    sizeof(response));
        if (result != 0)
            snprintf(response, sizeof(response),
                     "{\"ok\":false,\"error\":\"status_failed\"}\n");
    } else {
        current_request->callback_active = 1;
        result = server->ops.command(server->ops.context, request.command,
                                     request.argument, response,
                                     sizeof(response));
        current_request->callback_active = 0;
        /* A callback return code alone NEVER transfers ownership. */
        if (current_request->client < 0) return;
        if (result != 0 && response[0] == '\0')
            snprintf(response, sizeof(response),
                     "{\"ok\":false,\"error\":\"command_failed\"}\n");
    }
    if (server->ops.log) server->ops.log(server->ops.context, "ipc: wrote response");
    (void)write_all(client, response, strlen(response));
}

/* Detached workers retain the server and borrowed callback state until the
 * final release-store decrement. Shutdown must observe zero before freeing. */
static void *client_worker(void *argument)
{
    struct client_job *job = argument;
    GcIpcServer *server = job->server;
    current_request = job;
    if (!atomic_load(&server->stop)) handle_client(server, job->client);
    if (pthread_mutex_lock(&server->client_lock)) {
        atomic_store(&server->release_failed, 1);
        return NULL; /* retain handler count and all borrowed state */
    }
    if (job->client >= 0) {
        int fd = job->client;
        job->client = -1;
        server->client_fds[job->slot] = -1;
        if (close(fd)) atomic_store(&server->release_failed, 1);
    }
    if (pthread_mutex_unlock(&server->client_lock)) atomic_store(&server->release_failed, 1);
    current_request = NULL;
    free(job);
    atomic_fetch_sub(&server->handlers, 1);
    return NULL;
}

static void *server_thread(void *argument)
{
    GcIpcServer *server = argument;
    while (!atomic_load(&server->stop)) {
        struct pollfd descriptor = {server->socket_fd, POLLIN, 0};
        int ready = poll(&descriptor, 1, 250);
        if (atomic_load(&server->stop)) break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            /* Poll itself failed: this is a console runtime difference worth logging. */
            fprintf(stderr, "ipc: poll error errno=%d\n", errno);
            continue;
        }
        if (ready == 0) continue;
        if (!(descriptor.revents & POLLIN)) continue;
        struct sockaddr_in peer = {0};
        socklen_t peer_size = sizeof(peer);
        int client = accept(server->socket_fd, (struct sockaddr *)&peer, &peer_size);
        if (client < 0) {
            fprintf(stderr, "ipc: accept error errno=%d\n", errno);
            continue;
        }
#ifdef GC_IPC_BIND_LAN
        /* The title uses its console's LAN address because loopback TCP is
           refused. Accept only that console, as Hyperion's companion does. */
        struct sockaddr_in self = {0};
        socklen_t self_size = sizeof(self);
        if (peer_size != sizeof(peer) || peer.sin_family != AF_INET ||
            getsockname(client, (struct sockaddr *)&self, &self_size) != 0 ||
            self_size != sizeof(self) ||
            (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) &&
             peer.sin_addr.s_addr != self.sin_addr.s_addr)) {
            close(client);
            continue;
        }
#endif
        if (server->ops.log) server->ops.log(server->ops.context, "ipc: accepted client");
        struct timeval timeout = {2, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        /* Hand the socket to a worker; never serve inline (see client_worker). */
        if (atomic_load(&server->handlers) >= (int)GC_IPC_MAX_HANDLERS) {
            if (server->ops.log) server->ops.log(server->ops.context, "ipc: busy, refusing a client");
            close(client);
            continue;
        }
        struct client_job *job = calloc(1, sizeof(*job));
        if (!job) {
            close(client);
            continue;
        }
        job->server = server;
        job->client = client;
        pthread_mutex_lock(&server->client_lock);
        unsigned slot;
        for (slot = 0; slot < GC_IPC_MAX_HANDLERS; ++slot)
            if (server->client_fds[slot] < 0) break;
        if (slot == GC_IPC_MAX_HANDLERS || atomic_load(&server->stop)) {
            pthread_mutex_unlock(&server->client_lock); free(job); close(client); continue;
        }
        job->slot = slot;
        server->client_fds[slot] = client;
        atomic_fetch_add(&server->handlers, 1);
        pthread_mutex_unlock(&server->client_lock);
        pthread_attr_t worker_attributes;
        int attr_rc = pthread_attr_init(&worker_attributes);
        int worker_result = attr_rc;
        if (!worker_result) worker_result = pthread_attr_setdetachstate(&worker_attributes, PTHREAD_CREATE_DETACHED);
        /* Same reason the accept thread gets an explicit stack: the request and
         * response buffers in handle_client exceed the platform's default thread
         * stack. Without this the worker overflows its stack and the process
         * dies - the host state test caught it immediately under ASan. */
        if (!worker_result) worker_result = pthread_attr_setstacksize(&worker_attributes, GC_IPC_HANDLER_STACK);
        pthread_t worker;
        if (!worker_result) worker_result = pthread_create(&worker, &worker_attributes, client_worker, job);
        if (!attr_rc) pthread_attr_destroy(&worker_attributes);
        if (worker_result != 0) {
            pthread_mutex_lock(&server->client_lock);
            server->client_fds[slot] = -1;
            pthread_mutex_unlock(&server->client_lock);
            atomic_fetch_sub(&server->handlers, 1);
            free(job);
            if (server->ops.log) server->ops.log(server->ops.context, "ipc: worker start failed");
            close(client);
            continue;
        }
        continue;   /* the worker owns the socket from here on */
    }
    return NULL;
}

static int load_or_create_token(GcIpcServer *server)
{
    int fd = open(server->token_path, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat info;
        int valid_file = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                         info.st_nlink == 1 &&
                         (info.st_mode & 077u) == 0 &&
                         info.st_size == GC_IPC_TOKEN_HEX_SIZE;
        int token_read = valid_file
                             ? read_exact(fd, server->token,
                                          GC_IPC_TOKEN_HEX_SIZE)
                             : -1;
        close(fd);
        if (token_read == 0) {
            server->token[GC_IPC_TOKEN_HEX_SIZE] = '\0';
            GcIpcRequest check;
            char line[GC_IPC_MAX_REQUEST];
            snprintf(line, sizeof(line), "GC1 %s PING", server->token);
            if (gc_ipc_parse_request(line, &check) == 0)
                return 0;
        }
    }

    unsigned char random[GC_IPC_TOKEN_HEX_SIZE / 2];
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read_exact(fd, random, sizeof(random)) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    for (size_t i = 0; i < sizeof(random); ++i)
        snprintf(server->token + i * 2, 3, "%02x", random[i]);
    server->token[GC_IPC_TOKEN_HEX_SIZE] = '\0';

    char temporary[576];
    unsigned sequence = atomic_fetch_add(&g_token_temp_sequence, 1) + 1;
    int count = snprintf(temporary, sizeof(temporary), "%s.tmp.%d.%u",
                         server->token_path, (int)getpid(), sequence);
    if (count < 0 || (size_t)count >= sizeof(temporary))
        return -1;
    fd = open(temporary,
              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    int saved = 0;
    struct stat temporary_info;
    if (fstat(fd, &temporary_info) != 0 ||
        !S_ISREG(temporary_info.st_mode) || temporary_info.st_nlink != 1)
        saved = errno ? errno : EIO;
    else if (write_file_all(fd, server->token,
                            GC_IPC_TOKEN_HEX_SIZE) != 0)
        saved = errno ? errno : EIO;
    else if (fsync(fd) != 0)
        saved = errno;
    if (close(fd) != 0 && !saved)
        saved = errno;
    fd = -1;
    if (!saved && rename(temporary, server->token_path) != 0)
        saved = errno;
    if (saved) {
        unlink(temporary);
        errno = saved;
        return -1;
    }
    sync_token_parent(server->token_path);
    return 0;
}

GcIpcServer *gc_ipc_start(uint16_t port, const char *root,
                         const GcIpcOps *ops, char *error, size_t error_size)
{
    if (!port || !root || !ops || !ops->status || !ops->command)
        return NULL;
    GcIpcServer *server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    server->socket_fd = -1;
    for (unsigned i = 0; i < GC_IPC_MAX_HANDLERS; ++i) server->client_fds[i] = -1;
    atomic_init(&server->handlers, 0);
    atomic_init(&server->release_failed, 0);
    atomic_init(&server->stop, 0);
    int lock_result = pthread_mutex_init(&server->client_lock, NULL);
    if (lock_result != 0) {
        if (error && error_size)
            snprintf(error, error_size, "IPC lock failed: %s",
                     strerror(lock_result));
        free(server);
        return NULL;
    }
    server->client_lock_initialized = 1;
    server->ops = *ops;
    int count = snprintf(server->token_path, sizeof(server->token_path),
                         "%s/runtime/ipc.token", root);
    if (count < 0 || (size_t)count >= sizeof(server->token_path) ||
        load_or_create_token(server) != 0) {
        if (error && error_size)
            snprintf(error, error_size, "IPC token failed: %s", strerror(errno));
        gc_ipc_stop(server);
        return NULL;
    }
    server->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    if (server->socket_fd < 0 ||
        setsockopt(server->socket_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) != 0) {
        if (error && error_size)
            snprintf(error, error_size, "IPC socket failed: %s", strerror(errno));
        gc_ipc_stop(server);
        return NULL;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
#ifdef __PROSPERO__
    address.sin_len = sizeof(address);
#endif
#ifdef GC_IPC_BIND_LAN
    address.sin_addr.s_addr = htonl(INADDR_ANY);
#else
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
    pthread_attr_t attributes;
    int attr_result = pthread_attr_init(&attributes);
    /* IPC framing + profile serialization exceed PS5's 64 KiB default stack. */
    int thread_result = attr_result ? attr_result : pthread_attr_setstacksize(&attributes, 256 * 1024);
    if (!thread_result && (bind(server->socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->socket_fd, 4) != 0)) thread_result = errno;
    if (!thread_result) thread_result = pthread_create(&server->thread, &attributes, server_thread, server);
    if (!attr_result) pthread_attr_destroy(&attributes);
    if (thread_result != 0) {
        if (error && error_size)
            snprintf(error, error_size, "IPC listen failed on port %u: %s",
                     (unsigned)port, strerror(thread_result));
        gc_ipc_stop(server);
        return NULL;
    }
    server->thread_started = 1;
    return server;
}

int gc_ipc_stop_checked(GcIpcServer *server)
{
    if (!server) return 0;
    if (current_request && current_request->server == server) return -1;
    atomic_store(&server->stop, 1);
    /* shutdown wakes IO; successful close+join are the release witnesses. */
    if (server->socket_fd >= 0) (void)shutdown(server->socket_fd, SHUT_RDWR);
    if (server->client_lock_initialized) {
        if (pthread_mutex_lock(&server->client_lock)) return -1;
        for (unsigned i = 0; i < GC_IPC_MAX_HANDLERS; ++i)
            if (server->client_fds[i] >= 0) (void)shutdown(server->client_fds[i], SHUT_RDWR);
        if (pthread_mutex_unlock(&server->client_lock)) return -1;
    }
    /* No hard deadline is claimed for a platform join. A blocked join retains
     * original state/lease; the consumer independently times out without push. */
    if (server->thread_started) {
        if (pthread_join(server->thread, NULL)) return -1;
        server->thread_started = 0;
    }
    for (unsigned waited = 0; atomic_load(&server->handlers) && waited < 3000; waited += 25)
        usleep(25000);
    if (atomic_load(&server->handlers) || atomic_load(&server->release_failed)) return -1;
    if (server->socket_fd >= 0) {
        int fd = server->socket_fd;
        server->socket_fd = -1; /* never retry ambiguous close */
        if (close(fd)) { atomic_store(&server->release_failed, 1); return -1; }
    }
    if (server->client_lock_initialized && pthread_mutex_destroy(&server->client_lock)) return -1;
    free(server);
    return 0;
}
void gc_ipc_stop(GcIpcServer *server) { (void)gc_ipc_stop_checked(server); }

const char *gc_ipc_token_path(const GcIpcServer *server)
{
    return server ? server->token_path : NULL;
}

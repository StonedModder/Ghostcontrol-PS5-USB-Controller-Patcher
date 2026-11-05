/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "gc_ghostpad.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define GC_GHOSTPAD_BUTTON_MASK 0x0011ffffu

struct GcGhostpad {
    GcGhostpadOps ops;
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_int enabled;
    atomic_int stopping;
    int listen_fd;
    int client_fd;
    uint16_t port;
    unsigned stale_ms;
    GcGhostpadStatus status;
};

int gc_ghostpad_decode(const uint8_t packet[GC_GHOSTPAD_PACKET_SIZE],
                       GcGhostpadInput *input)
{
    if (!packet || !input)
        return -1;
    if (memcmp(packet, "DISC", 4) == 0)
        return 0;
    if (memcmp(packet, "GPAD", 4) != 0 || packet[14] || packet[15])
        return -1;
    const uint32_t buttons = ((uint32_t)packet[4] << 24) |
                             ((uint32_t)packet[5] << 16) |
                             ((uint32_t)packet[6] << 8) | packet[7];
    if (buttons & ~GC_GHOSTPAD_BUTTON_MASK)
        return -1;
    input->buttons = buttons;
    input->lx = packet[8];
    input->ly = packet[9];
    input->rx = packet[10];
    input->ry = packet[11];
    input->l2 = packet[12];
    input->r2 = packet[13];
    return 1;
}

static void set_fds(GcGhostpad *ghostpad, int listen_fd, int client_fd)
{
    pthread_mutex_lock(&ghostpad->lock);
    ghostpad->listen_fd = listen_fd;
    ghostpad->client_fd = client_fd;
    ghostpad->status.listening = listen_fd >= 0;
    ghostpad->status.connected = client_fd >= 0;
    if (client_fd < 0)
        ghostpad->status.client[0] = '\0';
    pthread_mutex_unlock(&ghostpad->lock);
}

static void clear_client(GcGhostpad *ghostpad)
{
    pthread_mutex_lock(&ghostpad->lock);
    ghostpad->client_fd = -1;
    ghostpad->status.connected = 0;
    ghostpad->status.client[0] = '\0';
    pthread_mutex_unlock(&ghostpad->lock);
}

/* 0=packet, 1=idle timeout before any bytes, -1=closed/error/partial frame. */
static int recv_packet(GcGhostpad *ghostpad, int fd, uint8_t *packet)
{
    size_t used = 0;
    while (used < GC_GHOSTPAD_PACKET_SIZE &&
           atomic_load(&ghostpad->enabled) &&
           !atomic_load(&ghostpad->stopping)) {
        ssize_t count = recv(fd, packet + used,
                             GC_GHOSTPAD_PACKET_SIZE - used, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return used == 0 ? 1 : -1;
        if (count <= 0)
            return -1;
        used += (size_t)count;
    }
    return used == GC_GHOSTPAD_PACKET_SIZE ? 0 : -1;
}

static int open_listener(GcGhostpad *ghostpad)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                             &yes, sizeof(yes)) != 0)
        goto failed;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    pthread_mutex_lock(&ghostpad->lock);
    uint16_t port = ghostpad->port;
    pthread_mutex_unlock(&ghostpad->lock);
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0)
        goto failed;
    set_fds(ghostpad, fd, -1);
    pthread_mutex_lock(&ghostpad->lock);
    ghostpad->status.port = port;
    ghostpad->status.last_error = 0;
    pthread_mutex_unlock(&ghostpad->lock);
    return fd;
failed:
    pthread_mutex_lock(&ghostpad->lock);
    ghostpad->status.last_error = errno;
    pthread_mutex_unlock(&ghostpad->lock);
    if (fd >= 0)
        close(fd);
    return -1;
}

static void handle_client(GcGhostpad *ghostpad, int fd,
                          const struct sockaddr_in *address)
{
    char client[64] = "LAN client";
    (void)inet_ntop(AF_INET, &address->sin_addr, client, sizeof(client));
    int yes = 1;
    pthread_mutex_lock(&ghostpad->lock);
    unsigned stale_ms = ghostpad->stale_ms;
    pthread_mutex_unlock(&ghostpad->lock);
    struct timeval timeout = {
        (time_t)(stale_ms / 1000u),
        (suseconds_t)((stale_ms % 1000u) * 1000u)
    };
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    pthread_mutex_lock(&ghostpad->lock);
    ghostpad->client_fd = fd;
    ghostpad->status.connected = 1;
    snprintf(ghostpad->status.client, sizeof(ghostpad->status.client),
             "%s", client);
    pthread_mutex_unlock(&ghostpad->lock);

    int session = ghostpad->ops.connected
                      ? ghostpad->ops.connected(ghostpad->ops.context, client)
                      : -1;
    if (session < 0)
        goto finished;

    uint8_t packet[GC_GHOSTPAD_PACKET_SIZE];
    int active_input = 0;
    while (atomic_load(&ghostpad->enabled) &&
           !atomic_load(&ghostpad->stopping)) {
        int received = recv_packet(ghostpad, fd, packet);
        if (received < 0)
            break;
        if (received > 0) {
            if (active_input) {
                GcGhostpadInput neutral = {0, 128, 128, 128, 128, 0, 0};
                if (ghostpad->ops.input(ghostpad->ops.context, session,
                                        &neutral) != 0)
                    break;
                pthread_mutex_lock(&ghostpad->lock);
                ++ghostpad->status.stale_releases;
                pthread_mutex_unlock(&ghostpad->lock);
                active_input = 0;
            }
            continue;
        }

        /* GhostPad's no-lag path is a latest-state mailbox. Mirror that
         * behavior here: when TCP has queued complete frames, discard stale
         * intermediates and inject only the newest state. */
        int available = 0;
        while (ioctl(fd, FIONREAD, &available) == 0 &&
               available >= GC_GHOSTPAD_PACKET_SIZE) {
            uint8_t latest[GC_GHOSTPAD_PACKET_SIZE];
            ssize_t count = recv(fd, latest, sizeof(latest), MSG_WAITALL);
            if (count != (ssize_t)sizeof(latest))
                goto finished_session;
            memcpy(packet, latest, sizeof(packet));
            pthread_mutex_lock(&ghostpad->lock);
            ++ghostpad->status.coalesced_packets;
            pthread_mutex_unlock(&ghostpad->lock);
        }

        GcGhostpadInput input;
        int decoded = gc_ghostpad_decode(packet, &input);
        if (decoded == 0)
            break;
        if (decoded < 0) {
            pthread_mutex_lock(&ghostpad->lock);
            ++ghostpad->status.rejected_packets;
            ghostpad->status.last_error = EPROTO;
            pthread_mutex_unlock(&ghostpad->lock);
            break;
        }
        pthread_mutex_lock(&ghostpad->lock);
        ++ghostpad->status.packets;
        pthread_mutex_unlock(&ghostpad->lock);
        active_input = input.buttons != 0 || input.lx != 128 ||
                       input.ly != 128 || input.rx != 128 ||
                       input.ry != 128 || input.l2 != 0 || input.r2 != 0;
        if (!ghostpad->ops.input ||
            ghostpad->ops.input(ghostpad->ops.context, session, &input) != 0)
            break;
    }
finished_session:
    if (ghostpad->ops.disconnected)
        ghostpad->ops.disconnected(ghostpad->ops.context, session);

finished:
    close(fd);
    clear_client(ghostpad);
}

static void *server_thread(void *argument)
{
    GcGhostpad *ghostpad = argument;
    while (!atomic_load(&ghostpad->stopping)) {
        if (!atomic_load(&ghostpad->enabled)) {
            usleep(100000);
            continue;
        }
        int listener = open_listener(ghostpad);
        if (listener < 0) {
            usleep(500000);
            continue;
        }
        while (atomic_load(&ghostpad->enabled) &&
               !atomic_load(&ghostpad->stopping)) {
            struct sockaddr_in address;
            socklen_t length = sizeof(address);
            int client = accept(listener, (struct sockaddr *)&address, &length);
            if (client < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            handle_client(ghostpad, client, &address);
        }
        close(listener);
        set_fds(ghostpad, -1, -1);
    }
    return NULL;
}

GcGhostpad *gc_ghostpad_start(const GcGhostpadOps *ops)
{
    if (!ops || !ops->connected || !ops->input || !ops->disconnected)
        return NULL;
    GcGhostpad *ghostpad = calloc(1, sizeof(*ghostpad));
    if (!ghostpad)
        return NULL;
    ghostpad->ops = *ops;
    ghostpad->listen_fd = -1;
    ghostpad->client_fd = -1;
    ghostpad->port = GC_GHOSTPAD_PORT;
    ghostpad->stale_ms = GC_GHOSTPAD_STALE_MS;
    ghostpad->status.port = GC_GHOSTPAD_PORT;
    ghostpad->status.stale_ms = GC_GHOSTPAD_STALE_MS;
    atomic_init(&ghostpad->enabled, 0);
    atomic_init(&ghostpad->stopping, 0);
    if (pthread_mutex_init(&ghostpad->lock, NULL) != 0) {
        free(ghostpad);
        return NULL;
    }
    if (pthread_create(&ghostpad->thread, NULL, server_thread, ghostpad) != 0) {
        pthread_mutex_destroy(&ghostpad->lock);
        free(ghostpad);
        return NULL;
    }
    return ghostpad;
}

void gc_ghostpad_configure(GcGhostpad *ghostpad, uint16_t port,
                           unsigned stale_ms)
{
    if (!ghostpad || port == 0 || stale_ms == 0)
        return;
    pthread_mutex_lock(&ghostpad->lock);
    int changed = ghostpad->port != port || ghostpad->stale_ms != stale_ms;
    ghostpad->port = port;
    ghostpad->stale_ms = stale_ms;
    ghostpad->status.port = port;
    ghostpad->status.stale_ms = stale_ms;
    int listener = ghostpad->listen_fd;
    int client = ghostpad->client_fd;
    pthread_mutex_unlock(&ghostpad->lock);
    if (changed) {
        if (client >= 0)
            shutdown(client, SHUT_RDWR);
        if (listener >= 0)
            shutdown(listener, SHUT_RDWR);
    }
}

void gc_ghostpad_set_enabled(GcGhostpad *ghostpad, int enabled)
{
    if (!ghostpad)
        return;
    atomic_store(&ghostpad->enabled, enabled != 0);
    pthread_mutex_lock(&ghostpad->lock);
    ghostpad->status.enabled = enabled != 0;
    int listener = ghostpad->listen_fd;
    int client = ghostpad->client_fd;
    pthread_mutex_unlock(&ghostpad->lock);
    if (!enabled) {
        if (client >= 0)
            shutdown(client, SHUT_RDWR);
        if (listener >= 0)
            shutdown(listener, SHUT_RDWR);
    }
}

void gc_ghostpad_snapshot(GcGhostpad *ghostpad, GcGhostpadStatus *status)
{
    if (!status)
        return;
    memset(status, 0, sizeof(*status));
    if (!ghostpad)
        return;
    pthread_mutex_lock(&ghostpad->lock);
    *status = ghostpad->status;
    pthread_mutex_unlock(&ghostpad->lock);
}

void gc_ghostpad_stop(GcGhostpad *ghostpad)
{
    if (!ghostpad)
        return;
    atomic_store(&ghostpad->stopping, 1);
    gc_ghostpad_set_enabled(ghostpad, 0);
    pthread_join(ghostpad->thread, NULL);
    pthread_mutex_destroy(&ghostpad->lock);
    free(ghostpad);
}

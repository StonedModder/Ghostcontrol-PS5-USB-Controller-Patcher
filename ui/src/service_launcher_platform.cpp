/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The two halves that only exist on the console: reading the packaged
 * companion out of /app0, and pushing it to the loader on loopback.
 */
#include "service_launcher_platform.hpp"

#include "app_log.hpp"
#include "core/save_file.hpp"
#include "gc_client.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <cstdio>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace ghostcontrol
{
namespace
{
std::string discovered_loader_host;

#if !defined(GC_EXISTING_SERVICE_ONLY) || !GC_EXISTING_SERVICE_ONLY
bool read_packaged_asset(const void *, std::string_view path, std::string *out)
{
    return hui::save::read_file(std::string(path), out);
}

std::uint64_t monotonic_ms()
{
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

/* One absolute deadline and one cancel check shared by every stage. Waits in
 * slices of at most 50 ms so cancellation is noticed promptly. Returns 1 when
 * ready, 0 on deadline, -1 on cancel, -2 on a poll error. */
int wait_for(int handle, short events, std::uint64_t end_ms, const LaunchControl &control)
{
    for (;;)
    {
        if (control.cancel && control.cancel(control.cancel_ctx)) return -1;
        const std::uint64_t now = monotonic_ms();
        if (now >= end_ms) return 0;
        const std::uint64_t left = end_ms - now;
        pollfd waiter{handle, events, 0};
        const int ready = ::poll(&waiter, 1, static_cast<int>(left > 50 ? 50 : left));
        if (ready > 0) return (waiter.revents & POLLNVAL) ? -2 : 1;
        if (ready < 0 && errno != EINTR) return -2;
    }
}

bool make_nonblocking(int handle)
{
#ifdef __PROSPERO__
    /* Same switch gc_client uses: libc fcntl is unreliable on this console. */
    int enabled = 1;
    return ::setsockopt(handle, SOL_SOCKET, 0x1200, &enabled, sizeof(enabled)) == 0;
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    return flags >= 0 && ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/* APP-SVC-03 bounded push. connect, every partial write, shutdown and the
 * short reply wait all share one absolute deadline; cancellation is polled
 * throughout. Once any byte has left, a failure is "partial" (outcome
 * unknown), never "nothing was sent". */
int push_to_loader_bounded(void *, const char *host, unsigned port, const std::string &payload,
                           const LaunchControl &control, std::string *detail,
                           std::size_t *transferred)
{
    const std::uint64_t end_ms = monotonic_ms() + control.deadline_ms;
    std::size_t written = 0;
    if (transferred) *transferred = 0;
    app_log("push_to_loader host=%s port=%u payload=%zu bytes deadline=%ums", host, port,
            payload.size(), control.deadline_ms);
    int handle = -1;
    const auto finish = [&](int code, const std::string &reason) {
        if (handle >= 0) ::close(handle);
        if (detail && !reason.empty()) *detail = reason;
        if (transferred) *transferred = written;
        return code;
    };

    const auto interrupted = [&]() {
        if (control.cancel && control.cancel(control.cancel_ctx)) return -1;
        return monotonic_ms() >= end_ms ? 0 : 1;
    };
    const auto abort_transfer = [&](int state) {
        return finish(written ? kLoaderPartial : (state == -1 ? kLoaderCancelled : kLoaderNotSent),
                      state == -1 ? "Cancelled during loader transport; transferred bytes cannot be retracted."
                                  : "The loader transport deadline expired.");
    };

    sockaddr_in address{};
    address.sin_family = AF_INET;
#ifdef __PROSPERO__
    address.sin_len = sizeof(address);
#endif
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (::inet_pton(AF_INET, host, &address.sin_addr) != 1)
        return finish(kLoaderNotSent, "The console loader address is not valid.");
    if (control.cancel && control.cancel(control.cancel_ctx))
        return finish(kLoaderCancelled, "Cancelled before connecting to the loader.");

    if (const int state = interrupted(); state != 1) return abort_transfer(state);
    handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0) return finish(kLoaderNotSent, "Could not create a socket for the console loader.");
    int one = 1;
    ::setsockopt(handle, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one));
    if (!make_nonblocking(handle))
        return finish(kLoaderNotSent, "Could not make the loader socket nonblocking.");

    /* connect() == 0 is connected, whatever the blocking mode: this console
     * can complete a nonblocking connect immediately (see gc_client.c). */
    if (const int state = interrupted(); state != 1) return abort_transfer(state);
    if (::connect(handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        if (errno != EINPROGRESS)
        {
            const std::string reason = std::strerror(errno);
            app_log("companion: loader connect %s:%u failed errno=%d (%s)", host, port, errno,
                    reason.c_str());
            return finish(kLoaderNotSent, "The console loader is not running: " + reason);
        }
        const int ready = wait_for(handle, POLLOUT, end_ms, control);
        if (ready < 0 && ready != -2)
            return finish(kLoaderCancelled, "Cancelled while connecting to the loader.");
        if (ready == 0) return finish(kLoaderNotSent, "The console loader did not accept a connection in time.");
        int error = 0;
        socklen_t size = sizeof(error);
        if (ready == -2 || ::getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)
        {
            const std::string reason = std::strerror(error ? error : errno);
            return finish(kLoaderNotSent, "The console loader is not running: " + reason);
        }
    }
    app_log("companion: loader connect %s:%u succeeded", host, port);

    while (written < payload.size())
    {
        // Even successful short writes and EINTR consume the same budget.
        if (const int state = interrupted(); state != 1) return abort_transfer(state);
        const ssize_t n = ::send(handle, payload.data() + written, payload.size() - written,
                                 MSG_NOSIGNAL);
        if (n > 0)
        {
            written += static_cast<std::size_t>(n);
            if (const int state = interrupted(); state != 1) return abort_transfer(state);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            const int ready = wait_for(handle, POLLOUT, end_ms, control);
            if (ready == 1) continue;
            const int code = written ? kLoaderPartial : (ready == -1 ? kLoaderCancelled : kLoaderNotSent);
            if (ready == -1) return finish(code, "Cancelled while sending to the loader.");
            if (ready == 0) return finish(code, "The loader stopped taking data before the deadline.");
            return finish(code, "Waiting on the loader socket failed.");
        }
        const std::string reason = n < 0 ? std::strerror(errno) : "no progress";
        app_log("companion: push failed after %zu of %zu bytes: %s", written, payload.size(),
                reason.c_str());
        return finish(written ? kLoaderPartial : kLoaderNotSent,
                      "The loader stopped receiving: " + reason);
    }

    app_log("companion: %zu bytes handed to loader %s:%u", payload.size(), host, port);
    if (const int state = interrupted(); state != 1) return abort_transfer(state);
    ::shutdown(handle, SHUT_WR);
    if (const int state = interrupted(); state != 1) return abort_transfer(state);

    /* The loader usually does not answer. A short wait, inside the same
     * deadline, notices a startup failure line; it never demands one. */
    const std::uint64_t reply_end = std::min<std::uint64_t>(end_ms, monotonic_ms() + 400);
    const int reply_ready = wait_for(handle, POLLIN, reply_end, control);
    if (reply_ready == -1) return abort_transfer(-1);
    if (const int state = interrupted(); state != 1) return abort_transfer(state);
    if (reply_ready == -2) return finish(kLoaderPartial, "Loader reply wait failed; startup outcome unknown.");
    if (reply_ready == 1)
    {
        char buffer[256];
        if (const int state = interrupted(); state != 1) return abort_transfer(state);
        const ssize_t n = ::recv(handle, buffer, sizeof(buffer), 0);
        if (const int state = interrupted(); state != 1) return abort_transfer(state);
        if (n > 0)
        {
            std::string reply(buffer, static_cast<std::size_t>(n));
            app_log("loader reply: %s", reply.c_str());
            // Loader text is not authenticated startup evidence. Bytes already
            // handed over remain submitted even for a failure-looking line.
            if (reply.find("app-service:") == 0 &&
                reply.find("loopback port") == std::string::npos &&
                reply.find("local console port") == std::string::npos)
                return finish(kLoaderPartial, "Full transfer; unauthenticated loader reply, startup outcome unknown: " + reply);
        }
    }
    return finish(kLoaderComplete, {});
}

int push_to_loader(void *ctx, const char *host, unsigned port, const std::string &payload,
                   std::string *detail)
{
    std::size_t transferred = 0;
    return push_to_loader_bounded(ctx, host, port, payload, LaunchControl{}, detail, &transferred) ==
                   kLoaderComplete
               ? 0
               : -1;
}
#endif // A client-only title has no asset reader or loader-send implementation.
} // namespace

LaunchAssets platform_launch_assets()
{
    LaunchAssets assets;
#if !defined(GC_EXISTING_SERVICE_ONLY) || !GC_EXISTING_SERVICE_ONLY
    assets.read = &read_packaged_asset;
#endif
    return assets;
}

bool platform_network_init(std::string *detail)
{
    /* 192.0.2.1 is documentation space. UDP connect only selects the route. */
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        if (detail) *detail = "Cannot open a socket to find the console address.";
        app_log("LAN discovery socket failed errno=%d", errno);
        return false;
    }
    sockaddr_in route{};
    route.sin_family = AF_INET;
#ifdef __PROSPERO__
    route.sin_len = sizeof(route);
#endif
    route.sin_port = htons(9);
    route.sin_addr.s_addr = htonl(0xc0000201u);
    sockaddr_in local{};
    socklen_t size = sizeof(local);
    int result = ::connect(fd, reinterpret_cast<sockaddr *>(&route), sizeof(route));
    if (result == 0)
        result = ::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &size);
    int saved = result ? errno : 0;
    ::close(fd);
    uint32_t address = 0;
    if (result == 0)
    {
        address = ntohl(local.sin_addr.s_addr);
        if (address == 0 || (address >> 24) == 127)
        {
            result = -1;
            saved = ENETUNREACH;
        }
    }
    if (result != 0)
    {
        if (detail)
            *detail = "Cannot determine the console LAN address (error " +
                      std::to_string(saved) + ").";
        app_log("LAN discovery failed errno=%d", saved);
        return false;
    }
    gc_client_set_address(address);
    char host[16];
    std::snprintf(host, sizeof(host), "%u.%u.%u.%u",
                  address >> 24, (address >> 16) & 255u, (address >> 8) & 255u,
                  address & 255u);
    discovered_loader_host = host;
    app_log("console LAN %s controller port %u; loopback is not used",
            host, static_cast<unsigned>(gc_client_port()));
    if (detail) *detail = host;
    return true;
}

const std::string &platform_loader_host()
{
    return discovered_loader_host;
}

LaunchTransport platform_launch_transport()
{
    LaunchTransport transport;
#if !defined(GC_EXISTING_SERVICE_ONLY) || !GC_EXISTING_SERVICE_ONLY
    transport.send_bytes = &push_to_loader;
    transport.send_bounded = &push_to_loader_bounded;
#endif
    return transport;
}

} // namespace ghostcontrol

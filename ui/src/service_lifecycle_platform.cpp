/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "service_lifecycle_platform.hpp"
#include "gc_client.h"
#include "json_reply.hpp"
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
namespace ghostcontrol {
namespace {
using Clock = std::chrono::steady_clock;
struct Context {
    bool capable = false, pending = false;
    CompanionIdentity capability, receipt;
};
bool same(const CompanionIdentity &a, const CompanionIdentity &b) {
    return a.boot == b.boot && a.pid == b.pid && a.started_ms == b.started_ms &&
        a.port == b.port && a.version == b.version;
}
bool cancelled(int (*cancel)(void *), void *ctx) { return cancel && cancel(ctx); }
unsigned remaining(Clock::time_point end) {
    auto n = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
    return n > 0 ? static_cast<unsigned>(n) : 0;
}
ProbeResult exchange(const char *command, const char *argument, unsigned timeout_ms,
                     int (*cancel)(void *), void *cancel_ctx) {
    ProbeResult result;
    result.transport = ProbeTransport::stalled;
    if (!timeout_ms || cancelled(cancel, cancel_ctx)) return result;
    std::vector<char> reply(32768);
    std::size_t length = 0;
    const GcClientResult rc = gc_client_send_cancelable(command, argument, reply.data(),
        reply.size(), &length, timeout_ms, cancel, cancel_ctx);
    result.connect_errno = gc_client_last_connect_errno();
    switch (rc) {
    case GC_CLIENT_OK:
        result.transport = ProbeTransport::answered;
        result.reply.assign(reply.data(), length); break;
    case GC_CLIENT_NO_SERVICE:
        result.transport = result.connect_errno == ECONNREFUSED ? ProbeTransport::refused :
            (result.connect_errno == ETIMEDOUT ? ProbeTransport::stalled : ProbeTransport::unreachable);
        break;
    case GC_CLIENT_TRUNCATED: result.transport = ProbeTransport::truncated; break;
    default: break;
    }
    return result;
}
ProbeResult probe_status(void *opaque, unsigned timeout_ms, int (*cancel)(void *), void *cancel_ctx) {
    auto &ctx = *static_cast<Context *>(opaque);
    ctx.capable = false; // Capability observation is NOT the transaction receipt.
    auto result = exchange("STATUS", nullptr, timeout_ms, cancel, cancel_ctx);
    CompanionIdentity identity;
    if (result.transport == ProbeTransport::answered && !cancelled(cancel, cancel_ctx) &&
        classify_companion_status(result.reply, gc_client_port(), &identity) == StatusVerdict::compatible) {
        const JsonReply json(result.reply);
        if (json.raw("lifecycle_contract") == "1" && json.raw("conditional_release") == "true") {
            ctx.capability = identity; ctx.capable = true;
        }
    }
    return result;
}
bool receipt_matches(const std::string &reply, const CompanionIdentity &expected, const std::string &nonce) {
    const JsonReply json(reply);
    return json.success() && json.raw("accepted") == "true" && json.raw("released") == "true" &&
        json.raw("lifecycle_schema") == "1" && json.raw("service_kind") == "\"app-native\"" &&
        json.raw("version") == "\"" + expected.version + "\"" &&
        json.raw("pid") == std::to_string(expected.pid) &&
        json.raw("started_ms") == std::to_string(expected.started_ms) &&
        json.raw("boot") == std::to_string(expected.boot) &&
        json.raw("listen_port") == std::to_string(expected.port) &&
        json.raw("request_nonce") == "\"" + nonce + "\"";
}
ProbeResult conditional_exit(void *opaque, const CompanionIdentity &expected, unsigned timeout_ms,
                             int (*cancel)(void *), void *cancel_ctx) {
    const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
    auto &ctx = *static_cast<Context *>(opaque);
    ctx.pending = false;
    ProbeResult refused; refused.transport = ProbeTransport::stalled;
    if (!timeout_ms || !ctx.capable || !same(ctx.capability, expected) ||
        cancelled(cancel, cancel_ctx)) return refused;
    ctx.capable = false;
    // The native UI kit is built without exceptions. Keep HOST exception
    // refusal where supported; neither branch changes protocol validation.
#if defined(__cpp_exceptions)
    try {
#endif
        unsigned char bytes[16]; std::size_t used = 0;
        int fd = open("/dev/urandom", O_RDONLY | O_NONBLOCK);
        if (fd < 0) return refused;
        while (used < sizeof(bytes) && remaining(end) && !cancelled(cancel, cancel_ctx)) {
            auto count = read(fd, bytes + used, sizeof(bytes) - used);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            used += static_cast<std::size_t>(count);
        }
        int closed = close(fd);
        if (closed || used != sizeof(bytes)) return refused;
        constexpr char hex[] = "0123456789abcdef";
        std::string nonce;
        for (auto byte : bytes) { nonce += hex[byte >> 4]; nonce += hex[byte & 15]; }
        const std::string argument = "1|app-native|" + expected.version + "|" +
            std::to_string(expected.pid) + "|" + std::to_string(expected.started_ms) + "|" +
            std::to_string(expected.boot) + "|" + std::to_string(expected.port) + "|" + nonce;
        const unsigned grant = remaining(end);
        if (!grant || argument.size() > 256 || cancelled(cancel, cancel_ctx)) return refused;
        auto result = exchange("RESIDENT_EXIT_IF", argument.c_str(), grant, cancel, cancel_ctx);
        if (result.transport != ProbeTransport::answered || !receipt_matches(result.reply, expected, nonce) ||
            !remaining(end) || cancelled(cancel, cancel_ctx)) return refused;
        ctx.receipt = expected;
        ctx.pending = true;
        return result;
#if defined(__cpp_exceptions)
    } catch (...) { ctx.pending = false; return refused; }
#endif
}
bool released(void *opaque, const CompanionIdentity &expected, unsigned timeout_ms,
              int (*cancel)(void *), void *cancel_ctx) {
    auto &ctx = *static_cast<Context *>(opaque);
    const bool valid = timeout_ms && !cancelled(cancel, cancel_ctx) && ctx.pending && same(ctx.receipt, expected);
    ctx.pending = false;
    return valid;
}
void transaction_end(void *opaque) { static_cast<Context *>(opaque)->pending = false; }
} // namespace
LifecycleProbe platform_lifecycle_probe() {
    LifecycleProbe probe;
    auto context = std::make_shared<Context>();
    probe.ctx = context.get(); probe.lifetime = context;
    probe.worker_claim = std::make_shared<std::atomic_bool>(false);
    probe.status = &probe_status;
    probe.conditional_exit = &conditional_exit;
    probe.released = &released;
    probe.transaction_end = &transaction_end;
    probe.expected_port = gc_client_port();
    return probe;
}
} // namespace ghostcontrol

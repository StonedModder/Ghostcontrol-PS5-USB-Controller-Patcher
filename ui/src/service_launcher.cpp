/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pure policy half of the companion bootstrap. No sockets and no files
 * here: the caller injects both, so the rules in service_launcher.hpp can be
 * proven on a host with no console.
 */
#include "service_launcher.hpp"

#include "app_log.hpp"
#include "json_reply.hpp"
#include "sha256.hpp"
#include <chrono>
#include <set>
#include <cctype>

namespace ghostcontrol
{
namespace
{
/* A loader that takes the whole ELF. A partial or empty push is not a launch,
 * and neither is a payload that is not an ELF: sending it would only produce a
 * console-side error the player cannot see. */
bool usable_elf(const std::string &bytes)
{
    if (bytes.size() < 64) return false;
    static const unsigned char kMagic[4] = {0x7f, 'E', 'L', 'F'};
    for (std::size_t i = 0; i < 4; ++i)
        if (static_cast<unsigned char>(bytes[i]) != kMagic[i]) return false;
    /* Match the package's structural ELF64 LE x86-64 bounded program-header
     * contract. Stripped images are allowed; this is not executability proof. */
    auto u64 = [&bytes](std::size_t at) {
        unsigned long long v = 0;
        for (std::size_t i = 0; i < 8; ++i)
            v |= static_cast<unsigned long long>(
                     static_cast<unsigned char>(bytes[at + i]))
                 << (8 * i);
        return v;
    };
    auto u16 = [&bytes](std::size_t at) {
        unsigned v = 0;
        for (std::size_t i = 0; i < 2; ++i)
            v |= static_cast<unsigned>(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
        return v;
    };
    if (bytes[4] != 2 || bytes[5] != 1 || u16(18) != 62) return false;
    const unsigned long long phoff = u64(32);
    const unsigned phentsize = u16(54), phnum = u16(56);
    return phnum > 0 && phentsize >= 56 && phoff <= bytes.size() &&
           static_cast<unsigned long long>(phnum) * phentsize <= bytes.size() - phoff;
}
} // namespace

ServiceLauncher::ServiceLauncher(LaunchTransport transport, LaunchAssets assets)
    : transport_(transport), assets_(assets)
{
}

std::string ServiceLauncher::check_manifest(std::string_view manifest, std::string_view elf,
                                            std::string *version)
{
    const JsonReply json(manifest);
    if (!json.valid) return "manifest is not one JSON object";
    if (json.raw("schema") != "1") return "unsupported manifest schema";
    if (json.string_field("artifact") != "GhosttoothAPI.elf") return "manifest names another artifact";
    if (json.raw("bytes") != std::to_string(elf.size())) return "byte count differs";
    const std::string digest = json.string_field("sha256");
    if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos)
        return "manifest digest is not lowercase SHA-256 hex";
    if (digest != Sha256::hex(elf)) return "SHA-256 differs";
    if (json.raw("status_schema") != "1" || json.string_field("service_kind") != "app-native")
        return "packaged companion speaks a different STATUS protocol";
    const std::string packaged = json.string_field("version");
    if (packaged.empty()) return "manifest has no version";
    auto alnum = [](unsigned char c) { return (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (packaged.size() > 128 || !alnum(packaged.front())) return "invalid version token";
    for (unsigned char c : packaged)
        if (!alnum(c) && c != '.' && c != '_' && c != '+' && c != '-') return "invalid version token";
    const auto revision = json.string_field("source_revision");
    if (revision != "unknown" && (revision.size() != 40 ||
        revision.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos))
        return "invalid source_revision";
    const auto dirty = json.raw("source_dirty");
    if (dirty != "true" && dirty != "false" && dirty != "null") return "invalid source_dirty";
    const auto notices = json.raw("notices");
    if (notices.size() < 2 || notices.front() != '[' || notices.back() != ']') return "invalid notices";
    std::set<std::string> seen;
    std::size_t pos = 1;
    auto space = [&]() { while (pos < notices.size() - 1 &&
        std::isspace(static_cast<unsigned char>(notices[pos]))) ++pos; };
    space();
    while (pos < notices.size() - 1)
    {
        if (notices[pos] != '"') return "invalid notice type";
        const auto start = pos++;
        while (pos < notices.size() - 1 && notices[pos] != '"')
        {
            if (notices[pos] == '\\') ++pos;
            ++pos;
        }
        if (pos >= notices.size() - 1) return "invalid notice string";
        const auto name = JsonReply::decode(notices.substr(start, ++pos - start));
        if (name.empty() || name == "." || name == "..") return "invalid notice basename";
        for (unsigned char c : name)
            if (c < 33 || c > 126 || c == '/' || c == '\\' || c == ':') return "invalid notice basename";
        if (!seen.insert(name).second) return "duplicate notice basename";
        space();
        if (pos == notices.size() - 1) break;
        if (notices[pos++] != ',') return "invalid notices";
        space();
        if (pos == notices.size() - 1) return "invalid notices";
    }
    // The runtime enforces typed descriptive provenance and notice names, not
    // filesystem/legal completeness or an external reviewed release hash.
    if (!usable_elf(std::string(elf))) return "ELF structure differs";
    if (version) *version = packaged;
    return {};
}

LaunchResult ServiceLauncher::launch(LaunchKind kind)
{
    std::string bytes;
    const LaunchResult prepared = prepare(kind, &bytes);
    if (prepared.state != LaunchState::not_attempted) return prepared;
    return submit(bytes);
}

LaunchResult ServiceLauncher::prepare(LaunchKind kind, std::string *bytes)
{
    LaunchResult result;
    prepared_bytes_.clear();
    prepared_version_.clear();
    prepared_manifest_.clear();
    authorized_ = false;
    if (bytes) bytes->clear();

    /* A refused loader is not retried every frame: one automatic attempt is
     * enough to know, and a player press is the way back. */
    if (kind == LaunchKind::automatic && (pushed_ || attempted_))
    {
        result.state = pushed_ ? LaunchState::already_sent
                               : LaunchState::transport_unreachable;
        result.detail = pushed_ ? "Ghosttooth API already sent; waiting for it to answer."
                                : "Automatic start already failed; use Service screen to retry.";
        app_log("service_launcher: automatic blocked pushed=%d attempted=%d",
                pushed_ ? 1 : 0, attempted_ ? 1 : 0);
        last_result_ = result;
        return result;
    }
    attempted_ = true;

    if (!transport_.send_bytes && !transport_.send_bounded)
    {
        result.state = LaunchState::transport_unavailable;
        result.detail = "This build cannot reach the console loader.";
        last_result_ = result;
        return result;
    }

    std::string loaded;
    if (!assets_.read || !assets_.read(assets_.ctx, kAssetPath, &loaded) || loaded.empty())
    {
        result.state = LaunchState::no_asset;
        result.detail =
            "Ghosttooth API is not packaged with this build. Load GhosttoothAPI.elf on the loader.";
        last_result_ = result;
        return result;
    }
    if (!usable_elf(loaded))
    {
        result.state = LaunchState::rejected;
        unsigned char m0 = loaded.size() > 0 ? static_cast<unsigned char>(loaded[0]) : 0;
        unsigned char m1 = loaded.size() > 1 ? static_cast<unsigned char>(loaded[1]) : 0;
        unsigned char m2 = loaded.size() > 2 ? static_cast<unsigned char>(loaded[2]) : 0;
        unsigned char m3 = loaded.size() > 3 ? static_cast<unsigned char>(loaded[3]) : 0;
        app_log("usable_elf rejected: size=%zu magic=%02x %02x %02x %02x", loaded.size(), m0, m1, m2, m3);
        result.detail =
            "The packaged Ghosttooth API ELF is incomplete or unreadable; it was not sent.";
        last_result_ = result;
        return result;
    }
    std::string manifest, version;
    if (!assets_.read(assets_.ctx, kManifestPath, &manifest) || manifest.empty())
    {
        result.state = LaunchState::rejected;
        result.detail = "The packaged Ghosttooth API has no manifest; it was not sent.";
        last_result_ = result;
        return result;
    }
    const std::string mismatch = check_manifest(manifest, loaded, &version);
    if (!mismatch.empty())
    {
        app_log("companion manifest rejected: %s", mismatch.c_str());
        result.state = LaunchState::rejected;
        result.detail = "The packaged Ghosttooth API does not match its manifest (" + mismatch +
                        "); it was not sent.";
        last_result_ = result;
        return result;
    }
    result.package_version = version;
    prepared_version_ = version;
    prepared_bytes_ = loaded;
    prepared_manifest_ = manifest;
    authorized_ = bytes != nullptr;
    if (bytes) *bytes = std::move(loaded);
    /* not_attempted: validated, nothing sent yet. last_result_ is left alone
     * so a reader never sees "nothing happened" for a push that is about to
     * happen. */
    return result;
}

LaunchResult ServiceLauncher::submit(const std::string &bytes, const LaunchControl &control)
{
    LaunchResult result;
    const auto start = std::chrono::steady_clock::now();
    auto cancelled = [&]() { return control.cancel && control.cancel(control.cancel_ctx); };
    if (cancelled())
    {
        authorized_ = false;
        result.state = LaunchState::cancelled;
        result.detail = "Cancelled before anything was sent to the loader.";
        last_result_ = result;
        return result;
    }
    if (!transport_.send_bytes && !transport_.send_bounded)
    {
        result.state = LaunchState::transport_unavailable;
        result.detail = "This build cannot reach the console loader.";
        last_result_ = result;
        return result;
    }
    if (!authorized_ || bytes != prepared_bytes_)
    {
        authorized_ = false;
        /* Every public submission is bound to the immutable prepared record.
         * No filesystem reread or digest callback can change that record. */
        result.state = LaunchState::rejected;
        result.detail =
            "The packaged Ghosttooth API ELF is incomplete or unreadable; it was not sent.";
        last_result_ = result;
        return result;
    }

    const bool withdrawn = cancelled();
    const auto expiry = start + std::chrono::milliseconds(control.deadline_ms);
    if (withdrawn || std::chrono::steady_clock::now() >= expiry)
    {
        authorized_ = false;
        result.state = withdrawn ? LaunchState::cancelled : LaunchState::transport_unreachable;
        result.detail = "Cancelled or deadline expired before anything was sent.";
        last_result_ = result;
        return result;
    }
    /* Keep the exact expiry alive through provider entry using its existing
     * cancellation seam. Flooring a relative allowance alone cannot cover a
     * scheduling delay between sampling and entry. The ordinary provider polls
     * this guard before connect/writes and throughout reply waits. */
    struct BudgetGuard
    {
        const LaunchControl &original;
        std::chrono::steady_clock::time_point expiry;
        bool withdrawn = false, expired = false;
        static int poll(void *ctx)
        {
            auto &guard = *static_cast<BudgetGuard *>(ctx);
            if (guard.original.cancel && guard.original.cancel(guard.original.cancel_ctx))
                guard.withdrawn = true;
            // Sample after the caller callback: its overhead is not loaned back.
            if (std::chrono::steady_clock::now() >= guard.expiry) guard.expired = true;
            return guard.withdrawn || guard.expired;
        }
    } guard{control, expiry};
    LaunchControl remaining = control;
    remaining.cancel = BudgetGuard::poll;
    remaining.cancel_ctx = &guard;
    result.payload_bytes = bytes.size();
    std::string detail;
    std::size_t transferred = 0;
    int rc;
    result.package_version = prepared_version_;
    if (transport_.send_bounded)
    {
        app_log("service_launcher: bounded push bytes=%zu deadline_ms=%u",
                bytes.size(), control.deadline_ms);
        /* Logging and result/version copies precede the final allowance sample.
         * Floor REMAINING time, not elapsed time; zero is never a client default.
         * The guard above prevents a rebased deadline across final entry work. */
        const auto whole_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            expiry - std::chrono::steady_clock::now()).count();
        if (whole_ms <= 0)
        {
            authorized_ = false;
            result.state = LaunchState::transport_unreachable;
            result.detail = "No whole millisecond remains; nothing was sent.";
            last_result_ = result;
            return result;
        }
        remaining.deadline_ms = static_cast<unsigned>(whole_ms);
        rc = transport_.send_bounded(transport_.ctx, loader_host_.c_str(), kLoaderPort, bytes,
                                     remaining, &detail, &transferred);
    }
    else
    {
        app_log("service_launcher: calling send_bytes=%p ctx=%p bytes=%zu",
                reinterpret_cast<void *>(transport_.send_bytes), transport_.ctx, bytes.size());
        rc = transport_.send_bytes(transport_.ctx, loader_host_.c_str(), kLoaderPort, bytes, &detail);
        rc = rc == 0 ? kLoaderComplete : kLoaderNotSent;
        if (rc == kLoaderComplete) transferred = bytes.size();
    }
    /* A transport can return Partial/Complete after observing withdrawal.
     * Retire authorization independently of byte/result classification: bytes
     * already accepted remain real, including a full completed transfer. */
    const bool transport_withdrawn = guard.withdrawn || cancelled();
    if (transport_withdrawn || rc == kLoaderCancelled) authorized_ = false;
    if (rc == kLoaderCancelled && guard.expired && !transport_withdrawn)
        rc = kLoaderNotSent;
    app_log("service_launcher: push returned rc=%d transferred=%zu detail='%s'",
            rc, transferred, detail.c_str());
    if (rc == kLoaderComplete && transferred == bytes.size() && !transport_withdrawn)
    {
        pushed_ = true;
        result.state = LaunchState::sent;
        result.bytes = bytes.size();
        last_result_ = result;
        /* Deliberately left not_attempted: only an answering STATUS may say
         * the service is running, and the caller owns that answer. */
        return result;
    }
    result.bytes = transferred;
    if (rc == kLoaderPartial || transferred > 0)
    {
        /* Bytes left this app. A loader that got part of an ELF may refuse it
         * or may not; nobody here can know, so it is never "not started". */
        pushed_ = true;
        result.state = LaunchState::outcome_unknown;
        result.detail = (detail.empty() ? std::string("The push to the loader did not finish")
                                        : detail) +
                        " (" + std::to_string(transferred) + " of " + std::to_string(bytes.size()) +
                        " bytes sent); whether anything started is unknown.";
    }
    else if (rc == kLoaderCancelled)
    {
        result.state = LaunchState::cancelled;
        result.detail = "Cancelled before anything was sent to the loader.";
    }
    else
    {
        result.state = LaunchState::transport_unreachable;
        result.detail = detail.empty() ? "The console loader did not accept Ghosttooth API." : detail;
    }
    last_result_ = result;
    return result;
}

} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "service_snapshot.hpp"

#include "app_log.hpp"
#include "gc_client.h"
#include "json_reply.hpp"
#include "screens.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <limits>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ghostcontrol
{
namespace
{
using Clock = std::chrono::steady_clock;

/* One worker, independent cadences. A full refresh is interruptible between
 * endpoints; actions always preempt the next poll. Runtime readback is targeted
 * and independent of the current live/motion screen. */
constexpr auto kStatusInterval = std::chrono::milliseconds(500);
constexpr auto kFullInterval = std::chrono::milliseconds(2000);
constexpr auto kLiveInterval = std::chrono::milliseconds(33);
constexpr auto kMotionInterval = std::chrono::milliseconds(250);


/* The worker's one reply buffer, reused for every exchange, so it must hold
 * the largest reply the app ever asks for. A LOG_TAIL reply is the big one:
 * the service hex-encodes the last 6144 bytes of its log, so a full one is
 * 6144 * 2 + framing ~= 12.3 KiB (a console-side probe measured 12,326). At
 * 32 KiB this buffer holds that whole with room for a longer list reply, and
 * a reply that still outgrows it comes back as GC_CLIENT_TRUNCATED rather
 * than a silent clip. */
constexpr std::size_t kReplyMax = 32768;
static_assert(kReplyMax >= 16384,
              "reply buffer must hold LOG_TAIL-class replies (~12.3 KiB) whole");
constexpr std::size_t kMaxPending = 16;
constexpr std::size_t kCommandMax = GC_CLIENT_COMMAND_MAX;
constexpr std::size_t kArgumentMax = GC_CLIENT_ARGUMENT_MAX;

/* Link transitions and the start of each failure streak are logged to the
 * app's own log. That log lives in the title's sandbox, which the PC can read
 * while the app runs (/mnt/sandbox/<title>_000/download0/ghostcontrol/app.log),
 * so a run that cannot reach the service leaves its reason behind instead of
 * looking like an empty screen. Called only from the worker thread. */
/* Probe the configured console address once after the first failed poll.
 * Each connection is bounded so diagnostics cannot block the worker. */
static void probe_one(unsigned port)
{
    const uint32_t ip = gc_client_address();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        app_log("service probe :%u socket() failed errno=%d", port, errno);
        return;
    }
    struct sockaddr_in address = {};
#ifdef __PROSPERO__
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(ip);
    int error = 0;
#ifdef __PROSPERO__
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, 0x1200, &enabled, sizeof(enabled)) != 0) error = errno;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) error = errno;
#endif
    if (!error && connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0)
    {
        error = errno;
        if (error == EINPROGRESS)
        {
            struct pollfd pending = {fd, POLLOUT, 0};
            const int ready = poll(&pending, 1, 250);
            if (ready <= 0) error = ready ? errno : ETIMEDOUT;
            else
            {
                socklen_t length = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) error = errno;
            }
        }
    }
    app_log("service probe %u.%u.%u.%u:%u result=%s errno=%d", ip >> 24,
            (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u, port,
            error ? "failed" : "connected", error);
    close(fd);
}

static void probe_network_once(unsigned port)
{
    probe_one(port);
    probe_one(9021);
}

bool log_link_result(bool up, const char *detail)
{
    static bool was_up = false;
    static bool ever_up = false;
    static unsigned fail_streak = 0;
    if (up)
    {
        if (!was_up)
        {
            app_log(ever_up ? "service link back up" : "service link up (first STATUS reply)");
            ever_up = true;
        }
        fail_streak = 0;
    }
    else
    {
        ++fail_streak;
        if (fail_streak == 1)
        {
            app_log("service link down (attempt %u): %s; connect errno=%d", fail_streak,
                    detail && *detail ? detail : "no detail",
                    gc_client_last_connect_errno());
            probe_network_once(gc_client_port());
        }
        else if (fail_streak == 30 || fail_streak % 300 == 0)
            app_log("service link down (attempt %u): %s", fail_streak,
                    detail && *detail ? detail : "no detail");
    }
    const bool changed = up != was_up;
    was_up = up;
    return changed;
}

bool text_is_clean(const char *text, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

/* ---- reading the service's one-line JSON ---------------------------------
 * Only what a toast sentence needs: whether "ok" is true, the string value of
 * a named field, and the two hex-encoded fields. The acknowledgements are
 * flat, so a key scan is enough and no parser is linked into the app. */

bool json_field_is_true(const std::string &json, const char *key)
{
    return JsonReply(json).is_true(key);
}

std::string json_string_field(const std::string &json, const char *key)
{
    return JsonReply(json).string_field(key);
}

/* Decode a hex-encoded field, as the service sends "detail", "data" and a
 * release "tag". Mirrors the reference decoder: an odd length, a non-hex digit
 * or an embedded NUL all mean "not usable" rather than a wrong value. */
std::string hex_field(const std::string &json, const char *key)
{
    const std::string hex = json_string_field(json, key);
    if (hex.empty() || (hex.size() % 2) != 0)
        return {};
    auto nibble = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int high = nibble(hex[i]);
        const int low = nibble(hex[i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0))
            return {};
        out.push_back(static_cast<char>(high * 16 + low));
    }
    return out;
}

/* The friendly kind a paired-device label leads with, from the vid/pid the
 * service reports. The console's own product-name table lives in the service,
 * which does not put a name on the wire, so this is a small, honest
 * approximation; the address that follows always tells two of the same kind
 * apart. Unknown identities fall back to the transport, never to a made-up
 * product name. */
std::string bluetooth_kind(std::string_view vid, std::string_view pid, bool le)
{
    if (vid == "054c")
    {
        if (pid == "0ce6") return "DualSense";
        if (pid == "0df2") return "DualSense Edge";
        if (pid == "05c4" || pid == "09cc") return "DualShock 4";
        return "PlayStation pad";
    }
    if (vid == "057e" && pid == "2009") return "Switch Pro";
    if (vid == "045e") return "Xbox";
    if (vid == "0f0d") return "Hori pad";
    if (vid == "2dc8") return "8BitDo pad";
    return le ? "Bluetooth LE pad" : "Bluetooth pad";
}

/* The sentence a command's reply becomes, ported from the reference client's
 * command_result() so the wording matches 1.x. Empty for a successful setup
 * step, which the setup screen already shows live. */
std::string command_result(const std::string &name, const std::string &argument, bool reached,
                           const std::string &response)
{
    if (!reached)
        return "Can't connect. Start Ghosttooth API from its page, then check the connection.";
    std::string result;
    if (!json_field_is_true(response, "ok"))
    {
        result = hex_field(response, "detail");
        if (result.empty())
        {
            result = json_string_field(response, "error");
            if (result.empty())
                result = "Request failed";
        }
        for (char &c : result)
            if (c == '_' || static_cast<unsigned char>(c) < 32 || c == 127)
                c = ' ';
        // The command log does not contain replies. Keep the actual reason
        // in feedback (and its Details reader), rather than send the player
        // to a log which cannot explain the refusal. Bound untrusted text and
        // never cut a UTF-8 continuation sequence midway.
        if (result.size() > 1024) {
            std::size_t end = 1024;
            while (end && (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) --end;
            result.resize(end);
            result += " [reason truncated]";
        }
        const std::string prefix = name.rfind("BLUETOOTH_", 0) == 0 ?
            "Bluetooth request failed: " : "Request failed: ";
        return prefix + result + ". Square opens Details.";
    }
    else if (name == "BLUETOOTH_PAIR")
        return json_field_is_true(response, "pending") ?
            "Pair requested. Waiting for the radio." :
            "Pair request accepted. Check Bluetooth for the search status.";
    else if (name.rfind("PROFILE_", 0) == 0 && json_field_is_true(response, "saved"))
    {
        const JsonReply storage(response);
        if (storage.raw("verified") == "false")
            return "Profile change committed, but not verified. Reopen it before retrying.";
        if (name == "PROFILE_CREATE") return "Profile created and saved.";
        if (name == "PROFILE_RENAME") return "Profile renamed and saved.";
        if (name == "PROFILE_IMPORT") return "Profile imported and saved.";
        if (name == "PROFILE_EXPORT") return "Profile exported to Ghosttooth API's exports folder.";
        return "Profile saved. Controller output is unchanged.";
    }
    else if (!json_field_is_true(response, "applied") &&
             name != "UPDATE_CHECK" && name != "UPDATE_DOWNLOAD" && name != "DEVICES")
    {
        return name == "DUALSENSE_RUMBLE" ? "Rumble request accepted. Effect not confirmed." :
               name == "DUALSENSE_RUMBLE_STOP" ? "Stop request accepted. Rumble stop not confirmed." :
               std::string("Request accepted. Result not confirmed.");
    }
    else if (name == "SETUP_BEGIN" || name == "SETUP_ARM" || name == "SETUP_SKIP")
    {
        /* A successful step is silent: the setup screen shows the prompt. */
    }
    else if (name == "SETUP_SAVE") result = "Controller profile saved";
    else if (name == "SETUP_CANCEL") result = "Setup cancelled";
    else if (name == "DUALSENSE_RUMBLE") result = json_field_is_true(response, "applied") ? "Rumble applied" : "Rumble request accepted. Effect not confirmed.";
    else if (name == "DUALSENSE_RUMBLE_STOP") result = json_field_is_true(response, "applied") ? "Rumble stop applied" : "Stop request accepted. Rumble stop not confirmed.";

    else if (name == "BLUETOOTH_RETRY") result = "Bluetooth retry started";
    else if (name == "BLUETOOTH_FORGET") result = "Bluetooth controller forgotten";
    else if (name.rfind("SETUP_", 0) == 0) result = "Setup updated";
    else if (name == "DEVICES") result = "Connected controller list refreshed";
    else if (name == "UPDATE_CHECK")
        result = json_field_is_true(response, "available")
                     ? "Update available: " + hex_field(response, "tag")
                     : "No newer release available";
    else if (name == "UPDATE_DOWNLOAD")
        result = json_field_is_true(response, "downloaded")
                     ? "Update saved to /data/ghostcontrol/updates/Ghostcontrol.elf"
                     : "No newer release available";
    else if (name == "UPDATE_ROLLBACK") result = "Previous download restored";
    else if (name == "PROFILE_CREATE") result = "Profile created: " + argument;
    else if (name == "PROFILE_IMPORT") result = "Profile imported: " + argument;
    else if (name == "PROFILE_EXPORT") result = "Profile exported to /data/ghostcontrol/exports";
    else if (name == "PROFILE_EDIT") result = "Profile saved";
    else if (name == "PROFILE_RENAME") result = "Profile renamed";
    else if (name == "CONTROLLER_SAVE")
        result = "Controller rule saved. Reconnect the controller.";
    else if (name == "START") result = "Controller input started";
    else if (name == "STOP") result = "Controller input stopped";
    else if (name == "RESTART") result = "Controller input restarted";
    else if (name == "REBIND" || name == "FORCE_VDA") result = "Controllers reconnected";
    else if (name == "RELOAD") result = "Configuration reloaded";
    else result = json_field_is_true(response, "applied") ? "Request applied" : "Request accepted. Result not confirmed.";
    return result;
}

} // namespace

ServiceSnapshot::~ServiceSnapshot()
{
    stop();
}

void ServiceSnapshot::start()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        stopping_ = false;
    }
    if (running_.exchange(true))
        return;
    const uint32_t ip = gc_client_address();
    app_log("snapshot worker starting; ipc target %u.%u.%u.%u:%u", ip >> 24,
            (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u, (unsigned)gc_client_port());
    worker_ = std::thread([this] { run(); });
}

void ServiceSnapshot::stop()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        stopping_ = true;
    }
    if (!running_.exchange(false))
        return;
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

ServiceSnapshot::View ServiceSnapshot::read() const
{
    std::lock_guard<std::mutex> guard(lock_);
    return view_;
}

std::vector<ServiceSnapshot::View::PairedDevice>
ServiceSnapshot::parse_bluetooth_paired(std::string_view bluetooth_json)
{
    std::vector<View::PairedDevice> devices;
    const JsonReply reply(bluetooth_json);
    const auto array = reply.raw("paired");
    if (!reply.success() || array.size() < 2 || array.front() != '[') return {};
    std::size_t pos = 1;
    json_detail::skip_space(array, pos);
    while (pos < array.size() && array[pos] != ']') {
        const auto start = pos;
        if (array[pos] != '{' || !json_detail::value_end(array, pos)) return {};
        const JsonReply entry(array.substr(start, pos - start));
        const auto field = entry.raw("address");
        const std::string address = entry.string_field("address");
        // gc_bluetooth_format_status emits six uppercase hex octets joined by
        // colons (%02X). Reject rather than normalize noncanonical identities.
        if (!entry.valid || field.empty() || field.front() != '"' || address.size() != 17) return {};
        for (std::size_t i = 0; i < address.size(); ++i) {
            const char c = address[i];
            if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return {};
        }
        for (const auto &seen : devices) if (seen.address == address) return {};
        for (const char *key : {"le", "connected"}) {
            const auto value = entry.raw(key);
            if (!value.empty() && value != "true" && value != "false") return {};
        }
        for (const char *key : {"vid", "pid"}) {
            const auto value = entry.raw(key);
            if (value.empty()) continue; // optional; absence is not classification
            const std::string decoded = entry.string_field(key);
            if (value.front() != '"' || decoded.size() != 4 ||
                !std::all_of(decoded.begin(), decoded.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) return {};
        }
        const std::string vid = entry.string_field("vid");
        const std::string pid = entry.string_field("pid");
        const bool le = entry.is_true("le");
        const bool classified = !vid.empty() && !pid.empty() && !entry.raw("le").empty();
        const std::string kind = classified ? bluetooth_kind(vid, pid, le) :
            (le ? "Bluetooth LE device" : "Bluetooth device");
        devices.push_back({address, kind + " - " + address});
        json_detail::skip_space(array, pos);
        if (pos < array.size() && array[pos] == ',') { ++pos; json_detail::skip_space(array, pos); }
        else break;
    }
    // Invalid or ambiguous lists have NO actionable subset.
    return pos < array.size() && array[pos] == ']' ? devices : std::vector<View::PairedDevice>{};
}

const char *ServiceSnapshot::controller_output_label(std::string_view type)
{
    if (type == "ds4") return "DualShock 4 (DS4)";
    if (type == "ds5") return "DualSense (DS5)";
    if (type == "psvr2_left") return "PSVR2 Sense left";
    if (type == "psvr2_right") return "PSVR2 Sense right";
    return "Unknown output type";
}
bool ServiceSnapshot::controller_output_type(std::string_view type)
{
    return type == "ds4" || type == "ds5" || type == "psvr2_left" || type == "psvr2_right";
}
bool ServiceSnapshot::parse_controller_rules(std::string_view json, std::vector<ControllerRule> &rules)
{
    rules.clear();
    const JsonReply reply(json);
    const auto array = reply.raw("controllers");
    if (!reply.success() || array.size() < 2 || array.front() != '[') return false;
    size_t i = 1;
    json_detail::skip_space(array, i);
    while (i < array.size() && array[i] != ']') {
        const auto start = i;
        if (!json_detail::value_end(array, i)) return false;
        const JsonReply entry(array.substr(start, i - start));
        ControllerRule rule{entry.string_field("id"), entry.string_field("output_type")};
        // Wire IDs are never normalized. Ambiguous/unrepresentable IDs cannot be selected.
        if (!entry.valid || rule.id.empty() || rule.id.find('|') != rule.id.npos ||
            !text_is_clean(rule.id.data(), rule.id.size()) || rule.id.find('\0') != rule.id.npos ||
            rule.output_type.empty()) { rules.clear(); return false; }
        for (const auto &seen : rules) if (seen.id == rule.id) { rules.clear(); return false; }
        rules.push_back(std::move(rule));
        json_detail::skip_space(array, i);
        if (i < array.size() && array[i] == ',') { ++i; json_detail::skip_space(array, i); }
        else break;
    }
    return i < array.size() && array[i] == ']';
}
std::string ServiceSnapshot::controller_rule_label(const ControllerRule &rule)
{
    return rule.id + " | Saved output type: " + controller_output_label(rule.output_type) +
        " [" + rule.output_type + "] | Live output unavailable";
}

std::vector<std::string> ServiceSnapshot::setup_raw_lines(std::string_view json)
{
    const JsonReply reply(json);
    const auto invalid = [] { return std::vector<std::string>{
        "Couldn't read input from the selected controller.", "Press Cross to refresh."}; };
    if (!reply.success()) return invalid();
    const auto active = reply.raw("active"), valid = reply.raw("source_valid");
    if (active != "true" && active != "false") return invalid();
    if (active == "false") return {"No controller selected for setup.",
        "Start Guided setup and choose a USB or Bluetooth controller.",
        "App controller input does not include raw USB or Bluetooth data."};
    if (valid != "true" && valid != "false") return invalid();
    if (valid == "false") return {"The selected controller disconnected or changed.",
        "Start setup again. Previous input is hidden."};
    const auto connected = reply.raw("connected");
    if (connected != "true" && connected != "false") return invalid();
    if (connected == "false") return {"No input available from the selected controller.",
        "Previous input is hidden. Press Cross to retry."};
    auto integer = [](std::string_view text, std::uint64_t &value) {
        value = 0;
        if (text.empty()) return false;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            const unsigned digit = static_cast<unsigned>(c - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return true;
    };
    std::uint64_t token = 0, bytes = 0, limit = 0;
    const auto token_text = reply.raw("source_connection_generation");
    const std::string path = reply.string_field("path"), hex = reply.string_field("raw_report_hex");
    const auto hex_field = reply.raw("raw_report_hex");
    if (!integer(token_text, token) || !token ||
        !integer(reply.raw("raw_cached_bytes"), bytes) || !integer(reply.raw("raw_cache_limit"), limit) ||
        !limit || limit > 32 || bytes > limit || hex.size() != bytes * 2 ||
        hex_field.empty() || hex_field.front() != '"' || path.empty() || path.size() >= 96 ||
        path.find('\0') != path.npos || !text_is_clean(path.data(), path.size())) return invalid();
    for (char c : hex)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return invalid();
    std::vector<std::string> lines{
        "Selected physical source cache (snapshot)", "Path: " + path,
        "Observed connection: " + std::string(token_text),
        "Cached bytes: " + std::to_string(bytes) + " / " + std::to_string(limit),
        "Cache is capped; larger reports may be truncated.",
        "Last service-cached packet. Cross requests the cache again.",
        "Not a live monitor, arbitrary HID decoder or game-output proof.", ""};
    if (!bytes) lines.emplace_back("No raw packet is cached for this source.");
    for (std::size_t offset = 0; offset < bytes; offset += 16) {
        char label[8]; std::snprintf(label, sizeof label, "%02zx: ", offset);
        std::string line(label);
        for (std::size_t i = offset; i < std::min<std::size_t>(bytes, offset + 16); ++i) {
            if (i != offset) line += ' ';
            line.append(hex, i * 2, 2);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

bool ServiceSnapshot::parse_setup_sources(std::string_view json, std::vector<SetupSource> &sources)
{
    sources.clear();
    const JsonReply reply(json);
    const auto array = reply.raw("devices");
    if (!reply.success() || array.size() < 2 || array.front() != '[') return false;
    auto integer = [](std::string_view text, std::uint64_t &value) {
        value = 0;
        if (text.empty()) return false;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            const unsigned digit = static_cast<unsigned>(c - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return true;
    };
    auto token_text = [](const std::string &text) {
        return !text.empty() && text.find('|') == text.npos && text.find('\0') == text.npos &&
               text_is_clean(text.data(), text.size());
    };
    std::vector<std::string> seen_paths;
    std::vector<std::uint64_t> seen_generations;
    std::vector<SetupSource> parsed;
    size_t i = 1;
    json_detail::skip_space(array, i);
    while (i < array.size() && array[i] != ']') {
        const auto start = i;
        if (!json_detail::value_end(array, i)) return false;
        const JsonReply entry(array.substr(start, i - start));
        if (!entry.valid) return false;
        const std::string transport = entry.string_field("transport");
        const bool saved_profile = !entry.string_field("setup_profile_id").empty() &&
            entry.string_field("setup_profile_id") != "default";
        /* A plugged controller that already has a profile must stay selectable
         * so setup can edit it, even when the pipe is not streaming right now. */
        if (entry.is_true("identity_known") && entry.is_true("connected") &&
            (entry.is_true("input_available") || saved_profile) &&
            (transport == "usb" || transport == "bluetooth")) {
            std::uint64_t vid, pid, generation;
            /* The service has emitted this as both a bare number and a quoted
             * string across builds; decode either, never reject the array for
             * a formatting difference. */
            std::string generation_text = entry.string_field("connection_generation");
            if (generation_text.empty())
                generation_text = std::string(entry.raw("connection_generation"));
            const std::string path = entry.string_field("path");
            const std::string protocol = entry.string_field("protocol");
            std::string profile = entry.string_field("setup_profile_id");
            const auto profile_field = entry.raw("setup_profile_id");
            if (!profile_field.empty() && profile_field.front() != '"') return false;
            if (!integer(entry.raw("vid"), vid) || !vid || vid > 0xffff ||
                !integer(entry.raw("pid"), pid) || pid > 0xffff ||
                !integer(generation_text, generation) || !generation ||
                !token_text(path) || path.size() >= 128 || !token_text(protocol) || protocol.size() >= 32)
                return false;
            char identity[16];
            std::snprintf(identity, sizeof identity, "%04x|%04x", static_cast<unsigned>(vid), static_cast<unsigned>(pid));
            /* "default" is the catch-all profile: it matches vid=any/pid=any and
             * its rule id already belongs to the wildcard controller, so a
             * draft bound to it is refused at save with "profile id already
             * belongs to another controller". Give every identified source its
             * own unique id instead, so one profile per (vid,pid,protocol) is
             * created exactly as the per-mode sweep requires. */
            if (profile.empty() || profile == "default") {
                char candidate[64];
                std::snprintf(candidate, sizeof candidate, "pad-%04x-%04x-%s", static_cast<unsigned>(vid), static_cast<unsigned>(pid), protocol.c_str());
                profile = candidate;
            }
            if (profile.empty() || profile.size() >= 64 || profile == "." || profile == ".." ||
                profile.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != profile.npos)
                return false;
            if (std::find(seen_paths.begin(), seen_paths.end(), path) != seen_paths.end() ||
                std::find(seen_generations.begin(), seen_generations.end(), generation) != seen_generations.end())
                return false;
            seen_paths.push_back(path); seen_generations.push_back(generation);
            const std::string selection = path + "|" + identity + "|" + profile + "|identity|" + generation_text;
            if (selection.size() >= 256) return false;
            SetupSource source;
            source.connection_generation = generation_text;
            static const char digits[] = "0123456789abcdef";
            for (unsigned char c : selection) { source.selection += digits[c >> 4]; source.selection += digits[c & 15]; }
            std::string name = entry.string_field("name");
            if (!text_is_clean(name.data(), name.size()) || name.find('\0') != name.npos) return false;
            source.edits_existing = !entry.string_field("setup_profile_id").empty() &&
                entry.string_field("setup_profile_id") != "default" &&
                entry.string_field("setup_profile_id") == profile;
            source.label = (name.empty() ? path : name) + " | " + transport + " | " + protocol + " | " + path +
                (source.edits_existing ? " | edit saved profile" : " | physical input");
            parsed.push_back(std::move(source));
        }
        json_detail::skip_space(array, i);
        if (i < array.size() && array[i] == ',') { ++i; json_detail::skip_space(array, i); }
        else break;
    }
    if (i >= array.size() || array[i] != ']') return false;
    sources = std::move(parsed);
    return true;
}

bool ServiceSnapshot::queue_command(const char *command, const char *argument,
                                    unsigned timeout_ms)
{
    if (!command || !*command)
        return false;
    const std::size_t command_length = std::strlen(command);
    if (command_length > kCommandMax || !text_is_clean(command, command_length))
        return false;

    std::string argument_text;
    if (argument && *argument)
    {
        const std::size_t argument_length = std::strlen(argument);
        if (argument_length > kArgumentMax || !text_is_clean(argument, argument_length))
            return false;
        argument_text = argument;
    }

    std::lock_guard<std::mutex> guard(lock_);
    if (stopping_ || commands_.size() >= kMaxPending)
        return false;
    commands_.push_back(Command{std::string(command), std::move(argument_text), timeout_ms});
    fetches_[command].state = ResultState::queued;
    wake_.notify_all();
    return true;
}

bool ServiceSnapshot::queue_commands(const std::vector<std::pair<std::string, std::string>> &commands,
                                     unsigned timeout_ms)
{
    if (commands.empty() || commands.size() > kMaxPending) return false;
    for (const auto &[name, argument] : commands) {
        if (name.empty() || name.size() > kCommandMax || argument.size() > kArgumentMax ||
            !text_is_clean(name.data(), name.size()) || !text_is_clean(argument.data(), argument.size())) return false;
    }
    std::lock_guard guard(lock_);
    if (stopping_ || commands_.size() + commands.size() > kMaxPending) return false;
    size_t index = 0;
    for (const auto &[name, argument] : commands) {
        commands_.push_back(Command{name, argument, timeout_ms, false, false, ++index == commands.size()});
        fetches_[name].state = ResultState::queued;
    }
    wake_.notify_all();
    return true;
}

bool ServiceSnapshot::queue_fetch(const char *command, const char *argument, unsigned timeout_ms)
{
    return queue_fetch_impl(command, argument, timeout_ms, false) != 0;
}

unsigned long long ServiceSnapshot::queue_fetch_request(const char *command, const char *argument, unsigned timeout_ms)
{
    return queue_fetch_impl(command, argument, timeout_ms, true);
}

unsigned long long ServiceSnapshot::queue_fetch_impl(const char *command, const char *argument,
                                                   unsigned timeout_ms, bool tracked)
{
    /* The same gate as a command: a fetch that cannot be sent must not join
     * the queue and then have the worker report a failure the screen did not
     * ask for. */
    if (!command || !*command)
        return false;
    const std::size_t command_length = std::strlen(command);
    if (command_length > kCommandMax || !text_is_clean(command, command_length))
        return false;

    std::string argument_text;
    if (argument && *argument)
    {
        const std::size_t argument_length = std::strlen(argument);
        if (argument_length > kArgumentMax || !text_is_clean(argument, argument_length))
            return false;
        argument_text = argument;
    }

    std::lock_guard<std::mutex> guard(lock_);
    if (stopping_ || commands_.size() >= kMaxPending)
        return false;
    unsigned long long request_id = 0;
    if (tracked) {
        if (next_fetch_request_ == std::numeric_limits<unsigned long long>::max()) return 0;
        // Bound retained receipts without ever discarding an in-flight request.
        while (fetch_requests_.size() >= 2 * kMaxPending) {
            const auto completed = std::find_if(fetch_requests_.begin(), fetch_requests_.end(),
                [](const auto &entry) { return entry.second.state != ResultState::queued; });
            if (completed == fetch_requests_.end()) return 0;
            fetch_requests_.erase(completed);
        }
        request_id = ++next_fetch_request_;
        fetch_requests_[request_id].state = ResultState::queued;
    }
    Command entry{std::string(command), std::move(argument_text), timeout_ms, /*fetch=*/true};
    entry.fetch_request_id = request_id;
    commands_.push_back(std::move(entry));
    fetches_[command].state = ResultState::queued;
    wake_.notify_all();
    return tracked ? request_id : 1;
}

ServiceSnapshot::Fetch ServiceSnapshot::fetch_request(unsigned long long request_id) const
{
    std::lock_guard guard(lock_);
    const auto found = fetch_requests_.find(request_id);
    return found == fetch_requests_.end() ? Fetch{} : found->second;
}

bool ServiceSnapshot::queue_runtime_toggle(const char *key)
{
    if (!key || !*key || std::strlen(key) > 64) return false;
    for (const char *c = key; *c; ++c)
        if (!std::isalnum(static_cast<unsigned char>(*c)) && *c != '_') return false;
    std::lock_guard guard(lock_);
    if (stopping_ || commands_.size() >= kMaxPending) return false;
    commands_.push_back(Command{"RUNTIME_SET", key, 0, false, true});
    fetches_["RUNTIME_SET"].state = ResultState::queued;
    wake_.notify_all();
    return true;
}

ServiceSnapshot::Fetch ServiceSnapshot::last_reply(std::string_view command) const
{
    std::lock_guard<std::mutex> guard(lock_);
    const auto found = fetches_.find(std::string(command));
    if (found == fetches_.end())
        return Fetch{};
    return found->second;
}

bool ServiceSnapshot::publish_native_pad(std::string_view poll, std::string_view ack)
{
    if (poll.empty() || poll.size() >= 512 || ack.size() >= 512 ||
        !text_is_clean(poll.data(), poll.size()) || !text_is_clean(ack.data(), ack.size()) ||
        poll.find('\0') != poll.npos || ack.find('\0') != ack.npos) return false;
    std::lock_guard guard(lock_);
    if (stopping_) return false;
    native_poll_.assign(poll); native_ack_.assign(ack);
    wake_.notify_all();
    return true;
}
ServiceSnapshot::NativeExchange ServiceSnapshot::native_pad_exchange() const
{
    std::lock_guard guard(lock_);
    return native_exchange_;
}
void ServiceSnapshot::publish_native_observation(std::string_view json)
{
    if (json.size() > 2048 || !JsonReply(json).valid) return;
    std::lock_guard guard(lock_);
    view_.native_pad_json.assign(json);
}

void ServiceSnapshot::refresh()
{
    std::lock_guard<std::mutex> guard(lock_);
    refresh_ = true;
    wake_.notify_all();
}

void ServiceSnapshot::set_live(bool enabled)
{
    std::lock_guard<std::mutex> guard(lock_);
    live_ = enabled;
    wake_.notify_all();
    if (!enabled)
        live_input_.clear();
}

void ServiceSnapshot::set_motion(bool enabled)
{
    std::lock_guard<std::mutex> guard(lock_);
    motion_ = enabled;
    wake_.notify_all();
}

std::string ServiceSnapshot::live_input() const
{
    std::lock_guard<std::mutex> guard(lock_);
    return live_input_;
}

std::string ServiceSnapshot::take_toast()
{
    std::lock_guard<std::mutex> guard(lock_);
    if (toasts_.empty()) return {};
    std::string toast = std::move(toasts_.front());
    toasts_.pop_front();
    return toast;
}

int ServiceSnapshot::exchange(const char *name, const char *arg, std::string &reply, unsigned timeout_ms)
{
    return gc_client_send_cancelable(name, arg, reply.data(), reply.size(), nullptr,
        timeout_ms, [](void *ctx) -> int {
            return !static_cast<ServiceSnapshot *>(ctx)->running_.load();
        }, this);
}

void ServiceSnapshot::push_toast(const std::string &text)
{
    if (text.empty()) return;
    std::lock_guard guard(lock_);
    if (toasts_.size() == 32) toasts_.pop_front();
    toasts_.push_back(text);
}

void ServiceSnapshot::run()
{
    std::string reply(kReplyMax, '\0');
    auto next_status = Clock::now(), next_full = Clock::now();
    auto next_live = Clock::now(), next_motion = Clock::now();
    auto next_native = Clock::now(), next_ack = Clock::now();
    std::string accepted_native_ack;
    constexpr auto native_interval = std::chrono::milliseconds(125);
    constexpr auto ack_retry_interval = std::chrono::milliseconds(250);
    bool was_live = false, was_motion = false;
    constexpr const char *endpoints[] = {"RUNTIME_GET", "BLUETOOTH_STATUS", "PROFILES",
        "CONTROLLERS", "DEVICES", "CONSOLE_ID", "LOG_TAIL", "DUALSENSE_FX_STATUS", "SETUP_STATUS", "AUDIO_STATUS"};
    size_t full_index = std::size(endpoints);
    bool batch_boundary = true;
    while (running_.load()) {
        // A continuous producer cannot grow the acknowledged batch without bound.
        if (batch_boundary && runtime_pending_.size() >= kMaxPending) { readback_runtime(reply); continue; }
        bool live, motion, requested;
        Command command;
        std::string native_poll, native_ack;
        bool have_command = false;
        {
            std::lock_guard guard(lock_);
            live = live_; motion = motion_; requested = refresh_; refresh_ = false;
            native_poll = native_poll_; native_ack = native_ack_;
            if (!commands_.empty()) {
                command = std::move(commands_.front()); commands_.pop_front(); have_command = true;
            }
        }
        const auto now = Clock::now();
        if (requested || (was_live && !live) || (was_motion && !motion) ||
            (!live && !motion && now >= next_full && full_index == std::size(endpoints))) {
            full_index = 0; next_full = now + kFullInterval;
        }
        was_live = live; was_motion = motion;
        // One exchange per iteration; queued writes always preempt the next poll.
        if (have_command) {
            if (command.toggle) {
                // Intent is evaluated only here, not from a stale frame copy.
                if (!runtime_pending_.empty()) readback_runtime(reply);
                fetch_endpoint("RUNTIME_GET", reply);
                const auto fresh = last_reply("RUNTIME_GET");
                const auto raw = JsonReply(fresh.body).raw(command.argument);
                if (!fresh.valid || (raw != "true" && raw != "false" && raw != "0" && raw != "1")) {
                    push_toast("Couldn't read this setting. Nothing sent.");
                    std::lock_guard guard(lock_);
                    auto &f = fetches_["RUNTIME_SET"]; f.state = ResultState::readback_failed;
                    f.detail = "Couldn't read this setting. Nothing sent."; f.generation = ++fetch_generation_;
                    continue;
                }
                command.argument += (raw == "true" || raw == "1") ? "=0" : "=1";
            }
            if (running_.load()) {
                if (command.fetch) run_fetch(command, reply); else run_command(command, reply);
            }
            batch_boundary = command.batch_last;
            continue;
        }
        if (!runtime_pending_.empty()) { readback_runtime(reply); continue; }
        if (now >= next_status) {
            fetch_status(reply); next_status = Clock::now() + kStatusInterval; continue;
        }
        if (!native_poll.empty() && now >= next_native) {
            timespec sent{};
            const bool clock_ok = clock_gettime(CLOCK_MONOTONIC, &sent) == 0;
            const auto sent_ms = clock_ok ? static_cast<std::uint64_t>(sent.tv_sec) * 1000 +
                static_cast<std::uint64_t>(sent.tv_nsec) / 1000000 : 0;
            run_fetch(Command{"APP_PAD_POLL", native_poll, 300, true}, reply);
            const auto response = last_reply("APP_PAD_POLL");
            {
                std::lock_guard guard(lock_);
                native_exchange_ = NativeExchange{response.body, response.generation, sent_ms,
                    response.valid && clock_ok};
            }
            next_native = Clock::now() + native_interval;
            continue;
        }
        if (!native_ack.empty() && native_ack != accepted_native_ack && now >= next_ack) {
            run_fetch(Command{"APP_PAD_ACK", native_ack, 300, true}, reply);
            if (last_reply("APP_PAD_ACK").valid) accepted_native_ack = native_ack;
            next_ack = Clock::now() + ack_retry_interval;
            continue;
        }
        if (full_index < std::size(endpoints)) {
            fetch_endpoint(endpoints[full_index++], reply);
            if (full_index == std::size(endpoints)) next_full = Clock::now() + kFullInterval;
            continue;
        }
        if (live && now >= next_live) {
            fetch_endpoint("SETUP_INPUT", reply); next_live = Clock::now() + kLiveInterval; continue;
        }
        if (motion && now >= next_motion) {
            fetch_fx(reply); next_motion = Clock::now() + kMotionInterval; continue;
        }
        auto due = next_status;
        if (!live && !motion) due = std::min(due, next_full);
        if (live) due = std::min(due, next_live);
        if (motion) due = std::min(due, next_motion);
        if (!native_poll.empty()) due = std::min(due, next_native);
        if (!native_ack.empty() && native_ack != accepted_native_ack) due = std::min(due, next_ack);
        std::unique_lock guard(lock_);
        wake_.wait_until(guard, due, [&] { return !running_.load() || !commands_.empty() || refresh_ ||
            live_ != live || motion_ != motion || native_poll_ != native_poll || native_ack_ != native_ack; });
    }
    // Shutdown cancels queued work rather than sending it after the app exits.
    std::lock_guard guard(lock_);
    commands_.clear(); runtime_pending_.clear();
    for (auto &[name, f] : fetches_) if (f.state == ResultState::queued || f.state == ResultState::acknowledged) {
        if (f.state == ResultState::queued) f.state = ResultState::transport_failed;
        else if (name == "RUNTIME_SET") f.state = ResultState::readback_failed;
        else continue;
        f.detail = "Stopped before completion"; f.generation = ++fetch_generation_;
    }
    for (auto &[id, f] : fetch_requests_) if (f.state == ResultState::queued) {
        f.state = ResultState::transport_failed;
        f.detail = "Stopped before completion"; f.generation = ++fetch_generation_;
    }
}

void ServiceSnapshot::run_command(const Command &command, std::string &reply)
{
    if (command.name == "AUDIO_UI") { run_audio(command, reply); return; }
    if (command.name == "CONTROLLER_OUTPUT_SET") { run_controller_output(command, reply); return; }
    if (command.name == "SETUP_BEGIN") { run_setup_begin(command, reply); return; }
    if (command.name == "SETUP_SAVE") { run_setup_save(command, reply); return; }
    const auto transport = exchange(command.name.c_str(), command.argument.empty() ? nullptr : command.argument.c_str(), reply, command.timeout_ms);
    const bool reached = transport == GC_CLIENT_OK;
    const std::string response = reached ? std::string(reply.c_str()) : std::string();
    const JsonReply json(response);
    const bool acknowledged = json.success();
    {
        std::lock_guard guard(lock_);
        auto &f = fetches_[command.name];
        f.body = response; f.reached = reached; f.valid = acknowledged;
        f.state = !reached ? ResultState::transport_failed : !acknowledged ? ResultState::rejected :
            (command.name != "RUNTIME_SET" && json.is_true("applied")) ? ResultState::applied : ResultState::acknowledged;
        f.detail = gc_client_result_text(static_cast<GcClientResult>(transport)); f.generation = ++fetch_generation_;
    }
    // Never log arguments or raw replies: profiles, tokens and user text can be sensitive.
    app_log("snapshot command %.64s arg_bytes=%zu transport=%s acknowledged=%d applied=%d",
        command.name.c_str(), command.argument.size(), gc_client_result_text(static_cast<GcClientResult>(transport)), acknowledged, json.is_true("applied"));
    if (acknowledged && command.name == "RUNTIME_SET") {
        runtime_pending_.push_back(command);
        return;
    }
    push_toast(command_result(command.name, command.argument, reached, response));
}

void ServiceSnapshot::run_setup_begin(const Command &command, std::string &reply)
{
    auto complete = [&](ResultState state, bool reached, const std::string &body, const std::string &detail) {
        {
            std::lock_guard guard(lock_);
            fetches_[command.name] = Fetch{body, reached, ++fetch_generation_,
                state == ResultState::acknowledged, state, detail};
        }
        push_toast(detail);
    };
    // Fresh send-time metadata excludes cached options after loss/reconnect or
    // profile changes. The core then checks the token again at sample binding.
    fetch_status(reply);
    const auto status = last_reply("STATUS");
    fetch_endpoint("DEVICES", reply);
    const auto fresh = last_reply("DEVICES");
    std::vector<SetupSource> sources;
    const bool parsed = fresh.valid && parse_setup_sources(fresh.body, sources);
    const auto selected = std::find_if(sources.begin(), sources.end(), [&](const auto &source) {
        return source.selection == command.argument;
    });
    const auto capabilities = json_raw_value(status.body, "capabilities");
    if (!status.valid || json_raw_value(capabilities, "setup") != "true" ||
        json_raw_value(capabilities, "device_inventory") != "true" || !parsed || selected == sources.end()) {
        complete(ResultState::rejected, fresh.reached, "",
            "Setup not sent: selected connection or fresh inventory unavailable. Select the source again.");
        return;
    }
    if (!running_.load()) return;
    const bool reached = exchange(command.name.c_str(), command.argument.c_str(), reply, command.timeout_ms) == GC_CLIENT_OK;
    const std::string body = reached ? std::string(reply.c_str()) : std::string();
    const JsonReply ack(body);
    if (!reached || !ack.success() || !ack.is_true("active") || !ack.is_true("source_valid") ||
        ack.raw("source_connection_generation") != selected->connection_generation) {
        complete(reached ? ResultState::rejected : ResultState::transport_failed, reached, body,
            "Setup binding not confirmed. Check the selected source and begin again.");
        return;
    }
    fetch_endpoint("SETUP_STATUS", reply);
    const auto readback = last_reply("SETUP_STATUS");
    const JsonReply current(readback.body);
    const bool matched = readback.valid && current.is_true("active") && current.is_true("source_valid") &&
        current.raw("source_connection_generation") == selected->connection_generation;
    complete(matched ? ResultState::acknowledged : ResultState::readback_failed, true, body,
        matched ? "Setup bound to selected physical input; game output not confirmed." :
                  "Setup acknowledged; current source readback failed or changed. Check status before capture.");
}

void ServiceSnapshot::run_controller_output(const Command &command, std::string &reply)
{
    const auto pipe = command.argument.find('|');
    const std::string id = command.argument.substr(0, pipe);
    const std::string type = pipe == command.argument.npos ? "" : command.argument.substr(pipe + 1);
    auto complete = [&](ResultState state, bool reached, const std::string &body, const std::string &detail) {
        {
            std::lock_guard guard(lock_);
            auto &f = fetches_[command.name];
            f = Fetch{body, reached, ++fetch_generation_, state == ResultState::saved, state, detail};
        }
        push_toast(detail);
    };
    if (id.empty() || !controller_output_type(type)) {
        complete(ResultState::rejected, false, "", "Output preference not sent: invalid rule ID or output type."); return;
    }
    // Revalidate immediately before the write. A stale chooser must not recreate a removed rule.
    fetch_endpoint("CONTROLLERS", reply);
    auto fresh = last_reply("CONTROLLERS");
    std::vector<ControllerRule> rules;
    const bool parsed = fresh.valid && parse_controller_rules(fresh.body, rules);
    const auto exists = std::find_if(rules.begin(), rules.end(), [&](const auto &rule) { return rule.id == id; });
    if (!parsed || exists == rules.end() ||
        json_raw_value(json_raw_value(read().status_json, "capabilities"), "controller_rules") != "true") {
        complete(ResultState::rejected, fresh.reached, "", "Output preference not sent: rule missing or fresh controller rules unavailable."); return;
    }
    if (!running_.load()) return;
    const bool reached = exchange(command.name.c_str(), command.argument.c_str(), reply, command.timeout_ms) == GC_CLIENT_OK;
    const std::string body = reached ? std::string(reply.c_str()) : std::string();
    const JsonReply ack(body);
    if (!reached || !ack.success() || !ack.is_true("saved") || ack.raw("applied") != "false" ||
        ack.raw("output_supported") != "false" || ack.string_field("effect") != "controller_rule_persisted" ||
        ack.string_field("id") != id || ack.string_field("output_type") != type) {
        complete(reached ? ResultState::rejected : ResultState::transport_failed, reached, body,
                 "Output preference not verified: service reply failed or did not match."); return;
    }
    fetch_endpoint("CONTROLLERS", reply);
    fresh = last_reply("CONTROLLERS");
    const bool valid = fresh.valid && parse_controller_rules(fresh.body, rules);
    const auto saved = std::find_if(rules.begin(), rules.end(), [&](const auto &rule) {
        return rule.id == id && rule.output_type == type;
    });
    const bool matched = valid && saved != rules.end();
    complete(matched ? ResultState::saved : ResultState::readback_failed, true, body,
        matched ? "Output preference saved: " + id + " - " + controller_output_label(type) + "; live output unsupported." :
                  "Output preference acknowledged; controller rule readback failed or did not match. No live output confirmed.");
}

void ServiceSnapshot::run_setup_save(const Command &command, std::string &reply)
{
    const bool reached = exchange("SETUP_SAVE", nullptr, reply, command.timeout_ms) == GC_CLIENT_OK;
    const std::string body = reached ? std::string(reply.c_str()) : std::string();
    const JsonReply json(body);
    auto uint64_token = [](std::string_view text, bool nonzero) {
        if (text.empty()) return false;
        std::uint64_t n = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            const unsigned digit = c - '0';
            if (n > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
            n = n * 10 + digit;
        }
        return !nonzero || n != 0;
    };
    auto id_valid = [](const std::string &id) {
        return !id.empty() && id.size() <= 255 &&
            std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; });
    };
    const auto profile = json.string_field("profile_id"), controller = json.string_field("controller_id");
    const bool saved = json.valid && json.is_true("saved");
    const bool verified = saved && json.success() && json.is_true("accepted") && json.is_true("verified") &&
        json.raw("applied") == "false" && id_valid(profile) && id_valid(controller) &&
        uint64_token(json.raw("generation"), false) && uint64_token(json.raw("source_connection_generation"), true);
    Fetch f;
    f.body = body; f.reached = reached; f.valid = verified;
    f.state = !reached ? ResultState::transport_failed : verified ? ResultState::saved :
        saved ? ResultState::readback_failed : json.valid && json.raw("ok") == "false" ? ResultState::rejected : ResultState::readback_failed;
    f.detail = verified ? "Profile saved and verified: " + profile + "; controller " + controller +
        "; generation " + std::string(json.raw("generation")) + "; source connection " +
        std::string(json.raw("source_connection_generation")) + "; game output not confirmed." :
        saved ? "Profile save committed; readback failed or metadata unverified. Not rolled back; game output not confirmed. Check saved profiles before retrying." :
        !reached ? "Setup save unavailable: no service reply. Save outcome unknown; check saved profiles before retrying." :
        json.valid && json.raw("ok") == "false" ? "Setup save rejected; no save confirmed." :
        "Setup save unverified: invalid or incomplete reply. Save outcome unknown; check saved profiles before retrying.";
    {
        std::lock_guard guard(lock_);
        f.generation = ++fetch_generation_; fetches_["SETUP_SAVE"] = f;
        if (command.fetch_request_id) fetch_requests_[command.fetch_request_id] = f;
    }
    if (!command.fetch) push_toast(f.detail);
}

bool ServiceSnapshot::cancel_fetch_request(unsigned long long id)
{
    std::lock_guard guard(lock_);
    const auto it=std::find_if(commands_.begin(),commands_.end(),[&](const Command &c){return c.fetch_request_id==id;});
    if(!id || it==commands_.end()) return false;
    commands_.erase(it);
    auto &f=fetch_requests_[id]; f.state=ResultState::rejected; f.detail="cancelled before dispatch"; f.generation=++fetch_generation_;
    return true;
}

void ServiceSnapshot::run_audio(const Command &command,std::string &reply)
{
    auto complete=[&](Fetch f) {
        {
            std::lock_guard guard(lock_);
            f.generation=++fetch_generation_; fetches_[command.name]=f;
            if(command.fetch_request_id) fetch_requests_[command.fetch_request_id]=f;
        }
        if(!command.fetch) push_toast(f.detail);
    };
    auto fail=[&](const char *why) {complete(Fetch{"",false,0,false,ResultState::readback_failed,why});};
    auto decimal=[](std::string_view text,std::uint64_t &n) {
        if(text.empty() || (text.size()>1 && text.front()=='0')) return false;
        const auto r=std::from_chars(text.data(),text.data()+text.size(),n);
        return r.ec==std::errc{} && r.ptr==text.data()+text.size();
    };
    // Both observations are made for this request on the worker. Retained View
    // and newer automatic polls cannot authorize or complete an Audio action.
    if(exchange("STATUS",nullptr,reply,300)!=GC_CLIENT_OK) {fail("fresh service identity unavailable"); return;}
    const std::string identity(reply.c_str()); const JsonReply product(identity);
    std::uint64_t boot=0,pid=0,started=0;
    if(!product.success() || product.string_field("service_kind")!="app-native" ||
       product.raw("schema")!="1" || product.string_field("version")!="2.0.0-app-service-candidate" ||
       !decimal(product.raw("boot"),boot) || !boot || !decimal(product.raw("pid"),pid) || !pid ||
       !decimal(product.raw("started_ms"),started) || !started) {fail("incompatible fresh service identity"); return;}
    if(exchange("AUDIO_STATUS",nullptr,reply,300)!=GC_CLIENT_OK) {fail("fresh Audio unavailable"); return;}
    const std::string before(reply.c_str()); const JsonReply j(before);
    if(!j.success() || j.raw("audio_schema")!="1" || !j.is_true("service_wired") || j.string_field("instance")!=std::to_string(boot)) {fail("fresh Audio identity mismatch"); return;}
    {std::lock_guard guard(lock_); view_.audio_json=before; ++view_.generation;}
    if(command.argument=="STATUS") {complete(Fetch{before,true,0,true,ResultState::acknowledged,"Fresh Audio status"}); return;}
    std::string fence;
    for(const char *key:{"instance","revision","session","consent_generation","selection_generation","inventory_generation"}) {
        const auto value=j.string_field(key); std::uint64_t n=0;
        if(!decimal(value,n)) {fail("invalid Audio generation"); return;}
        if(!fence.empty()) fence+='|';
        fence+=value;
    }
    std::string endpoint="AUDIO_"+command.argument;
    if(command.argument=="SELECT") {
        const auto address=j.string_field("next_headset");
        if(!j.is_true("inventory_available") || address.empty()) {fail("Headset inventory unavailable or no typed headset option"); return;}
        fence+='|'+address;
    } else if(command.argument=="CONSENT") {
        const auto source=j.string_field("source_id");
        if(source.empty()) {fail("Authorized PCM source unavailable; no fallback"); return;}
        fence+='|'+source;
    } else if(command.argument=="VOLUME" || command.argument=="LATENCY") {
        std::uint64_t volume=0,latency=0,generation=0;
        if(!j.is_true("settings_available") || !decimal(j.raw("volume"),volume) || volume>127 ||
           !decimal(j.raw("latency_ms"),latency) || latency<40 || latency>500 || !decimal(j.string_field("settings_generation"),generation)) {fail("Fresh settings unavailable"); return;}
        if(command.argument=="VOLUME") volume=volume==127?0:volume+1;
        else latency=latency>=490?40:latency+10;
        fence+='|'+std::to_string(generation)+'|'+std::to_string(volume)+'|'+std::to_string(latency); endpoint="AUDIO_PREFS";
    } else if(command.argument!="START" && command.argument!="STOP" && command.argument!="REVOKE" && command.argument!="SUSPEND" && command.argument!="RETRY") {
        fail("Unknown Audio intent"); return;
    }
    if(exchange(endpoint.c_str(),fence.c_str(),reply,300)!=GC_CLIENT_OK) {fail("Audio action transport failed; refresh before retrying"); return;}
    const std::string action(reply.c_str()); const JsonReply ack(action);
    if(!ack.success()) {complete(Fetch{action,true,0,false,ResultState::rejected,"Audio action refused"}); return;}
    if(exchange("AUDIO_STATUS",nullptr,reply,300)!=GC_CLIENT_OK) {complete(Fetch{action,true,0,false,ResultState::readback_failed,"Accepted; own readback unavailable"}); return;}
    const std::string observed(reply.c_str()); const JsonReply readback(observed);
    bool same=readback.success() && readback.raw("audio_schema")=="1";
    const bool withdrawal=command.argument=="STOP" || command.argument=="REVOKE";
    if(withdrawal) {
        // Withdrawal is tied to this request's actual session/consent outcome,
        // not the independent passive inventory (or preference file) observer.
        auto effect=[&](const JsonReply &v) {
            const auto state=v.string_field("state");
            if(!v.success() || v.raw("audio_schema")!="1" || !v.is_true("service_wired") ||
               v.raw("pending")!="false" || (state!="idle" && state!="error" && state!="unavailable")) return false;
            if(command.argument=="REVOKE")
                return v.raw("consent_granted")=="false" && v.raw("consent_source")=="\"\"";
            return (j.raw("consent_granted")=="true" || j.raw("consent_granted")=="false") &&
                v.raw("consent_granted")==j.raw("consent_granted") && v.raw("consent_source")==j.raw("consent_source");
        };
        same=same && effect(ack) && effect(readback) && ack.is_true("accepted");
        for(const char *key:{"instance","revision","session","consent_generation","selection_generation"}) {
            std::uint64_t prior=0,after=0,observed_value=0;
            const bool typed=decimal(j.string_field(key),prior) && decimal(ack.string_field(key),after) &&
                decimal(readback.string_field(key),observed_value);
            const auto state=j.string_field("state");
            const bool advance=std::string_view(key)=="revision" ||
                (std::string_view(key)=="consent_generation" && command.argument=="REVOKE") ||
                (std::string_view(key)=="session" && (state=="pending" || state=="recovering" || state=="streaming" || state=="suspended"));
            same=same && typed && after==observed_value &&
                (!advance || prior!=UINT64_MAX) && after==prior+(advance?1u:0u);
        }
        same=same && !ack.raw("selected").empty() && ack.raw("selected")==j.raw("selected") &&
            ack.raw("selected")==readback.raw("selected");
    } else {
        for(const char *key:{"instance","revision","session","consent_generation","selection_generation","inventory_generation","settings_generation","selected","consent_source","volume","latency_ms"})
            same=same && !ack.raw(key).empty() && ack.raw(key)==readback.raw(key);
    }
    if(same) {std::lock_guard guard(lock_); view_.audio_json=observed; ++view_.generation;}
    complete(Fetch{action,true,0,same,same?ResultState::acknowledged:ResultState::readback_failed,
        same?"Audio request-specific readback verified; not target-verified":"Accepted; Audio changed before own readback"});
}

void ServiceSnapshot::run_fetch(const Command &command, std::string &reply)
{
    if (command.name == "AUDIO_UI") { run_audio(command, reply); return; }
    if (command.name == "SETUP_SAVE") { run_setup_save(command, reply); return; }
    const bool reached = exchange(command.name.c_str(), command.argument.empty() ? nullptr : command.argument.c_str(), reply, command.timeout_ms) == GC_CLIENT_OK;
    std::lock_guard guard(lock_);
    auto &f = fetches_[command.name];
    f.body = reached ? std::string(reply.c_str()) : std::string();
    f.reached = reached; f.valid = JsonReply(f.body).success();
    f.state = !reached ? ResultState::transport_failed : f.valid ? ResultState::acknowledged : ResultState::rejected;
    f.generation = ++fetch_generation_;
    if (command.fetch_request_id) fetch_requests_[command.fetch_request_id] = f;
}

void ServiceSnapshot::fetch_endpoint(const char *name, std::string &reply)
{
    // Polls have a bounded budget independent of long user-request deadlines.
    const auto result = exchange(name, nullptr, reply, 300);
    const bool reached = result == GC_CLIENT_OK;
    const std::string body = reached ? std::string(reply.c_str()) : std::string();
    const JsonReply json(body);
    const bool status = std::strcmp(name, "STATUS") == 0;
    const bool valid = status ? json.valid && json.raw("ok") != "false" : json.success();
    if (status) log_link_result(valid, valid ? "ok" : reached ? "invalid status reply" : gc_client_result_text(static_cast<GcClientResult>(result)));
    std::lock_guard guard(lock_);
    auto &f = fetches_[name];
    f.body = body; f.reached = reached; f.valid = valid;
    f.state = !reached ? ResultState::transport_failed : valid ? ResultState::acknowledged : ResultState::rejected;
    f.generation = ++fetch_generation_;
    if (status) {
        ++view_.polls; view_.link = valid ? Link::up : Link::down;
        view_.detail = valid ? "" : reached ? "Invalid status reply" : gc_client_result_text(static_cast<GcClientResult>(result));
        if (valid) view_.status_json = body;
    }
    if (!valid) return; // retain this endpoint's last-good view, not fresh validity
    if (std::strcmp(name, "RUNTIME_GET") == 0) view_.runtime_json = body;
    else if (std::strcmp(name, "BLUETOOTH_STATUS") == 0) { view_.bluetooth_json = body; view_.bluetooth_paired = parse_bluetooth_paired(body); }
    else if (std::strcmp(name, "PROFILES") == 0) view_.profiles_json = body;
    else if (std::strcmp(name, "CONTROLLERS") == 0) view_.controllers_json = body;
    else if (std::strcmp(name, "DEVICES") == 0) view_.devices_json = body;
    else if (std::strcmp(name, "CONSOLE_ID") == 0) view_.console_id_json = body;
    else if (std::strcmp(name, "LOG_TAIL") == 0) view_.log_tail_json = body;
    else if (std::strcmp(name, "DUALSENSE_FX_STATUS") == 0) view_.fx_json = body;
    else if (std::strcmp(name, "SETUP_STATUS") == 0) view_.setup_json = body;
    else if (std::strcmp(name, "AUDIO_STATUS") == 0) view_.audio_json = body;
    else if (std::strcmp(name, "SETUP_INPUT") == 0) live_input_ = body;
    ++view_.generation;
}

void ServiceSnapshot::readback_runtime(std::string &reply)
{
    fetch_endpoint("RUNTIME_GET", reply);
    auto fresh = last_reply("RUNTIME_GET");
    JsonReply json(fresh.body);
    // Last write wins per key. All writes were delivered before this readback;
    // only the final value of a repeated key can be verified by this reply.
    std::map<std::string, std::string> expected;
    for (const auto &cmd : runtime_pending_) {
        auto at = cmd.argument.find('=');
        if (at != std::string::npos) expected[cmd.argument.substr(0, at)] = cmd.argument.substr(at + 1);
    }
    bool matched = fresh.valid && expected.size() > 0;
    for (const auto &[key, value] : expected) {
        auto raw = json.raw(key);
        const bool match = raw == value || json.string_field(key) == value ||
            (value == "1" && raw == "true") || (value == "0" && raw == "false");
        matched = matched && match;
    }
    {
        std::lock_guard guard(lock_);
        auto &f = fetches_["RUNTIME_SET"];
        if (f.state == ResultState::acknowledged) {
            f.state = matched ? ResultState::applied : ResultState::readback_failed;
            f.detail = matched ? "Runtime setting verified by readback" : "Acknowledged; runtime readback failed or did not match";
            f.generation = ++fetch_generation_;
        }
    }
    app_log("snapshot runtime readback writes=%zu valid=%d matched=%d", runtime_pending_.size(), fresh.valid, matched);
    push_toast(matched ? "Setting saved." : "Request accepted. Couldn't confirm the saved setting. Check status before retrying.");
    runtime_pending_.clear();
}

void ServiceSnapshot::fetch_status(std::string &reply) { fetch_endpoint("STATUS", reply); }
void ServiceSnapshot::fetch_fx(std::string &reply) { fetch_endpoint("DUALSENSE_FX_STATUS", reply); }

} // namespace ghostcontrol

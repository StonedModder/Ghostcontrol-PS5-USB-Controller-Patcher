/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "screens.hpp"
#include "service_snapshot.hpp"
#include "core/input.hpp"
#include <functional>

namespace ghostcontrol {
// Capability discovery is separate from IPC reachability. Never present an
// unavailable operation as a working modal merely because STATUS answered.
inline const char *command_capability(std::string_view command)
{
    if (command == "RUNTIME_SET" || command == "RELOAD" || command == "LOG_TAIL") return "persistence";
    if (command == "START" || command == "STOP" || command == "RESTART" ||
        command == "DUALSENSE_RUMBLE" || command == "DUALSENSE_RUMBLE_STOP") return "native_pad";
    if (command == "PROFILES" || command.starts_with("PROFILE_")) return "profiles";
    if (command.starts_with("SETUP_")) return "setup";
    if (command.starts_with("BLUETOOTH_")) return "bluetooth";
    if (command == "UPDATE_CHECK") return "update_check";
    if (command.starts_with("UPDATE_")) return "update_execution";
    if (command == "CONSOLE_ID") return "console_id";
    if (command == "CONTROLLER_SAVE" || command == "CONTROLLERS" ||
        command == "CONTROLLER_OUTPUT_SET") return "controller_rules";
    if (command == "DEVICES") return "device_inventory";
    if (command == "REBIND" || command == "FORCE_VDA") return "usb_vda";
    return nullptr;
}
inline bool row_capability_supported(const Row &row, const ServiceSnapshot::View &view)
{
    if (row.starts_companion || row.stops_companion || row.restarts_companion)
        return true; // The lifecycle worker probes for itself; no snapshot capability gates it.
    const char *command = row.overlay_command ? row.overlay_command : row.command;
    if (!command) return true; // Readers and local telemetry need no write capability.
    const char *capability = command_capability(command);
    if (!capability) return true;
    if (json_raw_value(json_raw_value(view.status_json, "capabilities"), capability) == "true")
        return true;
    // Reachability does not grant a provider. A real resident reports these
    // capabilities even while its radio is failed/waiting; a diagnostic peer
    // explicitly reports false. Keep supported Pair/Retry usable, but never
    // turn an unsupported peer into a working controller backend.
    return false;
}
inline const char *toggle_key(LiveSource source)
{
    switch (source) {
    case LiveSource::rt_toggle_autostart: return "autostart";
    case LiveSource::rt_toggle_background: return "run_in_background";
    case LiveSource::rt_toggle_ghostpad: return "ghostpad_enabled";
    case LiveSource::rt_toggle_wireless: return "wireless_ds4";
    case LiveSource::rt_toggle_fx: return "dualsense_fx_enabled";
    case LiveSource::rt_toggle_include_prereleases: return "include_prereleases";
    default: return nullptr;
    }
}
inline std::string resolve_toggle_argument(LiveSource source, std::string_view runtime)
{
    const char *key = toggle_key(source);
    if (!key) return {};
    const auto value = json_raw_value(runtime, key);
    if (value != "true" && value != "false") return {};
    return std::string(key) + (value == "true" ? "=0" : "=1");
}
inline std::string boolean_reading(std::string_view json, std::string_view key)
{
    const auto value = json_raw_value(json, key);
    return value == "true" ? "On" : value == "false" ? "Off" : "-";
}

/** No rendering/network dependency: the shell and tests use this same press
 * path. Enqueue is the nonblocking boundary, not a fabricated service reply.
 * A queued toggle stays pending until an explicitly captured RUNTIME_GET
 * arrives; whole-view generation alone is NOT authoritative acknowledgement. */
class RowActions {
public:
    enum class Result { none, rejected, queued, overlay, reader };
    using Enqueue = std::function<bool(const char *, const char *, unsigned)>;
    Result press(const hui::InputFrame &input, const Row &row,
                 const ServiceSnapshot::View &view, bool fresh,
                 const Enqueue &enqueue, unsigned long long fetch_generation = 0,
                 std::string dynamic_argument = {}, unsigned long long acknowledgement_generation = 0)
    {
        if (!input.is_pressed(hui::Action::confirm)) return Result::none;
        if (row.disabled || row.state == RowState::pending) return Result::rejected;
        const bool writable = row.command || row.overlay != OverlayKind::none;
        if (row.read_only && writable) return Result::rejected;
        if (writable && (view.link != ServiceSnapshot::Link::up || !fresh))
            return Result::rejected;
        if (writable && pending_) return Result::rejected;
        if (writable && !row_capability_supported(row, view)) return Result::rejected;
        if (row.overlay != OverlayKind::none) return Result::overlay;
        if (!row.command) {
            if (row.reader != ReaderKind::none || !row.detail.empty()) return Result::reader;
            return Result::rejected;
        }
        const char *key = toggle_key(row.command_argument);
        std::string argument = row.argument ? row.argument : "";
        if (key) {
            if (runtime_unavailable_) return Result::rejected;
            argument = resolve_toggle_argument(row.command_argument, runtime(view));
            if (argument.empty()) return Result::rejected;
        } else if (row.command_argument != LiveSource::none) {
            argument = std::move(dynamic_argument);
            if (argument.empty()) return Result::rejected;
        }
        if (!enqueue || !enqueue(row.command, argument.empty() ? nullptr : argument.c_str(),
                                 row.command_timeout_ms)) return Result::rejected;
        if (key) {
            pending_ = true;
            pending_key_ = key;
            baseline_fetch_ = fetch_generation;
            baseline_acknowledgement_ = acknowledgement_generation;
            baseline_runtime_ = view.runtime_json;
        }
        return Result::queued;
    }
    bool pending() const { return pending_; }
    /** A poll which happens to finish before the command must not unlock the
     * row. ServiceSnapshot captures command and endpoint replies on its shared
     * monotonic fetch counter. Require both fences before displaying read-back. */
    bool observe_after_command(const ServiceSnapshot::Fetch &acknowledgement,
                               const ServiceSnapshot::Fetch &fetch,
                               const ServiceSnapshot::View *current = nullptr)
    {
        if (pending_) {
            if (acknowledgement.generation <= baseline_acknowledgement_) return false;
            // The worker publishes final completion AFTER its own readback.
            // That verified readback necessarily has an older generation.
            const bool completed = acknowledgement.state == ServiceSnapshot::ResultState::applied ||
                                   acknowledgement.state == ServiceSnapshot::ResultState::readback_failed;
            if (!completed && fetch.generation <= acknowledgement.generation) return false;
        }
        return observe(fetch, current);
    }
    /** Ignore cached replies. Return true only when a fresh completion arrives.
     * A failed/invalid read-back disables subsequent toggles until recovery. */
    bool observe(const ServiceSnapshot::Fetch &fetch, const ServiceSnapshot::View *current = nullptr)
    {
        if (fetch.generation <= baseline_fetch_) return false;
        baseline_fetch_ = fetch.generation;
        if (current) baseline_runtime_ = current->runtime_json;
        if (!fetch.reached || json_raw_value(fetch.body, "ok") == "false" ||
            (!pending_key_.empty() && boolean_reading(fetch.body, pending_key_) == "-")) {
            runtime_unavailable_ = true;
            authoritative_runtime_.clear();
        } else {
            runtime_unavailable_ = false;
            authoritative_runtime_ = fetch.body;
        }
        pending_ = false;
        return true;
    }
    void reconcile(const ServiceSnapshot::View &view)
    {
        // A different polled payload supersedes a captured read-back. The
        // unchanged last-good payload must not overwrite a fresh read-back.
        if (!pending_ && view.runtime_json != baseline_runtime_) {
            authoritative_runtime_.clear();
            baseline_runtime_ = view.runtime_json;
        }
    }
    std::string_view runtime(const ServiceSnapshot::View &view) const
    {
        if (runtime_unavailable_) return {};
        return authoritative_runtime_.empty() ? std::string_view(view.runtime_json)
                                             : std::string_view(authoritative_runtime_);
    }
    bool unavailable() const { return runtime_unavailable_; }
    std::string reading(const Row &row, const ServiceSnapshot::View &view, bool fresh = true) const
    {
        if (!fresh || view.link != ServiceSnapshot::Link::up) return "-";
        const char *key = toggle_key(row.command_argument);
        if (!key) return "-";
        return boolean_reading(runtime(view), key);
    }
private:
    bool pending_ = false;
    bool runtime_unavailable_ = false;
    unsigned long long baseline_fetch_ = 0;
    unsigned long long baseline_acknowledgement_ = 0;
    std::string pending_key_;
    std::string baseline_runtime_;
    std::string authoritative_runtime_;
};
} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "toolbox_ui.hpp"
#include "bt_diagnostic_reader.hpp"
#include "json_reply.hpp"
#include "overlay_dispatch.hpp"

#include "app_log.hpp"
#include "core/save_file.hpp"
#include "core/tween.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace ghostcontrol
{
namespace
{

using hui::Action;
using hui::Direction;
using hui::audio::Cue;
using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

// Saved configuration, not observed animation output. Prefer the authoritative
// runtime readback so a completed modal does not reopen from an older FX poll.
std::string_view fx_setting(const ServiceSnapshot::View &service, std::string_view field)
{
    const auto saved = json_raw_value(service.runtime_json, std::string("dualsense_fx_") + std::string(field));
    if (!saved.empty()) return saved;
    return json_raw_value(service.fx_json, field);
}

/* Counts the top-level objects in a named JSON array -- the service's
 * "devices", "paired" and "controllers" lists. Strings are skipped so a brace
 * inside a controller's name is not mistaken for an element. */
std::size_t count_array_objects(std::string_view json, std::string_view key)
{
    const std::string_view array = json_raw_value(json, key);
    if (array.empty() || array.front() != '[')
        return 0;
    std::size_t count = 0;
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 1; i < array.size(); ++i)
    {
        const char c = array[i];
        if (in_string)
        {
            if (c == '\\')
                ++i;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '{')
        {
            if (depth == 0)
                ++count;
            ++depth;
        }
        else if (c == '}' && depth > 0)
            --depth;
        else if (c == ']' && depth == 0)
            break;
    }
    return count;
}

/* How many slots report "connected":true. STATUS carries no count of its own,
 * so it is read from the slot objects; the reply's only other "connected" is
 * the wireless bridge, and that one is a number, never true. */
std::vector<std::string_view> json_array_elements(std::string_view json, std::string_view key);
std::size_t count_connected_slots(std::string_view status_json)
{
    std::size_t count = 0;
    for (const auto object : json_array_elements(status_json, "slots"))
        if (json_raw_value(object, "connected") == "true") ++count;
    return count;
}

/* %04x without stdio: lowercase hex, fixed width. */
std::string hex_fixed(unsigned long value, int digits)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i)
    {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xfu];
        value >>= 4;
    }
    return out;
}

/* Whole-string hex, as the service's hex-carrying arguments (PROFILE_RENAME,
 * SETUP_BEGIN) expect. Lowercase, two digits per byte. */
std::string hex_encode(std::string_view text)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char c : text)
    {
        out += kDigits[c >> 4];
        out += kDigits[c & 15];
    }
    return out;
}

/* Readers require a validated explicit success, not absence of one spelling
 * of failure. Whitespace, nested fields and quoted text cannot spoof it. */
bool reply_ok(std::string_view json)
{
    return json_raw_value(json, "ok") == "true";
}

/* Decodes the hex a LOG_TAIL reply's "data" carries back to log text. An odd
 * length or a non-hex digit means the reply was not usable rather than a
 * wrong string, so it fails instead of producing half a line. */
bool decode_hex(std::string_view hex, std::string &out)
{
    if (hex.empty() || (hex.size() % 2) != 0)
        return false;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int high = nibble(hex[i]);
        const int low = nibble(hex[i + 1]);
        if (high < 0 || low < 0)
            return false;
        out.push_back(static_cast<char>(high * 16 + low));
    }
    return true;
}

/* Splits decoded log text into lines, dropping a carriage return so a log
 * written on Windows does not draw a stray glyph, and the empty tail a final
 * newline leaves behind. */
std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string line(text.substr(start, end - start));
        while (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == text.size())
            break;
        start = end + 1;
    }
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

/* The top-level elements of a named JSON array, each as a view into `json`.
 * Objects arrive whole, braces included, ready for json_value(); strings
 * arrive without their quotes. The scan is bounded by the view it is given,
 * so a cut reply yields the elements that did arrive and nothing invented --
 * the same truncation-is-normal stance the snapshot's own reader takes.
 *
 * A small scanner rather than a parser because the app links no JSON library
 * and only a couple of fields are ever read this way. */
std::vector<std::string_view> json_array_elements(std::string_view json, std::string_view key)
{
    std::vector<std::string_view> elements;
    const auto array = json_raw_value(json, key);
    if (array.empty() || array.front() != '[') return elements;
    std::size_t i = 1;
    while (i < array.size()) {
        json_detail::skip_space(array, i);
        if (i == array.size() || array[i] == ']') break;
        const auto start = i;
        if (!json_detail::value_end(array, i)) return {};
        auto element = array.substr(start, i - start);
        if (element.front() == '"') element = element.substr(1, element.size() - 2);
        elements.push_back(element);
        json_detail::skip_space(array, i);
        if (i < array.size() && array[i] == ',') ++i;
    }
    return elements;
}

/* A boolean field as On/Off. Missing reads as "-", never as Off. */
std::string bool_text(std::string_view json, std::string_view key)
{
    return boolean_reading(json, key);
}

/* The STATUS entry for one slot, from its '{' to the matching '}'. Slot
 * objects carry no nesting and the reply is machine-written, so the next '}'
 * closes it. Used by the slot rows and by the setup selection. */
std::string_view slot_object(std::string_view status_json, int index)
{
    for (const auto object : json_array_elements(status_json, "slots"))
        if (json_raw_value(object, "slot") == std::to_string(index)) return object;
    return {};
}

/* One line for a slot row: what is there, and whether it has a user. */
std::string slot_summary(std::string_view status_json, int index)
{
    const std::string_view object = slot_object(status_json, index);
    if (object.empty())
        return "-";
    if (json_value(object, "connected") != "true")
        return "empty";
    const std::string_view name = json_value(object, "name");
    const std::string_view vid_pid = json_value(object, "vid_pid");
    std::string text = name.empty() ? std::string("Controller") : std::string(name);
    if (!vid_pid.empty())
        text += " [" + std::string(vid_pid) + "]";
    const JsonReply slot(object);
    const auto bound = slot.raw("bound_user");
    const auto known = slot.raw("bound_user_known");
    // Preserve the service's token. A handle or streaming report does not
    // prove virtual identity creation or the console's assignment workflow.
    // bound_user_known false is unknown assignment, not a successful unassigned claim.
    bool numeric = !bound.empty();
    for (char c : bound) numeric = numeric && c >= '0' && c <= '9';
    if (known == "false")
        text += " — user unknown";
    else
        text += numeric ? " — reported user " + std::string(bound) + "" :
            bound == "-1" ? " — no user assigned" : " — user unknown";
    text += " · virtual pad and game input not verified";
    return text;
}

/* ---- BT-R01 Bluetooth diagnostics readings ----
 * Everything comes from BLUETOOTH_STATUS "diagnostics", which the service
 * fills from its own transport, host and engine counters. An older service
 * without the object says so instead of showing zeros. */
std::string_view bt_diagnostics(const ServiceSnapshot::View &service)
{
    return json_raw_value(service.bluetooth_json, "diagnostics");
}

/* A non-negative integer token, or -1 (null, missing or malformed). */
long long json_count(std::string_view json, std::string_view key)
{
    const auto raw = json_raw_value(json, key);
    if (raw.empty() || raw.size() > 18) return -1;
    long long value = 0;
    for (char c : raw) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

std::string age_text(long long ms)
{
    if (ms < 1000) return "just now";
    if (ms < 60000) return std::to_string(ms / 1000) + " s ago";
    if (ms < 3600000) return std::to_string(ms / 60000) + " min ago";
    return std::to_string(ms / 3600000) + " h ago";
}

/* An age member: null is the service saying "never"; anything else that is
 * not a non-negative integer is unknown, never "never" or "now". Ages are as
 * of the service's reply, which the worker polls about every 2 s. */
std::string age_member(std::string_view json, std::string_view key)
{
    if (json_raw_value(json, key) == "null") return "never";
    const auto ms = json_count(json, key);
    return ms < 0 ? std::string("time unknown") : age_text(ms);
}

/* The engine refreshes its snapshot every few milliseconds; an old one means
 * the service's Bluetooth thread stopped, so every reading says so. A missing
 * or malformed age cannot be shown as fresh. */
std::string bt_stale_suffix(std::string_view diagnostics)
{
    const auto age = json_count(diagnostics, "sampleAgeMs");
    if (age < 0) return " · age unknown";
    return age > 3000 ? " · stale (" + age_text(age) + ")" : std::string();
}

std::string bt_reading(LiveSource source, const ServiceSnapshot::View &service)
{
    if (service.bluetooth_json.empty()) return "-";
    const auto diagnostics = bt_diagnostics(service);
    if (diagnostics.empty()) return "Not reported by Ghosttooth API";
    if (diagnostics == "null") return "Unavailable";
    const auto stale = bt_stale_suffix(diagnostics);
    const auto radio = json_raw_value(diagnostics, "radio");
    const auto failures = json_raw_value(diagnostics, "failures");
    switch (source) {
    case LiveSource::bt_radio: {
        if (radio == "null") return "Never opened" + stale;
        if (radio.empty()) return "-";
        const auto usb = json_value(radio, "usb");
        std::string text = usb.empty() || usb == "null" ? std::string("USB id unknown") : "USB " + std::string(usb);
        const auto iface = json_raw_value(radio, "interface");
        if (!iface.empty() && iface != "-1") text += " · interface " + std::string(iface);
        const auto open = json_raw_value(diagnostics, "radioOpen");
        text += open == "true" ? " · open" : open == "false" ? " · closed (last session)" : " · state unknown";
        return text + stale;
    }
    case LiveSource::bt_traffic: {
        if (radio == "null") return "None measured" + stale;
        const auto counters = json_raw_value(radio, "counters");
        const auto events = json_count(counters, "events"), acl = json_count(counters, "acl");
        if (events < 0 || acl < 0) return "-";
        return std::to_string(events) + " events · " + std::to_string(acl) + " ACL · heard " +
               age_member(radio, "lastRxAgeMs") + stale;
    }
    case LiveSource::bt_drops: {
        if (radio == "null") return "None measured" + stale;
        const auto counters = json_raw_value(radio, "counters");
        struct Kind { const char *key, *label; };
        static constexpr Kind kinds[] = {
            {"aclStale", "stale handle"}, {"aclMalformed", "malformed"},
            {"eventQueueDrops", "event queue"}, {"aclQueueDrops", "ACL queue"},
            {"partialEventDrops", "partial event"}, {"txNoCredit", "no send buffer"},
            {"txFailed", "send refused"}};
        long long total = 0;
        std::string detail;
        for (const auto &kind : kinds) {
            const auto n = json_count(counters, kind.key);
            if (n < 0) return "-";
            if (!n) continue;
            total += n;
            detail += (detail.empty() ? "" : ", ") + std::to_string(n) + " " + kind.label;
        }
        return (total ? std::to_string(total) + " dropped (" + detail + ")" : std::string("None dropped")) + stale;
    }
    case LiveSource::bt_last_failure: {
        if (failures.empty()) return "-";
        const auto losses = json_count(failures, "transportLosses");
        const auto open_failures = json_count(failures, "openFailures");
        const auto last_raw = json_raw_value(failures, "last");
        if (losses < 0 || open_failures < 0 || last_raw.empty()) return "-"; /* never invent zero */
        const auto last = json_value(failures, "last");
        std::string text = last_raw == "null" ? std::string("None")
                                              : std::string(last) + " · " + age_member(failures, "lastAgeMs");
        auto counted = [](long long n, const char *one, const char *many) {
            return std::to_string(n) + " " + (n == 1 ? one : many);
        };
        if (losses > 0 || open_failures > 0)
            text += " (" + counted(open_failures, "open failure", "open failures") + ", " +
                    counted(losses, "loss", "losses") + ")";
        return text + stale;
    }
    default:
        return "-";
    }
}

/* Turns a row's LiveSource into the text it should show, reading the reply the
 * enum names. A source with no field in the current snapshot comes back as
 * "-", which the row already knows how to render as "no reading". */
std::string resolve_live(LiveSource source, const ServiceSnapshot::View &service)
{
    switch (source)
    {
    case LiveSource::audio_inventory: {
        const JsonReply j(service.audio_json);
        if(!j.success()) return "Unavailable";
        unsigned headsets=0,pads=0,unknown=0;
        for(const auto peer:json_array_elements(service.audio_json,"peers")) {
            const auto kind=json_value(peer,"kind");
            if(kind=="headset") ++headsets; else if(kind=="pad") ++pads; else ++unknown;
        }
        const auto saved=json_array_elements(service.audio_json,"saved_preferences").size();
        return (j.is_true("inventory_available")?std::to_string(headsets)+" headsets / "+std::to_string(pads)+" pads / "+std::to_string(unknown)+" unknown":"Headset list unavailable")+
            " / "+std::to_string(saved)+" saved devices"+(j.is_true("saved_inventory_truncated")?" (partial list)":"");
    }
    case LiveSource::audio_source: {
        const JsonReply j(service.audio_json);
        return j.success()?(j.string_field("source_id").empty()?"Audio input unavailable":j.string_field("source_id")+(j.is_true("consent_granted")?" / allowed":" / permission needed")):"Unavailable";
    }
    case LiveSource::audio_preferences: {
        const JsonReply j(service.audio_json);
        return j.is_true("settings_available")?"Volume "+std::string(j.raw("volume"))+" / "+std::string(j.raw("latency_ms"))+" ms / settings version "+j.string_field("settings_generation")+" / saved only":"Choose a device to view settings";
    }
    case LiveSource::audio_state: {
        const JsonReply j(service.audio_json);
        return j.success()?j.string_field("state")+" / "+j.string_field("selected")+" / "+j.string_field("seam_blocking"):"Audio status unavailable";
    }
    case LiveSource::service_version:
    {
        const std::string_view found = json_value(service.status_json, "version");
        return found.empty() ? std::string("-") : std::string(found);
    }
    case LiveSource::active_slots:
        if (service.status_json.empty())
            return "-";
        return std::to_string(count_connected_slots(service.status_json));
    case LiveSource::service_state:
    {
        const std::string_view found = json_value(service.status_json, "service");
        return found.empty() ? std::string("-") : std::string(found);
    }
    case LiveSource::bluetooth_state:
    {
        const std::string_view found = json_value(service.bluetooth_json, "bluetooth");
        const std::string_view reason = json_value(service.bluetooth_json, "reason");
        const std::string_view pairing = json_value(service.bluetooth_json, "pairing");
        long seconds = 0;
        if (!pairing.empty())
            seconds = std::strtol(std::string(pairing).c_str(), nullptr, 10);
        if (seconds > 0)
            return "Searching · " + std::to_string(seconds) + " s";
        if (found.empty())
            return "-";
        if (found == "failed")
            return reason.empty() ? std::string("Radio did not open") : std::string(reason);
        if (found == "stopped")
            return "Off";
        if (found == "trying")
            return "Opening the radio";
        if (found == "ready")
            return "Ready";
        return std::string(found);
    }
    case LiveSource::paired_count:
        if (service.bluetooth_json.empty())
            return "-";
        return std::to_string(count_array_objects(service.bluetooth_json, "paired"));
    case LiveSource::connected_count:
        if (service.bluetooth_json.empty())
            return "-";
        return std::to_string(count_array_objects(service.bluetooth_json, "controllers"));
    case LiveSource::active_profile:
    {
        const std::string_view found = json_value(service.runtime_json, "active_profile");
        return found.empty() ? std::string("-") : std::string(found);
    }
    case LiveSource::fx_summary:
    {
        const std::string_view effect = fx_setting(service, "effect");
        if (effect.empty())
            return "-";
        static const char *const kEffectNames[] = {"Solid", "Breathe", "Rainbow", "Police",
                                                   "Strobe"};
        for (unsigned index = 0; index < 5; ++index)
            if (effect == std::to_string(index)) return kEffectNames[index];
        return "-";
    }
    case LiveSource::console_serial:
    {
        const std::string_view found = json_value(service.console_id_json, "serial");
        return found.empty() ? std::string("-") : std::string(found);
    }
    case LiveSource::console_openpsid:
    {
        const std::string_view found = json_value(service.console_id_json, "openpsid");
        return found.empty() ? std::string("-") : std::string(found);
    }
    case LiveSource::devices_summary:
        if (service.devices_json.empty())
            return "-";
        return std::to_string(count_array_objects(service.devices_json, "devices"));
    case LiveSource::slot1:
    case LiveSource::slot2:
    case LiveSource::slot3:
    case LiveSource::slot4:
        if (service.status_json.empty())
            return "-";
        return slot_summary(service.status_json,
                            static_cast<int>(source) - static_cast<int>(LiveSource::slot1));
    case LiveSource::runtime_autostart:
        return bool_text(service.runtime_json, "autostart");
    case LiveSource::runtime_background:
        return bool_text(service.runtime_json, "run_in_background");
    case LiveSource::runtime_wireless:
        return bool_text(service.runtime_json, "wireless_ds4");
    case LiveSource::runtime_ghostpad:
    {
        const std::string_view enabled = json_value(service.runtime_json, "ghostpad_enabled");
        if (enabled.empty())
            return "-";
        if (enabled != "true")
            return "Off";
        return json_value(service.runtime_json, "ghostpad_listening") == "true"
                   ? "On · listening"
                   : "On";
    }
    case LiveSource::runtime_fx_enabled:
        return bool_text(service.runtime_json, "dualsense_fx_enabled");
    case LiveSource::fx_color:
    {
        const std::string_view r = fx_setting(service, "r");
        const std::string_view g = fx_setting(service, "g");
        const std::string_view b = fx_setting(service, "b");
        if (r.empty() || g.empty() || b.empty())
            return "-";
        return std::string(r) + ", " + std::string(g) + ", " + std::string(b);
    }
    case LiveSource::fx_period_ms:
    {
        const std::string_view found = fx_setting(service, "period_ms");
        return found.empty() ? std::string("-") : std::string(found) + " ms";
    }
    case LiveSource::motion_state:
    {
        // main.cpp exposes no app-native motion sample or freshness contract.
        // Neither service FX telemetry nor an observed/valid boolean proves
        // this capability. Keep unsupported distinct from a valid idle sample.
        return "No motion";
    }
    case LiveSource::console_model:
    {
        const std::string_view found = json_value(service.console_id_json, "model");
        return found.empty() ? std::string("-") : std::string(found);
    }
    case LiveSource::setup_message:
    {
        const std::string_view found = json_value(service.setup_json, "message");
        return found.empty() ? std::string("-") : std::string(found);
    }
    case LiveSource::setup_state:
    {
        const std::string_view active = json_value(service.setup_json, "active");
        if (active.empty())
            return "-";
        return active == "true" ? "in progress" : "idle";
    }
    case LiveSource::bluetooth_pairing:
    {
        const std::string_view pairing = json_value(service.bluetooth_json, "pairing");
        if (pairing.empty())
            return "-";
        const long seconds = std::strtol(std::string(pairing).c_str(), nullptr, 10);
        return seconds > 0 ? std::to_string(seconds) + " s" : "closed";
    }
    case LiveSource::profile_count:
        if (service.profiles_json.empty())
            return "-";
        return std::to_string(json_array_elements(service.profiles_json, "profiles").size());
    case LiveSource::controller_count:
        if (service.controllers_json.empty())
            return "-";
        return std::to_string(json_array_elements(service.controllers_json, "controllers").size());
    case LiveSource::bt_radio:
    case LiveSource::bt_traffic:
    case LiveSource::bt_drops:
    case LiveSource::bt_last_failure:
        return bt_reading(source, service);
    case LiveSource::native_pad_available:
        if (json_value(service.native_pad_json, "observed") != "true") return "No controller";
        return json_value(service.native_pad_json, "connected") == "true" ? "1 controller" : "No controller";
    case LiveSource::native_light_result:
    {
        if (json_value(service.native_pad_json, "producer_present") == "false")
            return "Not in this build";
        if (json_value(service.native_pad_json, "observed") != "true") return "No controller";
        auto rc_text = [](std::string_view rc) {
            if (rc.empty() || rc == "-2147483647") return std::string("not requested");
            return rc == "0" ? std::string("request accepted") : "error (" + std::string(rc) + ")";
        };
        const char *connection = json_value(service.native_pad_json, "connected") == "true"
                                     ? "connected" : "disconnected";
        return std::string(connection) + "; light " +
               rc_text(json_value(service.native_pad_json, "light_rc")) + "; reset " +
               rc_text(json_value(service.native_pad_json, "reset_rc"));
    }
    case LiveSource::native_rumble_result:
    {
        if (json_value(service.native_pad_json, "producer_present") == "false")
            return "Rumble is not available in this build";
        if (json_value(service.native_pad_json, "observed") != "true")
            return "Rumble unavailable";
        if (json_value(service.native_pad_json, "connected") != "true")
            return "Rumble unavailable";
        const auto phase = json_value(service.native_pad_json, "phase");
        const bool started = json_value(service.native_pad_json, "mode_rc") == "0" &&
                             json_value(service.native_pad_json, "start_rc") == "0";
        if (phase == "1") return started ? "Rumble request accepted" : "Rumble start not confirmed";
        if (phase == "2") return started && json_value(service.native_pad_json, "stop_rc") == "0"
                                    ? "Test request complete" : "Rumble stop not confirmed";
        if (phase == "3") return "Test failed";
        if (phase == "4") return "Test cancelled";
        return "Not tested yet";
    }
    case LiveSource::none:
    default:
        return "-";
    }
}

/* Builds a row's argument at press time when it depends on live state.
 * A toggle reads the current value and asks for the opposite; the setup
 * selection names the first connected controller's slot. An empty result
 * means the press cannot be served right now, and the shell refuses it
 * with the same cue as a rejected queue. */
std::string resolve_argument(LiveSource source, const ServiceSnapshot::View &service)
{
    if (toggle_key(source))
        return resolve_toggle_argument(source, service.runtime_json);

    switch (source)
    {
    case LiveSource::setup_begin_first_pad:
    {
        for (int index = 0; index < 4; ++index)
        {
            const std::string_view object = slot_object(service.status_json, index);
            if (object.empty() || json_value(object, "connected") != "true")
                continue;
            const std::string_view path = json_value(object, "device_path");
            const std::string_view vid = json_value(object, "vid");
            const std::string_view pid = json_value(object, "pid");
            if (path.empty() || vid.empty() || pid.empty())
                return {};
            const unsigned long vid_number = std::strtoul(std::string(vid).c_str(), nullptr, 10);
            const unsigned long pid_number = std::strtoul(std::string(pid).c_str(), nullptr, 10);
            /* The reply carries vid/pid as decimal; SETUP_BEGIN wants hex,
             * and the profile ID must be a fresh, allowed name. */
            const std::string selection = std::string(path) + "|" + hex_fixed(vid_number, 4) +
                                          "|" + hex_fixed(pid_number, 4) + "|pad-" +
                                          hex_fixed(vid_number, 4) + "-" +
                                          hex_fixed(pid_number, 4) + "|identity";
            static const char kHexDigits[] = "0123456789abcdef";
            std::string encoded;
            encoded.reserve(selection.size() * 2);
            for (unsigned char c : selection)
            {
                encoded += kHexDigits[c >> 4];
                encoded += kHexDigits[c & 15];
            }
            return encoded;
        }
        return {};
    }
    default:
        return {};
    }
}

/* A float as a command argument wants it: integer unless it truly has a
 * fraction, so a period in milliseconds is "1000" and not "1000.000000". */
std::string format_number(float value)
{
    char text[32];
    if (value == static_cast<float>(static_cast<long>(value)))
        std::snprintf(text, sizeof(text), "%ld", static_cast<long>(value));
    else
        std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
    return text;
}

/* Fills an overlay result into a command-argument template. Unknown keys are
 * left in place rather than dropped, so a typo shows up in the argument the
 * service rejects instead of vanishing. */
std::string format_overlay_argument(std::string_view tmpl, const OverlayResult &result)
{
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();)
    {
        if (tmpl[i] != '{')
        {
            out += tmpl[i++];
            continue;
        }
        const std::size_t close = tmpl.find('}', i);
        if (close == std::string_view::npos)
        {
            out.append(tmpl.substr(i));
            break;
        }
        const std::string_view key = tmpl.substr(i + 1, close - i - 1);
        if (key == "value")
            out += format_number(result.value);
        else if (key == "r")
            out += std::to_string(result.r);
        else if (key == "g")
            out += std::to_string(result.g);
        else if (key == "b")
            out += std::to_string(result.b);
        else if (key == "text")
            out += result.text;
        else if (key == "index")
            out += std::to_string(result.index);
        else if (key == "id")
            out += std::to_string(result.index);
        else if (key == "label")
            out += result.label;
        else if (key == "address")
            out += result.item_value;
        else if (key == "parent")
            out += result.parent_label;
        else if (key == "action")
            out += result.action;
        else
            out.append(tmpl.substr(i, close - i + 1));
        i = close + 1;
    }
    return out;
}

/* Fills a key binder's rows from a PROFILE_GET reply: the profile's own map.*
 * lines, one row per source button the binder can offer. The reply carries the
 * profile INI hex-encoded (see ipc_profile_get), so decode first, then read
 * each map.<BUTTON>=<target> line. A compound target ("L1+R2") has no single
 * action, so its row is shown locked rather than misrepresented as one of its
 * parts; an explicitly unbound ("NONE") target stays bindable. Malformed or
 * unmappable targets are locked rather than treated as an unbound mapping. */
void seed_profile_bindings(std::string_view reply, OverlayRequest &request)
{
    /* The kit's default verbs, widened to the buttons a saved profile's map.*
     * keys also name (shoulders, triggers, Options, touchpad). */
    request.allowed = hui::ui::KeyBinderStyle::kDefaultAllowed |
                      action_bit(Action::page_prev) | action_bit(Action::page_next) |
                      action_bit(Action::jump_prev) | action_bit(Action::jump_next) |
                      action_bit(Action::menu) | action_bit(Action::touch);

    std::string ini;
    const std::string_view hex = json_value(reply, "data");
    if (hex.empty() || !decode_hex(hex, ini))
        return; /* no profile reply: an empty binder, the honest fallback */

    int id = 0;
    for (std::string_view line : split_lines(ini))
    {
        if (line.substr(0, 4) != "map.")
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq);   /* "map.CROSS" */
        const std::string_view expr = line.substr(eq + 1); /* "TRIANGLE", "NONE" */
        // Only exact known source keys are actionable. Never invent a mapping
        // from an empty/sentinel/unknown/placeholder source name.
        if (button_action_from_name(key.substr(4)) == Action::count)
            continue;
        const bool compound = expr.find('+') != std::string_view::npos;
        const Action action = compound ? Action::count : button_action_from_name(expr);
        hui::ui::KeyBinding binding;
        binding.id = id++;
        binding.label = std::string(key);
        binding.action = action;
        binding.fallback = action;
        binding.locked = compound || (action == Action::count && expr != "NONE");
        request.binding_expressions.emplace_back(expr);
        request.bindings.push_back(std::move(binding));
    }
}

/* The sections, in the order docs/UI-DESIGN.md settled: most-used first,
 * diagnostics last. Each section's rows live in their own src/screens/ file,
 * so this only fixes the order. */
std::vector<Section> build_screens()
{
    std::vector<Section> sections;
    sections.push_back(build_screen_controllers());
    sections.push_back(build_screen_setup());
    sections.push_back(build_screen_bluetooth());
    sections.push_back(build_screen_profiles());
    sections.push_back(build_screen_lightfx());
    sections.push_back(build_screen_service());
    sections.push_back(build_screen_diagnostics());
    sections.push_back(build_screen_audio());
    sections.push_back(build_screen_donate());
    sections.push_back(build_screen_about());
    return sections;
}

const char *loading_sentence(const char *endpoint)
{
    const std::string_view name = endpoint ? endpoint : "";
    if (name == "SETUP_STATUS") return "Loading setup...";
    if (name == "BLUETOOTH_STATUS") return "Loading Bluetooth...";
    if (name == "RUNTIME_GET") return "Loading settings...";
    if (name == "DEVICES") return "Loading controllers...";
    if (name == "PROFILES") return "Loading profiles...";
    return "Loading...";
}

/* A missing poll is not a dropped link. Picker rows fetch their own list;
 * a direct command waits for that reply before anything is sent. */
const char *press_poll_endpoint(const Row &row)
{
    const char *command = row.overlay_command ? row.overlay_command : row.command;
    const std::string_view name = command ? command : "";
    /* Pair, setup, and profile actions do not need a prior poll. Blocking
     * them on a stale status made Cross say the page was loading forever
     * while the service was already running. Toggles still need the current
     * value so a press does not invert a stale reading. */
    if (name == "RUNTIME_SET" || toggle_key(row.command_argument)) return "RUNTIME_GET";
    return nullptr;
}

bool picker_fetches_poll(const Row &row)
{
    return row.overlay == OverlayKind::picker &&
        (row.picker_source == PickerSource::fetch ||
         row.picker_source == PickerSource::bluetooth_paired ||
         row.picker_source == PickerSource::setup_sources ||
         row.picker_source == PickerSource::controller_rules);
}

} // namespace

ToolboxUi::ToolboxUi(ServiceSnapshot *service) : service_(service), sections_(build_screens())
{
    load_testers();
    /* One live-value slot per row, so a reading that moves can be spotted the
     * frame it moves and flashed. */
    page_positions_.resize(sections_.size());
    live_.resize(sections_.size());
    for (std::size_t s = 0; s < sections_.size(); ++s)
        live_[s].resize(sections_[s].rows.size());
    focus_ = Focus::content;
    rebuild_task(ServiceSnapshot::View{});
}

void ToolboxUi::load_testers()
{
    /* Packaged with the application and read at run time, so adding a name is
     * an edit to a text file and a deploy, not a code change. */
    static constexpr const char *kPath = "/app0/assets/ghostcontrol/testers.txt";

    Row *target = nullptr;
    for (Section &section : sections_)
        for (Row &row : section.rows)
            if (std::string_view(row.label) == "Testers")
                target = &row;
    if (!target)
        return;

    std::string bytes;
    if (!hui::save::read_file(kPath, &bytes))
    {
        /* Say what is missing rather than showing an empty panel. */
        target->detail = {"The tester list could not be read.",
                          "",
                          "Expected at:",
                          "  /app0/assets/ghostcontrol/testers.txt"};
        return;
    }

    target->detail.emplace_back("With thanks to everyone who tested this.");
    target->detail.emplace_back("");

    std::size_t start = 0;
    bool pending_blank = false;
    while (start <= bytes.size())
    {
        std::size_t end = bytes.find('\n', start);
        if (end == std::string::npos)
            end = bytes.size();
        std::string line = bytes.substr(start, end - start);
        start = end + 1;

        /* Strip a trailing carriage return so a file saved on Windows does
         * not draw a stray glyph. */
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();

        /* Comments are for whoever edits the file, not for the screen. */
        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '#')
            continue;

        if (line.empty())
        {
            /* Blank lines are kept as spacing, but only between names and
             * never as a run, so stray blank lines cannot pad the panel. */
            pending_blank = !target->detail.empty();
            continue;
        }
        if (pending_blank)
        {
            target->detail.emplace_back("");
            pending_blank = false;
        }
        target->detail.push_back(std::move(line));
    }

    if (target->detail.size() <= 2)
        target->detail.emplace_back("No names in the list yet.");
}

std::size_t ToolboxUi::visible_rows() const { return 8; }
std::size_t ToolboxUi::reader_lines() const { return 15; }
void ToolboxUi::clamp_scroll() {
    if (section().rows.empty()) row_index_ = 0;
    else row_index_ = std::min(row_index_, section().rows.size() - 1);
}

void ToolboxUi::open_section(std::size_t index, hui::ui::Feedback &feedback)
{
    if (index == section_index_)
        return;
    /* Which way the content travelled, so the pane slides in from there. The
     * rail wraps, so leaving the last section for the first is a forward move
     * and the other way round is a backward one. */
    int direction = index > section_index_ ? 1 : -1;
    if (section_index_ == 0 && index + 1 == sections_.size())
        direction = -1;
    else if (index == 0 && section_index_ + 1 == sections_.size())
        direction = 1;
    switch_dir_ = direction;

    page_positions_[section_index_] = {row_index_, scroll_};
    section_index_ = index;
    button_focus_ = 0;
    task_.buttons.clear();
    app_log("workspace changed: %s", section().name);
    row_index_ = page_positions_[index].row;
    scroll_ = page_positions_[index].scroll;
    clamp_scroll();
    rows_age_ = 0.0f;   /* the new rows arrive rather than appear */
    switch_age_ = 0.0f;
    // Return to the same control and scroll position when changing pages.
    scroll_pos_.snap(static_cast<float>(scroll_));
    const auto count = section().rows.size();
    const auto window = visible_rows();
    scroll_thumb_.snap(count > window ? static_cast<float>(scroll_) /
                      static_cast<float>(count - window) : 0.0f);
    /* A fetch-backed picker reads whatever is already in the fetch store when
     * it opens, so the first open can be empty. Queueing the section's fetch
     * commands now means the list is populated by the time the player reaches
     * the row, rather than only on the second open. Distinct commands only:
     * two rows sharing PROFILE_FILES need one request, not two. */
    if (service_)
    {
        std::vector<std::string> queued;
        for (const Row &row : section().rows)
        {
            if (row.picker_source != PickerSource::fetch || !row.picker_command)
                continue;
            if (std::find(queued.begin(), queued.end(), row.picker_command) != queued.end())
                continue;
            queued.emplace_back(row.picker_command);
            service_->queue_fetch(row.picker_command);
        }
    }
    feedback.play(Cue::tab);
}

void ToolboxUi::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    update(input, dt, feedback, service_ ? service_->read() : ServiceSnapshot::View{});
}

void ToolboxUi::show_toast(std::string text)
{
    toast_text_ = std::move(text);
    toast_age_ = 0.0f;
}

/* "Sent to the loader" is deliberately never "Service started": only a
 * fresh compatible STATUS taken by the lifecycle worker may say the latter. */
static bool companion_success(LifecycleOutcome outcome)
{
    return outcome == LifecycleOutcome::ready || outcome == LifecycleOutcome::already_running ||
           outcome == LifecycleOutcome::restarted || outcome == LifecycleOutcome::stopped;
}

/* Which lifecycle operation a Service row owns. Start shows manual and
 * automatic starts; Stop and Restart show only their own operations. */
static bool row_owns(const Row &row, LifecycleKind kind)
{
    if (row.stops_companion) return kind == LifecycleKind::stop;
    if (row.restarts_companion) return kind == LifecycleKind::restart;
    return kind == LifecycleKind::manual || kind == LifecycleKind::automatic;
}

std::string ToolboxUi::companion_reading(const Row &row) const
{
    if (!lifecycle_) return {};
    const auto &v = companion_rows_[row.stops_companion ? 1 : row.restarts_companion ? 2 : 0];
    if (v.active) return row_owns(row, v.kind) ? lifecycle_phase_text(v.phase) : std::string{};
    if (!v.completed || !row_owns(row, v.completed_kind)) return {};
    switch (v.outcome)
    {
    case LifecycleOutcome::none: return {};
    case LifecycleOutcome::already_running: return "Answering";
    case LifecycleOutcome::ready: return "Started";
    case LifecycleOutcome::start_unconfirmed:
        return v.launch.bytes == v.launch.payload_bytes && v.launch.bytes
                   ? "Sent; startup not confirmed" : "Partly sent; outcome unknown";
    case LifecycleOutcome::identity_mismatch: return "Port in use";
    case LifecycleOutcome::unknown_liveness: return "Port busy";
    case LifecycleOutcome::unreachable: return "Unreachable";
    case LifecycleOutcome::launch_failed: return "Not sent";
    case LifecycleOutcome::cancelled:
        if (v.launch.bytes) return v.launch.bytes == v.launch.payload_bytes
                                      ? "Cancelled after send; outcome unknown" : "Cancelled mid-send; outcome unknown";
        return "Cancelled; nothing sent";
    case LifecycleOutcome::stopped: return "Stopped";
    case LifecycleOutcome::restarted: return "Restarted";
    case LifecycleOutcome::stop_unconfirmed: return "Stop unconfirmed";
    }
    return {};
}

void ToolboxUi::refresh_lifecycle()
{
    lifecycle_view_ = lifecycle_->read();
    const auto &v = lifecycle_view_;
    if (v.completed && !v.active)
    {
        const auto slot = v.completed_kind == LifecycleKind::stop ? 1 : v.completed_kind == LifecycleKind::restart ? 2 : 0;
        auto completed = v;
        completed.active = 0;
        companion_rows_[slot] = std::move(completed);
    }
    if (v.active)
    {
        const auto slot = v.kind == LifecycleKind::stop ? 1 : v.kind == LifecycleKind::restart ? 2 : 0;
        companion_rows_[slot] = v;
    }
}

std::uint64_t ToolboxUi::companion_request(LifecycleKind kind)
{
    if (!lifecycle_)
    {
        show_toast(kind == LifecycleKind::manual ? "Start Ghosttooth API with your ELF loader."
                                                 : "Ghosttooth API control is unavailable in this build.");
        return 0;
    }
    // Retain the previous completion before a new worker result can replace it.
    refresh_lifecycle();
    /* A press is the player asking again on purpose, so it is never rate
     * limited the way the automatic attempt is -- but it is still one
     * operation at a time, and every kind identifies before it acts. */
    const std::uint64_t receipt = kind == LifecycleKind::stop      ? lifecycle_->request_stop()
                                  : kind == LifecycleKind::restart ? lifecycle_->request_restart()
                                                                   : lifecycle_->request_start();
    refresh_lifecycle();
    if (!receipt)
    {
        show_toast(lifecycle_view_.available
                       ? "Ghosttooth API is busy. Wait for the current task."
                       : "Ghosttooth API control is unavailable. Nothing was sent.");
        return 0;
    }
    show_toast(kind == LifecycleKind::manual
                   ? "Checking Ghosttooth API..."
                   : "Checking whether Ghosttooth API control is supported...");
    return receipt;
}

std::uint64_t ToolboxUi::start_companion() { return companion_request(LifecycleKind::manual); }
std::uint64_t ToolboxUi::stop_companion() { return companion_request(LifecycleKind::stop); }
std::uint64_t ToolboxUi::restart_companion() { return companion_request(LifecycleKind::restart); }

void ToolboxUi::open_overlay(OverlayRequest request, hui::ui::Feedback &feedback)
{
    /* A modal opened with no owning row has no command to post; its result is
     * the caller's to read from the host if it wants it. */
    overlay_row_ = nullptr;
    overlay_.open(request, feedback);
}

void ToolboxUi::open_beta_disclaimer(hui::ui::Feedback &feedback)
{
    hui::ui::DialogContent content;
    content.icon = hui::ui::StatusKind::warning;
    content.title = "Beta build";
    content.body =
        "This is a beta of Ghostcontrol Toolbox 2.0.\n\n"
        "Expect bugs. Controller profiles, wireless pads and audio features "
        "may change or break between builds. Saved profiles are not "
        "guaranteed to carry over.\n\n"
        "Use at your own risk. Export your profiles from the Profiles page "
        "before you update.";
    content.buttons = {{"Continue", hui::ui::ButtonKind::primary},
                       {"Don't show again", hui::ui::ButtonKind::secondary}};
    content.default_button = 0;
    beta_dialog_.style.theme = ghost_theme();
    /* The app does not capture a glass blur this frame; a solid panel is the
     * kit's documented fallback and still hides the page behind it. */
    beta_dialog_.style.frosted = false;
    beta_dialog_.style.width = 880.0f;
    beta_dialog_.style.body_lines = 12;
    beta_dialog_.style.buttons = hui::ui::DialogButtons::row;
    beta_dialog_.open(std::move(content), feedback);
}

void ToolboxUi::open_row_overlay(std::string_view label, const ServiceSnapshot::View &service,
                                 hui::ui::Feedback &feedback)
{
    for (const Section &section : sections_)
        for (const Row &row : section.rows)
            if (std::string_view(row.label) == label)
            {
                if (row.overlay != OverlayKind::none)
                    open_overlay_for_row(row, service, feedback);
                return;
            }
}

void ToolboxUi::open_reader(std::vector<std::string> lines, ReaderSource source,
                            hui::ui::Feedback &feedback)
{
    reader_lines_ = std::move(lines);
    if (source == ReaderSource::detail && row().shows_launch_result && lifecycle_)
    {
        refresh_lifecycle();
        std::string diagnostic = lifecycle_view_.active
            ? std::string("In progress: ") + lifecycle_phase_text(lifecycle_view_.phase) + "."
            : lifecycle_outcome_text(lifecycle_view_);
        if (!lifecycle_view_.automatic_note.empty())
            diagnostic += " " + lifecycle_view_.automatic_note;
        if (!lifecycle_view_.launch.package_version.empty())
            diagnostic += " Packaged Ghosttooth API " + lifecycle_view_.launch.package_version +
                          " (package checked).";
        std::vector<std::string> startup = {"Last connection attempt:", ""};
        while (diagnostic.size() > 72) {
            std::size_t cut = diagnostic.rfind(' ', 72);
            if (cut == std::string::npos || cut == 0) cut = 72;
            startup.push_back(diagnostic.substr(0, cut));
            diagnostic.erase(0, cut + (diagnostic[cut] == ' ' ? 1 : 0));
        }
        startup.push_back(diagnostic);
        startup.push_back("");
        startup.insert(startup.end(), reader_lines_.begin(), reader_lines_.end());
        reader_lines_ = std::move(startup);
    }
    reader_title_.clear(); /* a row reader is titled by its row's label */
    reader_source_ = source;
    reader_visual_lines_.clear();
    reader_scroll_ = 0;
    reader_age_ = 0.0f;
    reader_fetch_request_ = 0;
    reader_fetch_pending_ = false;
    focus_ = Focus::reader;

    if (reader_refreshable()) {
        reader_lines_ = {"Connect Ghosttooth API to read this."};
        refresh_reader();
    }
    feedback.play(Cue::modal_open);
}

void ToolboxUi::refresh_reader()
{
    if (!service_)
        return;
    if (!reader_refreshable()) return;
    reader_visual_lines_.clear();
    reader_scroll_ = 0; // Loading/refusal replaces the old document immediately.
    const bool selected_raw = reader_source_ == ReaderSource::setup_raw;
    // Each panel owns its precise completion; last-good or automatic polls cannot fill it.
    reader_fetch_request_ = service_->queue_fetch_request(reader_source_ == ReaderSource::bluetooth_diagnostics ? "BLUETOOTH_STATUS" : selected_raw ? "SETUP_INPUT" : "LOG_TAIL",
                                                         selected_raw || reader_source_ == ReaderSource::bluetooth_diagnostics ? nullptr : "120");
    reader_fetch_pending_ = reader_fetch_request_ != 0;
    reader_lines_ = {reader_fetch_pending_
        ? reader_source_ == ReaderSource::bluetooth_diagnostics ? "Loading Bluetooth details..."
        : selected_raw ? "Loading controller input..."
                       : reader_source_ == ReaderSource::log_tail ? "Loading the log..."
                                                                  : "Loading captured input..."
        : "Could not refresh. Press Cross to try again."};
}

void ToolboxUi::rebuild_reader_lines(const ServiceSnapshot::Fetch &fetch)
{
    if (fetch.state == ServiceSnapshot::ResultState::none) {
        reader_lines_ = {"This result is no longer available.", "Press Cross to refresh."};
        return;
    }
    if (reader_source_ == ReaderSource::bluetooth_diagnostics) {
        reader_lines_ = fetch.reached && fetch.valid && fetch.state == ServiceSnapshot::ResultState::acknowledged
            ? bt_diagnostic_lines(fetch.body)
            : std::vector<std::string>{"Could not read Bluetooth details.", "Press Cross to try again."};
        return;
    }
    if (reader_source_ == ReaderSource::setup_raw) {
        reader_lines_ = fetch.reached && fetch.valid && fetch.state == ServiceSnapshot::ResultState::acknowledged
            ? ServiceSnapshot::setup_raw_lines(fetch.body)
            : std::vector<std::string>{"Could not read this controller.",
                                       "Press Cross to try again."};
        return;
    }
    if (!fetch.reached)
    {
        reader_lines_ = {"Ghosttooth API did not respond.", "",
                         "Check the Ghosttooth API link in the header, then press",
                         "Cross to try again."};
        return;
    }
    if (!reply_ok(fetch.body))
    {
        reader_lines_ = {"Ghosttooth API refused the log request.", "",
                         fetch.body.empty() ? "(no reply)" : fetch.body};
        return;
    }

    std::string decoded;
    const std::string_view hex = json_value(fetch.body, "data");
    if (hex.empty() || !decode_hex(hex, decoded))
    {
        reader_lines_ = {"The log is empty or could not be read.", "",
                         "Try refreshing it. Ghosttooth API may not have logged",
                         "anything yet."};
        return;
    }

    std::vector<std::string> lines = split_lines(decoded);
    if (reader_source_ == ReaderSource::raw_input)
    {
        std::vector<std::string> captures;
        for (std::string &line : lines)
            if (line.find("setup raw:") != std::string::npos)
                captures.push_back(std::move(line));
        if (captures.empty())
        {
            /* An empty capture list has an honest cause, so it is explained
             * rather than shown as a blank panel. */
            reader_lines_ = {"No raw input captures in the recent log.", "",
                             "To produce captures:",
                             "  1. Open Setup and run a guided setup.",
                             "  2. Move and press the controller when prompted.",
                             "  3. The service logs one \"setup raw:\" line per input.",
                             "  4. Return here and press Cross to refresh."};
        }
        else
        {
            reader_lines_ = std::move(captures);
        }
        return;
    }

    if (lines.empty())
    {
        reader_lines_ = {"The log tail is empty.", "",
                         "Ghosttooth API has not logged anything yet."};
        return;
    }
    reader_lines_ = std::move(lines);
}

void ToolboxUi::show_reader(std::string title, std::vector<std::string> lines,
                            hui::ui::Feedback &feedback, bool refreshable)
{
    reader_lines_ = std::move(lines);
    reader_title_ = std::move(title);
    /* A "refreshable" harness reader behaves like a live one for the hint row
     * and the refresh verb, but there is no service behind it, so a refresh is
     * a harmless no-op and the fixed lines stay put. */
    reader_source_ = refreshable ? ReaderSource::log_tail : ReaderSource::host;
    reader_visual_lines_.clear();
    reader_scroll_ = 0;
    reader_age_ = 0.0f;
    reader_fetch_request_ = 0;
    reader_fetch_pending_ = false;
    focus_ = Focus::reader;
    feedback.play(Cue::modal_open);
}

void ToolboxUi::open_overlay_for_row(const Row &row, const ServiceSnapshot::View &service,
                                     hui::ui::Feedback &feedback)
{
    OverlayRequest request;
    request.kind = row.overlay;
    request.title = row.label;
    request.label = row.label;
    switch (row.overlay)
    {
    case OverlayKind::range:
        request.min = row.overlay_min;
        request.max = row.overlay_max;
        request.step = row.overlay_step;
        request.value = row.overlay_value;
        if (row.overlay_unit)
            request.unit = row.overlay_unit;
        /* Start from the reading, so the thumb sits where the service is
         * rather than at the declared default. */
        if (row.live == LiveSource::fx_period_ms)
        {
            const std::string_view found = fx_setting(service, "period_ms");
            if (!found.empty())
                request.value = std::strtof(std::string(found).c_str(), nullptr);
        }
        break;
    case OverlayKind::color:
    {
        const auto channel_of = [&service](const char *key) {
            const std::string_view found = fx_setting(service, key);
            return found.empty()
                       ? 255
                       : static_cast<int>(std::strtol(std::string(found).c_str(), nullptr, 10));
        };
        request.r = channel_of("r");
        request.g = channel_of("g");
        request.b = channel_of("b");
        break;
    }
    case OverlayKind::picker:
        /* The options come from wherever the row says: its own detail lines,
         * the polled bluetooth view, or a fetched command's reply. Naming the
         * source on the row keeps the chooser honest: a paired list or a file
         * list is never a copy typed into the row table. */
        switch (row.picker_source)
        {
        case PickerSource::bluetooth_paired:
        case PickerSource::controller_rules:
        case PickerSource::fetch:
        {
            const char *endpoint = row.picker_source == PickerSource::bluetooth_paired ? "BLUETOOTH_STATUS" :
                row.picker_source == PickerSource::controller_rules ? "CONTROLLERS" : row.picker_command;
            picker_request_ = service_ && endpoint ? service_->queue_fetch_request(endpoint) : 0;
            if (!picker_request_) { show_toast("Could not load the list. Try again."); return; }
            picker_row_ = &row;
            show_toast("Loading choices...");
            return;
        }
        case PickerSource::setup_sources:
            if (!service_) { show_toast("No controllers are available for setup."); return; }
            setup_picker_request_ = service_->queue_fetch_request("DEVICES");
            if (!setup_picker_request_) {
                show_toast("Could not load controllers. Try again."); return;
            }
            setup_picker_row_ = &row;
            show_toast("Finding controllers...");
            return;
        case PickerSource::detail:
        default:
            request.items.assign(row.detail.begin(), row.detail.end());
            if (row.live == LiveSource::fx_summary) {
                // This picker edits saved configuration. Never fall back to an
                // animation poll or a default when the runtime value is absent.
                const auto receipt = service_ ? service_->last_reply("RUNTIME_GET") : ServiceSnapshot::Fetch{};
                const JsonReply saved(service_ ? receipt.body : service.runtime_json);
                const auto effect = saved.raw("dualsense_fx_effect");
                if ((service_ && (!receipt.generation || !receipt.reached || !receipt.valid)) ||
                    !saved.success() || effect.size() != 1 || effect[0] < '0' || effect[0] > '4') {
                    show_toast("Could not read the saved effect. No change was sent.");
                    overlay_row_ = nullptr; // Do not reopen the overlay.
                    return;
                }
                request.index = effect[0] - '0';
            }
            break;
        }
        break;
    case OverlayKind::text:
        /* A text modal can open seeded and capped: the row names the seed and
         * the length, and the keyboard refuses past it. A chained text modal
         * is seeded from the first result instead (open_chain_next). */
        request.max_length = static_cast<int>(row.overlay_max_length);
        if (row.overlay_text)
            request.text = row.overlay_text;
        break;
    case OverlayKind::keybinder:
        /* A key binder's rows are the shell's to seed: a plain row opens the
         * scaffold empty, and a chained one fills it from a fetched profile
         * (open_chain_next), so nothing is declared here. */
        break;
    default:
        return;
    }
    /* Remember which row owns this modal: its metadata decides what an
     * accepted result posts, not whichever row happens to hold the focus. */
    overlay_row_ = &row;
    overlay_.open(request, feedback);
}

void ToolboxUi::apply_overlay_result(const Row &row, const OverlayResult &result,
                                     hui::ui::Feedback &feedback)
{
    if (service_ && (!picker_fresh(row) || !action_fresh(row, service_->read()) ||
                     !row_capability_supported(row, service_->read()))) {
        show_toast("This control is unavailable. No change was sent.");
        feedback.play(Cue::error); return;
    }
    if (row.live == LiveSource::fx_summary) {
        // Revalidate the latest actual receipt, not the modal's opening value
        // or View's last-good cache. A new valid value still permits selection.
        const auto receipt = service_ ? service_->last_reply("RUNTIME_GET") : ServiceSnapshot::Fetch{};
        const JsonReply saved(receipt.body);
        const auto effect = saved.raw("dualsense_fx_effect");
        if (!receipt.generation || !receipt.reached || !receipt.valid ||
            !saved.success() || effect.size() != 1 || effect[0] < '0' || effect[0] > '4' ||
            result.index < 0 || result.index > 4) {
            show_toast("Could not read the saved effect. No change was sent.");
            feedback.play(Cue::error); return;
        }
    }
    std::vector<std::string> arguments;
    if (row.overlay_command) {
        for (const char *tmpl : row.overlay_arguments) {
            if (!tmpl) continue;
            const std::string formatted = format_overlay_argument(tmpl, result);
            if (formatted.empty()) continue;
            arguments.push_back(row.overlay_hex ? hex_encode(formatted) : formatted);
        }
    }
    /* Remember a chosen physical source so arming the guided walk can re-begin
     * and arm atomically (see setup_selection_). The picker encodes the whole
     * selection into the single SETUP_BEGIN argument. */
    if (row.overlay_command && std::string_view(row.overlay_command) == "SETUP_BEGIN" && !arguments.empty())
        setup_selection_ = arguments.front();
    const bool queued = service_ && row.overlay_command &&
        queue_overlay_group(*service_, row.overlay_command, arguments, row.command_timeout_ms);
    feedback.play(queued ? Cue::select : Cue::error, 1.0f, 0.0f, queued ? 1.0f : 0.6f);
    if (!queued)
        feedback.rumble(0.25f, 0.06f);
    /* The toast is the only place a queued command reports back, exactly as a
     * plain command row's does. */
    const std::string toast = queued ? "Waiting for Ghosttooth API..." :
                                       "Could not send the change. Try again.";
    if (!toast.empty())
    {
        toast_text_ = toast;
        toast_age_ = 0.0f;
    }
}

void ToolboxUi::begin_chain(const Row &row, const OverlayResult &first, hui::ui::Feedback &feedback)
{
    chain_ = Chain{};
    chain_.active = true;
    chain_.row = &row;
    chain_.first = first;
    /* A between-modals fetch (a profile's content) needs an argument built
     * from what the first modal chose (which profile). Without one the second
     * modal opens straight away. */
    if (row.overlay_next_command && service_)
    {
        const std::string argument = row.overlay_next_argument
                                         ? format_overlay_argument(row.overlay_next_argument, first)
                                         : std::string();
        chain_.fetch_command = row.overlay_next_command;
        chain_.fetch_request = service_->queue_fetch_request(row.overlay_next_command,
                                                     argument.empty() ? nullptr : argument.c_str());
        chain_.fetch_pending = chain_.fetch_request != 0;
        if (!chain_.fetch_pending) { chain_ = Chain{}; show_toast("Could not load the profile. No change was sent."); return; }
    }
    if (!chain_.fetch_pending)
        open_chain_next(feedback);
}

void ToolboxUi::open_chain_next(hui::ui::Feedback &feedback)
{
    if (!chain_.row)
        return;
    const Row &row = *chain_.row;
    if (row.full_profile_editor) {
        profile_editor_.open(service_, chain_.first.label, chain_.fetch_request, overlay_, feedback);
        chain_ = Chain{}; overlay_row_ = nullptr;
        const auto message = profile_editor_.take_message();
        if (!message.empty()) show_toast(message);
        return;
    }
    OverlayRequest request;
    request.kind = row.overlay_next;
    request.title = row.label;
    request.label = row.label;
    switch (row.overlay_next)
    {
    case OverlayKind::picker:
        if (row.picker_source == PickerSource::controller_rules) {
            request.label = "Saved output type (not active in games)";
            for (const char *type : {"ds4", "ds5", "psvr2_left", "psvr2_right"}) {
                request.items.emplace_back(ServiceSnapshot::controller_output_label(type));
                request.item_values.emplace_back(type);
            }
        }
        break;
    case OverlayKind::text:
        /* Seed with the first modal's choice (the name being renamed), so the
         * player edits the current name rather than retyping it. */
        request.max_length = static_cast<int>(row.overlay_max_length);
        request.text = chain_.first.label;
        break;
    case OverlayKind::keybinder:
    {
        const ServiceSnapshot::Fetch fetch =
            service_ && chain_.fetch_request ? service_->fetch_request(chain_.fetch_request)
                                             : ServiceSnapshot::Fetch{};
        request.single_edit = true;
        seed_profile_bindings(fetch.body, request);
        break;
    }
    default:
        break;
    }
    overlay_row_ = chain_.row;
    overlay_.open(request, feedback);
}

bool ToolboxUi::chain_source_fresh() const
{
    if (!chain_.active || !chain_.row) return false;
    if (!chain_.row->overlay_next_command) return true;
    if (!service_ || !chain_.fetch_request || !chain_.fetch_command) return false;
    const auto own = service_->fetch_request(chain_.fetch_request);
    const auto latest = service_->last_reply(chain_.fetch_command);
    // Only this chain's retained receipt may authorize its binder. A later
    // failed OR successful query (possibly for another profile) invalidates it;
    // recovery requires a fresh chain, never a cached or unrelated success.
    return own.reached && own.valid && own.generation != 0 &&
        own.state == ServiceSnapshot::ResultState::acknowledged &&
        latest.reached && latest.valid && latest.state == ServiceSnapshot::ResultState::acknowledged &&
        latest.generation == own.generation;
}

void ToolboxUi::finish_chain(const Row &row, const OverlayResult &second,
                             hui::ui::Feedback &feedback)
{
    if (!chain_source_fresh()) {
        show_toast("Could not refresh this item. Choose it again. No change was sent.");
        feedback.play(Cue::error);
        // Keep the invalid source fence while the binder is still open. Back
        // cancels it; subsequent captures must not restart from binder text.
        return;
    }
    if (row.overlay_next == OverlayKind::keybinder) {
        const auto *request = overlay_.request();
        const bool owned = request && std::any_of(request->bindings.begin(), request->bindings.end(), [&](const hui::ui::KeyBinding &binding) {
            return !binding.locked && binding.id == second.index && binding.label == second.text;
        });
        if (!owned || second.index < 0 || second.text.substr(0, 4) != "map." ||
            button_action_from_name(std::string_view(second.text).substr(4)) == Action::count ||
            second.action.empty() || button_action_from_name(second.action) == Action::count) {
            show_toast("This mapping cannot be edited here. No change was sent.");
            feedback.play(Cue::error);
            return;
        }
    }
    /* The second modal's result is the live one (the new name, the new binding);
     * the first's choice is carried alongside so the row's single
     * overlay_command can name both. */
    OverlayResult merged = second;
    merged.parent_label = row.picker_source == PickerSource::controller_rules
        ? chain_.first.item_value : chain_.first.label;
    apply_overlay_result(row, merged, feedback);
    // The real keybinder reports changes without closing. Its source/parent
    // must survive every capture until Back, unlike a completed text/picker.
    if (row.overlay_next != OverlayKind::keybinder) {
        chain_ = Chain{};
        overlay_row_ = nullptr;
    }
}

void ToolboxUi::draw_overlay(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const
{
    overlay_.draw(list, fonts);
}

bool ToolboxUi::endpoint_fresh(const char *endpoint) const
{
    if (!service_) return true; // Explicit host preview only.
    const auto f = service_->last_reply(endpoint);
    return f.generation != 0 && f.reached && f.valid;
}

bool ToolboxUi::picker_fresh(const Row &row) const
{
    const char *endpoint = row.picker_source == PickerSource::bluetooth_paired ? "BLUETOOTH_STATUS" :
        row.picker_source == PickerSource::controller_rules ? "CONTROLLERS" :
        row.picker_source == PickerSource::setup_sources ? "DEVICES" :
        row.picker_source == PickerSource::fetch ? row.picker_command : nullptr;
    return !endpoint || endpoint_fresh(endpoint);
}

ServiceSnapshot::View ToolboxUi::fresh_view(const ServiceSnapshot::View &view) const
{
    auto current = view;
    const std::pair<const char *, std::string ServiceSnapshot::View::*> fields[] = {
        {"STATUS", &ServiceSnapshot::View::status_json},
        {"RUNTIME_GET", &ServiceSnapshot::View::runtime_json},
        {"BLUETOOTH_STATUS", &ServiceSnapshot::View::bluetooth_json},
        {"PROFILES", &ServiceSnapshot::View::profiles_json},
        {"CONTROLLERS", &ServiceSnapshot::View::controllers_json},
        {"DEVICES", &ServiceSnapshot::View::devices_json},
        {"CONSOLE_ID", &ServiceSnapshot::View::console_id_json},
        {"DUALSENSE_FX_STATUS", &ServiceSnapshot::View::fx_json},
        {"SETUP_STATUS", &ServiceSnapshot::View::setup_json}};
    for (const auto &[endpoint, field] : fields)
        if (!endpoint_fresh(endpoint) || view.link != ServiceSnapshot::Link::up)
            (current.*field).clear();
    if (current.bluetooth_json.empty()) current.bluetooth_paired.clear();
    // Native samples belong to the app and do not depend on service IPC.
    return current;
}

bool ToolboxUi::action_fresh(const Row &row, const ServiceSnapshot::View &view) const
{
    if (view.link != ServiceSnapshot::Link::up || !endpoint_fresh("STATUS")) return false;
    const std::string_view command = row.overlay_command ? row.overlay_command : row.command ? row.command : "";
    if (command == "RUNTIME_SET" && !endpoint_fresh("RUNTIME_GET")) return false;
    if (command.starts_with("SETUP_") && command != "SETUP_BEGIN" &&
        !endpoint_fresh("SETUP_STATUS")) return false;
    if (row.picker_source == PickerSource::bluetooth_paired && !endpoint_fresh("BLUETOOTH_STATUS")) return false;
    return true;
}

std::string ToolboxUi::row_reading(const Row &row, const ServiceSnapshot::View &service) const
{
    if (row.starts_companion || row.stops_companion || row.restarts_companion)
        return companion_reading(row);
    if (row.app_launch_opt_in) {
        const auto &v = app_preference_view_;
        if (!app_preferences_ || !v.available) return "Unavailable";
        if (!v.loaded) return "Loading";
        if (v.pending) return v.requested ? "Turning on..." : "Turning off...";
        if (v.saved && !v.verified) return "Saved; not confirmed";
        if (!v.error.empty()) return v.authoritative_known && v.authoritative ? "On; save failed" : "Could not read setting";
        return v.authoritative ? "On" : "Off";
    }
    if (toggle_key(row.command_argument))
        return row_actions_.reading(row, service, !service_ ||
            (endpoint_fresh("RUNTIME_GET") && action_runtime_generation_ != 0 && action_runtime_age_ <= 5.0f));
    return resolve_live(row.live, fresh_view(service));
}

void ToolboxUi::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback,
                       const ServiceSnapshot::View &service)
{
    /* The spirit field drifts whether or not anything is focused. */
    ghostscape_.update(dt);
    rebuild_task(service);
    if(service_) {
        const bool pending=audio_screen_.pending(); audio_screen_.update(*service_);
        if(pending && !audio_screen_.pending()) show_toast(audio_screen_.feedback());
    }

    app_preference_view_ = app_preferences_ ? app_preferences_->read() : AppServicePreferencesWorker::View{};
    if (app_preference_request_ && !app_preference_view_.pending &&
        app_preference_view_.receipt == app_preference_request_ &&
        app_preference_view_.generation == app_preference_request_) {
        const auto &v = app_preference_view_;
        show_toast(!v.error.empty() ? v.error :
            "Saved. This applies next time you open the app.");
        feedback.play(v.saved && v.verified ? Cue::select : Cue::error);
        app_preference_request_ = 0;
    }
    if (lifecycle_)
    {
        /* Hand the committed preference to the lifecycle owner. This only
         * compares under a lock; the probe and push happen on its thread. */
        if (app_preferences_) lifecycle_->observe_consent(app_preference_view_);
        refresh_lifecycle();
        const auto &lv = lifecycle_view_;
        if (lv.completed > companion_reported_)
        {
            companion_reported_ = lv.completed;
            const std::string text = lifecycle_outcome_text(lv);
            show_toast(lv.completed_kind == LifecycleKind::automatic ? "Automatic start: " + text
                       : lv.completed_kind == LifecycleKind::stop      ? "Stop: " + text
                       : lv.completed_kind == LifecycleKind::restart   ? "Restart: " + text
                                                                       : text);
            feedback.play(companion_success(lv.outcome) ? Cue::select : Cue::error);
        }
    }
    action_view_age_ += dt;
    if (service.generation != action_view_generation_) {
        action_view_generation_ = service.generation;
        action_view_age_ = 0.0f;
    }
    action_runtime_age_ += dt;
    if (service_) {
        const auto runtime_fetch = service_->last_reply("RUNTIME_GET");
        if (runtime_fetch.generation != action_runtime_generation_) {
            action_runtime_generation_ = runtime_fetch.generation;
            action_runtime_age_ = 0.0f;
        }
        row_actions_.observe_after_command(service_->last_reply("RUNTIME_SET"),
                                           runtime_fetch, &service);
        row_actions_.reconcile(service);
        if (row_actions_.pending() || row_actions_.unavailable()) {
            action_pending_age_ += dt;
            if (!runtime_fetch_queued_)
                runtime_fetch_queued_ = service_->queue_fetch("RUNTIME_GET");
            if (action_pending_age_ > 5.0f) {
                show_toast("Ghosttooth API is not responding. Open the Ghosttooth API page to reconnect.");
                action_pending_age_ = 0.0f;
                runtime_fetch_queued_ = false;
            }
        } else {
            runtime_fetch_queued_ = false;
            action_pending_age_ = 0.0f;
        }
    }
    age_ += dt;
    rows_age_ += dt;
    reader_age_ += dt;
    switch_age_ += dt;
    press_age_ += dt;
    link_flash_ += dt;
    clock_ += dt;

    /* Launch disclaimer owns the frame while it is up: open once after arm
     * unless "Don't show again" is saved, then take every input. */
    if (beta_disclaimer_armed_ && !beta_disclaimer_checked_)
    {
        beta_disclaimer_checked_ = true;
        const bool dismissed = beta_disclaimer_prefs_.load() && beta_disclaimer_prefs_.dismissed();
        if (!dismissed)
            open_beta_disclaimer(feedback);
    }
    if (beta_dialog_.is_open())
    {
        beta_dialog_.update(dt);
        const hui::ui::Event event = beta_dialog_.handle(input, feedback);
        if (event == hui::ui::Event::activated)
        {
            const int choice = beta_dialog_.choice();
            beta_dialog_.close(feedback);
            if (choice == 1)
            {
                const DisclaimerWrite write = beta_disclaimer_prefs_.set_dismissed(true);
                show_toast(write.verified
                               ? "Disclaimer will not show again."
                               : (write.error.empty() ? "Could not save that preference."
                                                      : write.error));
                feedback.play(write.verified ? Cue::select : Cue::error);
            }
        }
        else if (event == hui::ui::Event::cancelled)
        {
            beta_dialog_.close(feedback);
        }
        return;
    }

    /* A command the service has answered shows for a few seconds and fades;
     * it is the only place a queued command reports back. */
    if (service_)
    {
        const std::string toast = service_->take_toast();
        if (!toast.empty())
        {
            toast_text_ = toast;
            toast_age_ = 0.0f;
        }
    }
    toast_age_ += dt;

    /* The link coming up is worth a moment: a quick mint flash on the pill. */
    if (have_link_ && last_link_ == ServiceSnapshot::Link::connecting &&
        service.link == ServiceSnapshot::Link::up)
        link_flash_ = 0.0f;
    last_link_ = service.link;
    have_link_ = true;

    /* Live readings are watched off the frame the player is looking at: a
     * value that changes between frames starts a flash and ticks its dot.
     * draw() only ever reads these timers, so it stays const. */
    for (auto &section_states : live_)
        for (LiveValue &state : section_states)
            state.flash += dt;
    if (service.link == ServiceSnapshot::Link::up && section_index_ < live_.size())
    {
        std::vector<LiveValue> &states = live_[section_index_];
        const std::vector<Row> &rows = section().rows;
        for (std::size_t i = 0; i < rows.size() && i < states.size(); ++i)
        {
            if (rows[i].state != RowState::live)
                continue;
            const std::string text = row_reading(rows[i], service);
            if (states[i].seen && text != states[i].text)
                states[i].flash = 0.0f;
            states[i].text = text;
            states[i].seen = true;
        }
    }

    /* The reader eases in and out rather than popping. */
    const float reader_target = focus_ == Focus::reader ? 1.0f : 0.0f;
    reader_open_ += (reader_target - reader_open_) * std::min(1.0f, dt * 14.0f);

    /* Logical actions, not raw pad bits. InputFrame::pressed is a mask of
     * action bits, so testing a pad_bits value against it reads a different
     * button entirely -- which is how the first pass ended up with L1 and R1
     * behaving as L2 and R2. */
    const bool confirm = input.is_pressed(Action::confirm);
    const bool back = input.is_pressed(Action::back);
    const bool page_prev = input.is_pressed(Action::page_prev);
    const bool page_next = input.is_pressed(Action::page_next);

    /* A modal owns the frame while it is up: the pad is the widget's, and the
     * hint row (draw_hints) shows the widget's verbs. When it is accepted the
     * value is turned into the row's queued command here -- off the frame
     * thread, as every command is. A row that names a second modal chains: the
     * first result starts the chain and the row's command fires from the
     * second. */
    if (profile_editor_.active()) {
        profile_editor_.update(input, dt, overlay_, feedback);
        const auto message = profile_editor_.take_message();
        if (!message.empty()) show_toast(message);
        return;
    }
    if (setup_save_request_) {
        const auto completion = service_->fetch_request(setup_save_request_);
        if (completion.state == ServiceSnapshot::ResultState::queued) {
            if (back) show_toast("Save is already in progress. Closing this will not undo it.");
            return;
        }
        setup_save_request_ = 0;
        show_toast(completion.detail.empty() ? "Could not confirm the save. Check your saved profiles." : completion.detail);
        return;
    }
    if (setup_picker_row_) {
        if (back) { setup_picker_row_ = nullptr; setup_picker_request_ = 0; return; }
        const auto fetch = service_->fetch_request(setup_picker_request_);
        if (fetch.state == ServiceSnapshot::ResultState::queued)
            return;
        setup_picker_request_ = 0;
        const Row *selected = setup_picker_row_;
        setup_picker_row_ = nullptr;
        std::vector<ServiceSnapshot::SetupSource> sources;
        if (!fetch.reached || !fetch.valid || fetch.state != ServiceSnapshot::ResultState::acknowledged ||
            service.link != ServiceSnapshot::Link::up || !service_->last_reply("STATUS").valid ||
            !row_capability_supported(*selected, service) || !picker_fresh(*selected) ||
            json_raw_value(json_raw_value(service.status_json, "capabilities"), "device_inventory") != "true" ||
            !ServiceSnapshot::parse_setup_sources(fetch.body, sources) || sources.empty()) {
            show_toast("No controller is ready for setup. Reconnect it and try again."); return;
        }
        OverlayRequest request;
        request.kind = OverlayKind::picker; request.title = selected->label;
        request.label = "Choose a controller";
        for (const auto &source : sources) {
            request.items.push_back(source.label);
            // The parser supplies the complete encoded selection, never a label.
            request.item_values.push_back(source.selection);
        }
        overlay_row_ = selected; overlay_.open(request, feedback);
        return;
    }
    if (picker_row_) {
        if (back) { picker_row_ = nullptr; picker_request_ = 0; return; }
        const auto fetch = service_->fetch_request(picker_request_);
        if (fetch.state == ServiceSnapshot::ResultState::queued) return;
        const Row *selected = picker_row_;
        picker_row_ = nullptr; picker_request_ = 0;
        OverlayRequest request;
        request.kind = OverlayKind::picker; request.title = selected->label; request.label = selected->label;
        bool valid = fetch.reached && fetch.valid && fetch.state == ServiceSnapshot::ResultState::acknowledged &&
            action_fresh(*selected, service) && picker_fresh(*selected) && row_capability_supported(*selected, service);
        if (valid && selected->picker_source == PickerSource::controller_rules) {
            std::vector<ServiceSnapshot::ControllerRule> rules;
            valid = ServiceSnapshot::parse_controller_rules(fetch.body, rules);
            request.label = "Choose a saved controller";
            if (valid) for (const auto &rule : rules) {
                request.items.push_back(ServiceSnapshot::controller_rule_label(rule)); request.item_values.push_back(rule.id);
            }
        } else if (valid && selected->picker_source == PickerSource::bluetooth_paired) {
            for (const auto &device : ServiceSnapshot::parse_bluetooth_paired(fetch.body)) {
                request.items.push_back(device.label); request.item_values.push_back(device.address);
            }
        } else if (valid) {
            const auto array = json_raw_value(fetch.body, selected->picker_key ? selected->picker_key : "items");
            valid = !array.empty() && array.front() == '[';
            std::size_t pos = 1;
            while (valid && pos < array.size()) {
                json_detail::skip_space(array, pos);
                if (pos == array.size() || array[pos] == ']') break;
                const auto start = pos;
                valid = array[pos] == '"' && json_detail::value_end(array, pos);
                if (!valid) break;
                const auto value = JsonReply::decode(array.substr(start, pos - start));
                valid = !value.empty() && std::none_of(value.begin(), value.end(),
                    [](unsigned char c) { return c < 32 || c == 127; }) &&
                    std::find(request.item_values.begin(), request.item_values.end(), value) == request.item_values.end();
                if (!valid) break;
                request.items.push_back(value); request.item_values.push_back(value);
                json_detail::skip_space(array, pos);
                if (pos < array.size() && array[pos] == ',') ++pos;
            }
        }
        if (!valid || request.items.empty()) {
            show_toast(selected->picker_source == PickerSource::controller_rules ?
                "No saved controllers are available." : "No choices are available right now."); return;
        }
        overlay_row_ = selected; overlay_.open(request, feedback); return;
    }

    if (overlay_.is_open())
    {
        overlay_.update(input, dt, feedback);
        OverlayResult result;
        const bool took = overlay_.take_result(&result) && overlay_row_ != nullptr;
        if (took)
        {
            const Row &row = *overlay_row_;
            if (row.overlay_next != OverlayKind::none && !chain_.active)
                begin_chain(row, result, feedback);
            else if (chain_.active)
                finish_chain(row, result, feedback);
            else
                apply_overlay_result(row, result, feedback);
        }
        else if (chain_.active && !overlay_.is_open())
        {
            /* The second modal was cancelled: drop the chain, post nothing. */
            chain_ = Chain{};
            overlay_row_ = nullptr;
        }
        return;
    }

    /* A chain waiting on its between-modals fetch owns the frame too: it opens
     * the second modal the instant the reply lands, and swallows input until
     * then so the list cannot act under a half-finished answer. */
    if (chain_.active)
    {
        if (back) { chain_ = Chain{}; overlay_row_ = nullptr; return; }
        if (chain_.fetch_pending && service_)
        {
            const auto fetch = service_->fetch_request(chain_.fetch_request);
            if (fetch.state != ServiceSnapshot::ResultState::queued) {
                chain_.fetch_pending = false;
                if (!chain_source_fresh() || !endpoint_fresh("STATUS")) {
                    chain_ = Chain{}; overlay_row_ = nullptr;
                    show_toast("Could not read the profile. No change was sent."); return;
                }
            }
        }
        if (!chain_.fetch_pending)
            open_chain_next(feedback);
        return;
    }

    if (focus_ == Focus::reader)
    {
        /* A fetch-backed reader rebuilds the moment a fresh reply lands, so
         * Refresh and the first open both fill in without blocking a frame. */
        if (reader_refreshable() && reader_fetch_pending_) {
            const auto fetch = service_ ? service_->fetch_request(reader_fetch_request_) : ServiceSnapshot::Fetch{};
            if (fetch.state != ServiceSnapshot::ResultState::queued) {
                reader_fetch_request_ = 0;
                reader_fetch_pending_ = false;
                rebuild_reader_lines(fetch);
                reader_age_ = 0.0f;
            }
        }

        const bool fetch_backed = reader_refreshable();
        const std::size_t lines = reader_visual_lines_.empty() ? reader_lines_.size() : reader_visual_lines_.size();
        const std::size_t window = reader_lines();
        const std::size_t max_scroll = lines > window ? lines - window : 0;
        reader_scroll_ = std::min(reader_scroll_, max_scroll);
        if (input.nav == Direction::down)
        {
            if (reader_scroll_ < max_scroll)
            {
                ++reader_scroll_;
                feedback.play(Cue::tick);
            }
            else
                feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
        }
        else if (input.nav == Direction::up)
        {
            if (reader_scroll_ > 0)
            {
                --reader_scroll_;
                feedback.play(Cue::tick);
            }
            else
                feedback.play(Cue::error, 1.0f, 0.0f, 0.5f);
        }
        if (back || (confirm && !fetch_backed))
        {
            focus_ = Focus::content;
            reader_visual_lines_.clear();
            reader_scroll_ = 0;
            reader_fetch_request_ = 0;
            reader_fetch_pending_ = false;
            feedback.play(Cue::modal_close);
        }
        else if (confirm)
        {
            /* A live reader's confirm re-reads the service rather than
             * closing, so the log or the capture list can be refreshed
             * without leaving the panel. */
            refresh_reader();
            feedback.play(Cue::tick);
        }
        return; /* the reader owns input while it is up */
    }

    rebuild_task(service);
    if (page_prev || page_next) {
        open_section(page_prev ? (section_index_ + sections_.size() - 1) % sections_.size()
                               : (section_index_ + 1) % sections_.size(), feedback);
        page_menu_ = tools_open_ = false;
        button_focus_ = 0;
        rebuild_task(service);
        return;
    }
    if (page_menu_) {
        if (back) { page_menu_ = false; return; }
        if (input.nav == Direction::down) menu_index_ = (menu_index_ + 1) % sections_.size();
        if (input.nav == Direction::up) menu_index_ = (menu_index_ + sections_.size() - 1) % sections_.size();
        if (confirm) {
            open_section(menu_index_, feedback); page_menu_ = false;
            button_focus_ = 0; rebuild_task(service);
        }
        return;
    }
    if (back) {
        if (tools_open_) { tools_open_ = false; button_focus_ = 0; }
        else { page_menu_ = true; menu_index_ = section_index_; }
        feedback.play(Cue::back);
        return;
    }
    if (!tools_open_ && input.is_pressed(Action::west)) {
        std::vector<std::string> lines{task_.lead, "", task_.status, task_.detail, ""};
        if(!toast_text_.empty()) { lines.push_back("Last result"); lines.push_back(toast_text_); lines.push_back(""); }
        for (const auto &f : task_.facts) { lines.push_back(f.label); lines.push_back("  " + f.value); lines.push_back(""); }
        for (const auto &item : task_.items) { lines.push_back(item.title); lines.push_back("  " + item.detail); lines.push_back(""); }
        show_reader(task_.scene == TaskScene::setup ? "Setup mapping review" : std::string(section().name) + " details",
                    std::move(lines), feedback);
        return;
    }
    if (input.is_pressed(Action::menu)) {
        tools_open_ = !tools_open_; tool_index_ = 0;
        feedback.play(Cue::select); return;
    }
    const auto &buttons = tools_open_ ? task_.more : task_.buttons;
    auto &selection = tools_open_ ? tool_index_ : button_focus_;
    if (!buttons.empty()) {
        selection = std::min(selection, buttons.size() - 1);
        if (input.nav == Direction::right || input.nav == Direction::down) {
            selection = (selection + 1) % buttons.size(); feedback.play(Cue::focus);
        } else if (input.nav == Direction::left || input.nav == Direction::up) {
            selection = (selection + buttons.size() - 1) % buttons.size(); feedback.play(Cue::focus);
        }
        row_index_ = buttons[selection].action;
    }
    focus_ = Focus::content;
    {
        const std::size_t rows = buttons.size();
        if (confirm && rows > 0 && row().command && std::string_view(row().command)=="AUDIO_UI")
        {
            const bool accepted=service_ && audio_screen_.press(row(),*service_);
            show_toast(audio_screen_.feedback());
            feedback.play(accepted?Cue::select:Cue::error);
        }
        else if (confirm && rows > 0 && row().app_launch_opt_in)
        {
            const auto &v = app_preference_view_;
            if (row().disabled || row().read_only || !app_preferences_ || !v.available || !v.loaded || v.pending) {
                show_toast(v.pending ? "Still saving. Please wait." :
                           "This setting is unavailable. No change was sent.");
                feedback.play(Cue::error);
            } else {
                // Unknown/invalid state fails closed; an explicit press can repair it.
                const bool enable = !(v.authoritative_known && v.authoritative);
                if (!enable && lifecycle_) {
                    // Revoke before enqueueing file IO, even if that request
                    // is refused or its save fails. No later frame is needed.
                    auto revoked = v;
                    revoked.startup_eligible = false;
                    lifecycle_->observe_consent(revoked);
                }
                const auto request = app_preferences_->request(enable);
                if (!request) {
                    show_toast("Could not save this setting. Try again.");
                    feedback.play(Cue::error);
                } else {
                    app_preference_request_ = request;
                    app_preference_view_ = app_preferences_->read();
                    show_toast(enable ? "Saving On. Ghosttooth API will not start yet." :
                                       "Saving Off. A start already sent may still finish.");
                    feedback.play(Cue::select);
                }
            }
        }
        else if (confirm && rows > 0 && (row().stops_companion || row().restarts_companion))
        {
            /* Resident Stop/Restart: the worker identifies the instance and
             * confirms the result; the frame only queues the receipt. */
            press_row_ = row_index_;
            press_age_ = 0.0f;
            const std::uint64_t receipt = row().stops_companion ? stop_companion() : restart_companion();
            feedback.play(receipt ? Cue::select : Cue::error);
        }
        else if (confirm && rows > 0 && row().starts_companion)
        {
            /* The companion row is the one press that must work when nothing
             * is answering: it hands the packaged ELF to the console's own
             * loader on the console LAN address rather than asking a service that is not
             * there. It therefore never goes through the queued-command path,
             * which refuses while the link is down. */
            press_row_ = row_index_;
            press_age_ = 0.0f;
            feedback.play(start_companion() ? Cue::select : Cue::error);
        }
        else if (confirm && rows > 0)
        {
            const Row &chosen = row();
            app_log("workspace action: page=%s action=%s", section().name, chosen.label);
            const bool link_up = service.link == ServiceSnapshot::Link::up;
            bool status_reached = !service_;
            if (service_) {
                const auto status = service_->last_reply("STATUS");
                status_reached = status.generation != 0 && status.reached && status.valid;
            }
            /* View generation stops moving on a stable service, so action_view_age_
             * passes 5s while the header still says connected. That age is not a
             * dropped link once STATUS has been reached. */
            const bool connected = link_up && status_reached;
            const char *needed = press_poll_endpoint(chosen);
            const bool needed_stale = needed && service_ && !endpoint_fresh(needed);
            const bool gateable = !chosen.disabled && !chosen.read_only &&
                chosen.state != RowState::pending && row_capability_supported(chosen, service) &&
                !row_actions_.pending();
            const bool wait_for_poll = connected && needed_stale && gateable && !picker_fetches_poll(chosen);
            if (service_ && link_up && !status_reached && gateable &&
                (chosen.command || chosen.overlay != OverlayKind::none)) {
                const bool queued = service_->queue_fetch("STATUS");
                show_toast(queued ? "Loading Ghosttooth API status..." : "Could not load that yet. Try again.");
                feedback.play(queued ? Cue::tick : Cue::error, 1.0f, 0.0f, queued ? 1.0f : 0.6f);
                if (!queued) feedback.rumble(0.25f, 0.06f);
            } else if (wait_for_poll) {
                const bool queued = service_->queue_fetch(needed);
                show_toast(queued ? loading_sentence(needed) : "Could not load that yet. Try again.");
                feedback.play(queued ? Cue::tick : Cue::error, 1.0f, 0.0f, queued ? 1.0f : 0.6f);
                if (!queued) feedback.rumble(0.25f, 0.06f);
            } else {
            if (service_ && connected && needed_stale && gateable && picker_fetches_poll(chosen))
                service_->queue_fetch(needed);
            const auto fetch_generation = service_ ? service_->last_reply("RUNTIME_GET").generation : 0;
            const bool fresh = service_ ? connected : (
                action_fresh(chosen, service) && action_view_age_ <= 5.0f && service.generation != 0 &&
                (!toggle_key(chosen.command_argument) ||
                 (action_runtime_generation_ != 0 && action_runtime_age_ <= 5.0f)));
            const auto activation = row_actions_.press(input, chosen, service, fresh,
                [this, &chosen](const char *command, const char *argument, unsigned timeout) {
                    if (!service_) return false;
                    if (std::string_view(command) == "SETUP_SAVE") {
                        setup_save_request_ = service_->queue_fetch_request(command, argument, timeout);
                        return setup_save_request_ != 0;
                    }
                    /* Arming the guided walk establishes the draft itself: queue
                     * SETUP_BEGIN <selection> then SETUP_ARM guided as one atomic
                     * group so the core always sees an active draft when ARM
                     * lands (a lone ARM is refused with setup_not_begun). Falls
                     * back to the plain arm when no source was chosen yet. */
                    if (std::string_view(command) == "SETUP_ARM" && argument &&
                        std::string_view(argument) == "guided" && !setup_selection_.empty())
                        return service_->queue_commands(
                            {{"SETUP_BEGIN", setup_selection_}, {"SETUP_ARM", "guided"}}, timeout);
                    const char *key = toggle_key(chosen.command_argument);
                    // Preserve press intent; never send an inversion of a stale frame copy.
                    return key ? service_->queue_runtime_toggle(key)
                               : service_->queue_command(command, argument, timeout);
                }, fetch_generation,
                chosen.command_argument == LiveSource::setup_begin_first_pad
                    ? resolve_argument(chosen.command_argument, service) : std::string(),
                service_ ? service_->last_reply("RUNTIME_SET").generation : 0);
            if (activation == RowActions::Result::overlay)
            {
                /* The row opens a modal instead of firing a command: the
                 * command, if any, is the modal's result to post. */
                press_row_ = row_index_;
                press_age_ = 0.0f;
                open_overlay_for_row(chosen, service, feedback);
            }
            else if (activation == RowActions::Result::queued)
            {
                press_row_ = row_index_;
                press_age_ = 0.0f;
                feedback.play(Cue::select);
                if (row_actions_.pending()) {
                    runtime_fetch_queued_ = service_ && service_->queue_fetch("RUNTIME_GET");
                    action_pending_age_ = 0.0f;
                    show_toast("Saving. Waiting for confirmation.");
                } else show_toast("Waiting for Ghosttooth API...");
            }
            else if (activation == RowActions::Result::reader)
            {
                /* A reader row: its own detail lines, or a fetch-backed log
                 * or raw-input view that opens empty and fills from the
                 * reply. Confirm on a live one refreshes it (above). */
                const ReaderSource source = chosen.reader == ReaderKind::bluetooth_diagnostics
                                                ? ReaderSource::bluetooth_diagnostics
                                            : chosen.reader == ReaderKind::log_tail
                                                ? ReaderSource::log_tail
                                            : chosen.reader == ReaderKind::raw_input
                                                ? ReaderSource::raw_input
                                            : chosen.reader == ReaderKind::setup_raw
                                                ? ReaderSource::setup_raw
                                                : ReaderSource::detail;
                open_reader(chosen.detail, source, feedback);
            }
            else
            {
                /* Refused, and said so: a row with nothing behind it yet must
                 * not look like a press that silently failed. */
                if (!chosen.disabled && !chosen.read_only && chosen.state != RowState::pending &&
                    toggle_key(chosen.command_argument) && service_ && !row_actions_.pending())
                    service_->queue_fetch("RUNTIME_GET");
                const bool nothing_to_send = !chosen.command && chosen.overlay == OverlayKind::none &&
                    chosen.reader == ReaderKind::none && chosen.detail.empty();
                const char *refusal = chosen.disabled ? "This option is unavailable." :
                    (chosen.read_only || (link_up && nothing_to_send)) ? "This setting cannot be changed here." :
                    !row_capability_supported(chosen, service) ? "Not supported by Ghosttooth API." :
                    row_actions_.pending() ? "Waiting for the previous change to finish." :
                    !link_up ? "Check the Ghosttooth API connection and try again." :
                    "Could not send that yet. Try again.";
                show_toast(refusal);
                feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
                feedback.rumble(0.25f, 0.06f);
            }
            }
        }
    }

    rebuild_task(service);
}

void ToolboxUi::set_qr(QrCode bitcoin, QrCode discord, QrCode patreon)
{
    qr_btc_ = bitcoin;
    qr_discord_ = discord;
    qr_patreon_ = patreon;
}

void ToolboxUi::set_logo(std::uint32_t texture, int width, int height)
{
    logo_texture_ = texture;
    logo_w_ = width;
    logo_h_ = height;
}

} // namespace ghostcontrol

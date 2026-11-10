/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The declared tables every screen is built from.
 *
 * A screen is a Section of Rows, and a row says where its value comes from,
 * or which command it fires, instead of computing anything. Keeping the
 * tables in their own translation units lets a screen be reviewed and
 * rearranged on its own, and keeps the shell free of content.
 *
 * A row never dresses a placeholder up as a reading. `state` says whether the
 * value is real (live), an action to press, or something the service does that
 * this screen does not show yet (pending). Where a value is real, `live` names
 * the field it comes from, so the shell resolves it instead of matching on the
 * row's label -- a reworded row keeps its reading. Where a row does something,
 * `command` names the IPC command confirm fires, with `argument` when it takes
 * one.
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ghostcontrol
{

/** Where a row's value comes from, so the screen can say so. */
enum class RowState
{
    live,    // read from the service now
    pending, // the service implements it; this screen does not show it yet
    action,  // something to press rather than read
};

/** Which service field a live row's value is read from.
 *
 * Naming the source rather than matching on the row's label means a screen can
 * be reworded without silently losing its reading. */
enum class LiveSource
{
    none,
    service_version,  // STATUS "version"
    active_slots,     // STATUS slots whose "connected" is true, counted
    service_state,    // STATUS "service"
    bluetooth_state,  // BLUETOOTH_STATUS "bluetooth"
    paired_count,     // BLUETOOTH_STATUS "paired" array length
    connected_count,  // BLUETOOTH_STATUS "controllers" array length
    active_profile,   // RUNTIME_GET "active_profile"
    fx_summary,       // DUALSENSE_FX_STATUS "effect" as a name
    console_serial,   // CONSOLE_ID "serial"
    console_openpsid, // CONSOLE_ID "openpsid"
    devices_summary,  // DEVICES "devices" array length

    /* Per-slot summaries. slot1..slot4 are contiguous: the shell maps them
     * by offset, so keep them adjacent and in order. */
    slot1, // STATUS slots[0]: "name [vid_pid] -- user N" / "empty"
    slot2,
    slot3,
    slot4,

    /* Runtime switches, as On/Off. */
    runtime_autostart,  // RUNTIME_GET "autostart"
    runtime_background, // RUNTIME_GET "run_in_background"
    runtime_ghostpad,   // RUNTIME_GET "ghostpad_enabled" (+ "listening")
    runtime_wireless,   // RUNTIME_GET "wireless_ds4"
    runtime_fx_enabled, // DUALSENSE_FX_STATUS "enabled"
    runtime_include_prereleases, // RUNTIME_GET "include_prereleases"
    fx_color,           // saved base r/g/b, never inferred animation output
    fx_period_ms,       // DUALSENSE_FX_STATUS "period_ms", as "N ms"
    motion_state,       // DUALSENSE_FX_STATUS "motion_valid": live / idle
    console_model,      // CONSOLE_ID "model"
    setup_message,      // SETUP_STATUS "message"
    setup_state,        // SETUP_STATUS "active": in progress / idle
    bluetooth_pairing,  // BLUETOOTH_STATUS "pairing" seconds, or "closed"
    profile_count,      // PROFILES "profiles" array length

    /* Argument-only sources. Never drawn; resolved at press time into a
     * command argument from live state (toggles, device selection). */
    rt_toggle_autostart,   // RUNTIME_SET "autostart=<opposite>"
    rt_toggle_background,  // RUNTIME_SET "run_in_background=<opposite>"
    rt_toggle_ghostpad,    // RUNTIME_SET "ghostpad_enabled=<opposite>"
    rt_toggle_wireless,    // RUNTIME_SET "wireless_ds4=<opposite>"
    rt_toggle_fx,          // RUNTIME_SET "dualsense_fx_enabled=<opposite>"
    rt_toggle_include_prereleases, // RUNTIME_SET "include_prereleases=<opposite>"
    setup_begin_first_pad, // SETUP_BEGIN hex(path|vid|pid|pad-vid-pid|identity)
    native_pad_available, // owning app's observed connection; not console-wide inventory
    native_light_result,  // actual app output API rc, never an IPC acknowledgement
    native_rumble_result, // durable native event phase and actual motor API rc
    controller_count,     // CONTROLLERS "controllers" array length

    /* BT-R01: BLUETOOTH_STATUS "diagnostics", measured by the service's
     * transport/host/engine. Never the radio address or pairing material. */
    audio_state,    // request-owned Audio endpoint diagnostics
    audio_inventory,
    audio_source,
    audio_preferences,
    bt_radio,        // diagnostics.radio: USB id, interface, open or last session
    bt_traffic,      // diagnostics.radio.counters events/acl + lastRxAgeMs
    bt_drops,        // every drop/stale counter, by kind
    bt_last_failure, // diagnostics.failures: last reason, age, counts
};

/** Which modal a row opens on confirm.
 *
 * The shell hosts it over the list; the widget that is up takes the pad (see
 * overlay.hpp). The row's `overlay_arguments` receive its result, so a screen
 * declares a control and the command it posts, and writes no UI at all. */
enum class OverlayKind
{
    none,     // not a modal row
    range,    // a Slider: {value}
    color,    // a ColorPicker: {r} {g} {b}
    text,     // a Keyboard: {text}
    picker,   // a WheelPicker: {index} and {label}
    keybinder // a KeyBinder: {id} and {label}, a scaffold for remapping
};

/** Where a reader row's lines come from.
 *
 *  The reader shows a row's fixed `detail` lines by default. Two rows want
 *  more than that: the log reader decodes a fetched LOG_TAIL, and the raw
 *  input view filters the "setup raw:" lines out of the same tail. Naming the
 *  source on the row keeps the shell free of content and lets those two rows
 *  re-fetch and rebuild on their own. */
enum class ReaderKind
{
    none,      // use `detail` when it is not empty (the common case)
    detail,    // always the row's own `detail` lines
    log_tail,  // a fetched LOG_TAIL, hex-decoded, scrollable
    raw_input, // historical "setup raw:" lines from LOG_TAIL
    setup_raw, // selected physical source's bounded SETUP_INPUT packet cache
    bluetooth_diagnostics, // request-owned redacted measured readout
};

/** Where a picker row's options come from.
 *
 *  A picker with no other source uses its own `detail` lines as the item
 *  list. A screen can instead feed it from live state or from a fetched
 *  command, so the options reflect the service rather than a fixed list. */
enum class PickerSource
{
    detail,           // the row's own detail lines (default)
    bluetooth_paired, // BLUETOOTH_STATUS "paired", one option per address
    fetch,            // a command's reply, read by picker_command/picker_key
    controller_rules, // fresh CONTROLLERS IDs, never retained last-good options
    setup_sources,    // fresh identified DEVICES input, with connection token
};

struct Row
{
    const char *label;
    const char *value;
    const char *note;
    RowState state = RowState::pending;
    /** Shown in the reader when the row is opened. Empty means nothing to
     *  read, and the row reports that rather than appearing to do nothing.
     *  Owned strings, because some of it is loaded at run time. For a picker
     *  row with no other source of options, these lines are the item list. */
    std::vector<std::string> detail{};
    /** Where a live row's value comes from. */
    LiveSource live = LiveSource::none;
    /** The IPC command confirm fires, and its argument when it takes one. */
    const char *command = nullptr;
    const char *argument = nullptr;
    /** When set, the argument is built from live state at press time and
     *  `argument` is ignored. An empty resolution refuses the press. */
    LiveSource command_argument = LiveSource::none;

    /* ---- overlay metadata: what confirm opens, and what its result posts ---- */

    /** The modal confirm opens. Takes precedence over `command` and `detail`,
     *  so a settable reading can still be shown live on the row. */
    OverlayKind overlay = OverlayKind::none;
    /** The range modal's bounds and starting thumb, and its unit suffix. */
    float overlay_min = 0.0f;
    float overlay_max = 0.0f;
    float overlay_step = 0.0f; // 0: continuous
    float overlay_value = 0.0f;
    const char *overlay_unit = nullptr;
    /** The command the modal's result fires, once per template below. */
    const char *overlay_command = nullptr;
    /** One argument template per command queued. Placeholders are substituted
     *  from the result: {value} {r} {g} {b} {text} {index} {label} {id}
     *  {parent} {action}. A colour row names three (one per channel); a range
     *  row names one; a chained row (below) names {parent} (the first modal's
     *  chosen option) and {action} (a key binder's newly bound button). */
    std::vector<const char *> overlay_arguments{};

    /* ---- reader and picker sources, and the command deadline ---- */

    /** Where the reader's lines come from when this row is opened. `none`
     *  keeps the original behaviour: a non-empty `detail` opens the reader. */
    ReaderKind reader = ReaderKind::none;
    /** Where a picker row's options come from. */
    PickerSource picker_source = PickerSource::detail;
    /** For PickerSource::fetch: the command that fills the options, and the
     *  array field in its reply they are read from (e.g. PROFILE_FILES,
     *  "imports"). The shell queues the command when the picker opens and
     *  reads the reply from the fetch store, so options reflect the service.
     *  A wave-2 screen may queue it earlier (on section entry) so the first
     *  open is already populated. */
    const char *picker_command = nullptr;
    const char *picker_key = nullptr;
    /** The per-command wire deadline for a command row, in milliseconds; 0
     *  keeps the client's default. A command that reaches the network
     *  (UPDATE_CHECK) needs more than three seconds, so it names its own. */
    unsigned command_timeout_ms = 0;

    /* ---- chained modals, text seeding and hex arguments (wave 2c) --------
     * A row that needs two answers in order (pick a profile, then type its new
     * name) names a second modal in `overlay_next`. The shell opens it once the
     * first is accepted, seeding it from that result; the row's
     * overlay_command then fires with a result that carries both. An optional
     * `overlay_next_command` fetch (e.g. PROFILE_GET) runs between the two,
     * its argument built from the first result, so a key binder can open on the
     * profile's real bindings. See docs/OVERLAY-API.md. */

    /** The modal opened after this one is accepted. `none` posts straight
     *  away, which is every row that is not a chain. */
    OverlayKind overlay_next = OverlayKind::none;
    /** A fetch queued between the two modals, and its argument template,
     *  filled from the first modal's result ({label} of the chosen option). */
    const char *overlay_next_command = nullptr;
    const char *overlay_next_argument = nullptr;
    /** Seed text a text modal opens with, and its character cap (0: the
     *  keyboard's own default). A chained text modal is seeded from the first
     *  result instead, so a rename starts with the current name. */
    const char *overlay_text = nullptr;
    unsigned overlay_max_length = 0;
    /** Hex-encode each produced argument before queueing it. PROFILE_RENAME
     *  and SETUP_BEGIN decode their argument from hex on the service side, so
     *  a row whose command expects that names it here. */
    bool overlay_hex = false;
    /** Explicit activation policy; appended for aggregate/API compatibility. */
    bool disabled = false;
    bool read_only = false;
    /** Fired by the shell itself rather than as an IPC command: pushing the
     *  companion the app carries to the console loader. Only the Service page
     *  uses it, and it needs no reachable service -- that is the point. */
    bool starts_companion = false;
    bool shows_launch_result = false;
    bool app_launch_opt_in = false; // app-private, offline; never resident IPC
    /** Resident GhosttoothAPI Stop / Restart through the lifecycle worker
     *  (APP-SVC-04): identity-checked EXIT with its own receipt. Distinct
     *  from the controller-work START/STOP/RESTART command rows. */
    bool stops_companion = false;
    bool restarts_companion = false;
    bool full_profile_editor = false; // explicit value form, separate from restricted physical capture
};

struct Section
{
    const char *name;
    const char *blurb;
    std::vector<Row> rows;
};

/* Bounded JSON token reader. Only top-level members match; nested keys and
 * strings containing key-like text cannot masquerade as runtime settings.
 * Invalid/truncated documents and duplicate requested keys have no reading. */
namespace json_detail
{
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline void skip_space(std::string_view s, std::size_t &i)
{ while (i < s.size() && space(s[i])) ++i; }
inline bool string_end(std::string_view s, std::size_t &i)
{
    if (i >= s.size() || s[i++] != '"') return false;
    while (i < s.size()) {
        unsigned char c = s[i++];
        if (c == '"') return true;
        if (c < 32) return false;
        if (c == '\\') {
            if (i == s.size()) return false;
            const char escape = s[i++];
            if (escape == 'u') {
                for (int n = 0; n < 4; ++n) {
                    if (i == s.size()) return false;
                    const char h = s[i++];
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                          (h >= 'A' && h <= 'F'))) return false;
                }
            } else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
                return false;
        }
    }
    return false;
}
inline bool value_end(std::string_view s, std::size_t &i, unsigned depth = 0)
{
    skip_space(s, i);
    if (i == s.size() || depth > 32) return false;
    if (s[i] == '"') return string_end(s, i);
    if (s[i] == '{' || s[i] == '[') {
        const bool object = s[i++] == '{';
        const char close = object ? '}' : ']';
        skip_space(s, i);
        if (i < s.size() && s[i] == close) { ++i; return true; }
        for (;;) {
            if (object) {
                if (!string_end(s, i)) return false;
                skip_space(s, i);
                if (i == s.size() || s[i++] != ':') return false;
            }
            if (!value_end(s, i, depth + 1)) return false;
            skip_space(s, i);
            if (i == s.size()) return false;
            if (s[i] == close) { ++i; return true; }
            if (s[i++] != ',') return false;
            skip_space(s, i);
        }
    }
    for (auto literal : {std::string_view("true"), std::string_view("false"), std::string_view("null")})
        if (s.substr(i, literal.size()) == literal) { i += literal.size(); return true; }
    if (s[i] == '-') ++i;
    if (i == s.size()) return false;
    if (s[i] == '0') ++i;
    else {
        if (s[i] < '1' || s[i] > '9') return false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (i < s.size() && s[i] == '.') {
        const auto start = ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == start) return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        const auto start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == start) return false;
    }
    return true;
}
}
inline std::string_view json_raw_value(std::string_view json, std::string_view key)
{
    std::size_t i = 0;
    json_detail::skip_space(json, i);
    if (i == json.size() || json[i++] != '{') return {};
    std::string_view found;
    bool matched = false;
    json_detail::skip_space(json, i);
    if (i < json.size() && json[i] != '}') for (;;) {
        const auto name_start = i;
        if (!json_detail::string_end(json, i)) return {};
        const auto name = json.substr(name_start + 1, i - name_start - 2);
        json_detail::skip_space(json, i);
        if (i == json.size() || json[i++] != ':') return {};
        json_detail::skip_space(json, i);
        const auto start = i;
        if (!json_detail::value_end(json, i)) return {};
        if (name == key) {
            if (matched) return {};
            matched = true;
            found = json.substr(start, i - start);
        }
        json_detail::skip_space(json, i);
        if (i == json.size()) return {};
        if (json[i] == '}') break;
        if (json[i++] != ',') return {};
        json_detail::skip_space(json, i);
    }
    if (i == json.size() || json[i++] != '}') return {};
    json_detail::skip_space(json, i);
    return i == json.size() ? found : std::string_view{};
}
inline std::string_view json_value(std::string_view json, std::string_view key)
{
    auto raw = json_raw_value(json, key);
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        return raw.substr(1, raw.size() - 2);
    return raw;
}

/* One builder per section, so a screen is its own file. */
Section build_screen_controllers();
Section build_screen_setup();
Section build_screen_bluetooth();
Section build_screen_profiles();
Section build_screen_lightfx();
Section build_screen_service();
Section build_screen_updates();
Section build_screen_diagnostics();
Section build_screen_donate();
Section build_screen_about();
Section build_screen_audio();

} // namespace ghostcontrol

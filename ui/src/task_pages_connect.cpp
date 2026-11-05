/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task_screen.hpp"

#include <charconv>

namespace ghostcontrol {
namespace {

// Resolve presentation through the same reader as the command catalog, even
// when a task has no corresponding read-only catalog entry.
std::string reading(const Section &catalog, const TaskRead &read, LiveSource source)
{
    for (const auto &row : catalog.rows)
        if (row.live == source) return read(row);
    Row probe{.label="", .value="", .note="", .state=RowState::live,
              .live=source, .read_only=true};
    return read(probe);
}

bool service_available(TaskPage &page, const ServiceSnapshot::View &view)
{
    if (view.link == ServiceSnapshot::Link::up) return true;
    const bool waiting = view.link == ServiceSnapshot::Link::connecting;
    page.status = waiting ? "Waiting for Ghosttooth API" : "Ghosttooth API offline";
    page.tone = waiting ? TaskTone::busy : TaskTone::warning;
    page.detail = view.detail.empty()
        ? "Open the Ghosttooth API page and select Start Ghosttooth API."
        : view.detail;
    return false;
}

// Keep complete endpoint text; in particular do not pass failure reasons through
// a shortened status label. Decode common JSON string escapes for prose.
std::string text(std::string_view json, std::string_view key)
{
    const auto raw = json_raw_value(json, key);
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return {};
    const auto value = raw.substr(1, raw.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 == value.size()) { out += value[i]; continue; }
        const char escape = value[++i];
        switch (escape) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '"': case '\\': case '/': out += escape; break;
        default: out += '\\'; out += escape; break;
        }
    }
    return out;
}

bool positive_seconds(std::string_view raw, unsigned long long &seconds)
{
    seconds = 0;
    if (raw.empty()) return false;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), seconds);
    return result.ec == std::errc{} && result.ptr == raw.data() + raw.size() && seconds > 0;
}

} // namespace

TaskPage controllers_task(const Section &catalog, const ServiceSnapshot::View &view,
                          const TaskRead &read)
{
    TaskPage page;
    page.scene = TaskScene::controllers;
    page.eyebrow = "Controllers";
    page.title = "Your controllers";
    page.lead = "Check your connected controllers and saved device settings.";
    page.status = reading(catalog, read, LiveSource::native_pad_available);
    page.detail = "Use Setup to map a new controller. Choose output changes its saved controller type. Game output is unavailable in this build.";
    page.facts.push_back({"App controller", page.status});
    if (view.link == ServiceSnapshot::Link::up) {
        page.facts.push_back({"Detected devices", reading(catalog, read, LiveSource::devices_summary)});
        page.facts.push_back({"Saved controller rules", reading(catalog, read, LiveSource::controller_count)});
        // Physical identities come only from the validated DEVICES parser.
        std::vector<ServiceSnapshot::SetupSource> sources;
        if (ServiceSnapshot::parse_setup_sources(view.devices_json, sources)) {
            page.facts.push_back({"Identified input sources", std::to_string(sources.size())});
            for (const auto &source : sources)
                page.items.push_back({source.label, source.edits_existing ? "Saved profile" : "Physical input source"});
        } else {
            page.facts.push_back({"Identified input sources", "Unavailable"});
        }
        for (const auto &slot : {std::pair{LiveSource::slot1, "Virtual output 1"},
                                std::pair{LiveSource::slot2, "Virtual output 2"},
                                std::pair{LiveSource::slot3, "Virtual output 3"},
                                std::pair{LiveSource::slot4, "Virtual output 4"}})
            page.items.push_back({slot.second, reading(catalog, read, slot.first)});
    }
    task_button(page, catalog, "Refresh devices", "Refresh devices");
    task_button(page, catalog, "Virtual controller type", "Choose output");
    return page;
}

TaskPage setup_task(const Section &catalog, const ServiceSnapshot::View &view,
                    const TaskRead &read)
{
    TaskPage page;
    page.scene = TaskScene::setup;
    page.eyebrow = "Controller setup";
    page.title = "Set up your controller";
    page.lead = "Plug in your controller, choose it, then follow the button prompts.";
    /* The four steps are the wizard's real states. Their labels are filled from
     * what the service reports at that moment (the control it is asking for,
     * the draft it already holds), page.step moves only on SETUP_STATUS, and
     * progress stays unmeasured because no producer publishes a step/total. */
    page.steps = {"Choose controller", "Start guided", "Requested input", "Review / save"};
    page.step = 0;
    if (!service_available(page, view)) return page;
    const auto caps = json_raw_value(view.status_json, "capabilities");
    if (json_raw_value(caps, "setup") != "true" ||
        json_raw_value(caps, "device_inventory") != "true") {
        page.status = "Controller setup unavailable";
        page.detail = "The connected Ghosttooth API build does not provide physical controller setup. This is not a missing controller.";
        page.tone = TaskTone::warning;
        return page;
    }
    if (json_raw_value(view.setup_json, "ok") != "true" ||
        (json_raw_value(view.setup_json, "active") != "true" &&
         json_raw_value(view.setup_json, "active") != "false")) {
        page.status = "Waiting for setup";
        page.detail = text(view.setup_json, "error");
        if (page.detail.empty()) page.detail = "The service has not returned a setup status yet.";
        page.tone = TaskTone::warning;
        // Setup is supported but the session state is unknown, so the flow's
        // entry verb stays on the pad: choosing a controller starts a fresh
        // session. The verbs that need a live session stay in Tools because
        // the service would refuse them out of order.
        task_button(page, catalog, "Choose controller", "Choose controller");
        return page;
    }
    const bool active = json_raw_value(view.setup_json, "active") == "true";
    // Review only draft data actually supplied by SETUP_STATUS. PROFILE_GET
    // is persisted state, not this draft, and must never stand in for it.
    bool mappings_reported = false;
    bool mappings_valid = true;
    std::vector<TaskItem> mappings;
    // What this session is about: the input the service leased and the profile
    // it will write. Every state that has a session shows these two first, so
    // the column reads the same from the first prompt to the save.
    if (active) {
        auto source = text(view.setup_json, "path");
        if (source.empty()) source = text(view.setup_json, "controller_id");
        auto profile = text(view.setup_json, "profile_id");
        if (profile.empty()) profile = text(view.setup_json, "profile");
        page.facts.push_back({"Selected controller", source.empty() ? "Not reported by the service" : source});
        page.facts.push_back({"Save to profile", profile.empty() ? "Not reported by the service" : profile});
        const auto array = json_raw_value(view.setup_json, "mappings");
        mappings_reported = !array.empty();
        if (mappings_reported) {
            mappings_valid = array.size() >= 2 && array.front() == '[' && array.back() == ']';
            std::size_t at = 1;
            while (mappings_valid && at < array.size()) {
                json_detail::skip_space(array, at);
                if (at < array.size() && array[at] == ']') break;
                const auto start = at;
                if (at >= array.size() || array[at] != '{' || !json_detail::value_end(array, at)) {
                    mappings_valid = false; break;
                }
                const auto object = array.substr(start, at - start);
                const auto from = text(object, "source"), to = text(object, "target");
                if (from.empty() || to.empty() || mappings.size() >= 128) { mappings_valid = false; break; }
                mappings.push_back({from, to});
                json_detail::skip_space(array, at);
                if (at < array.size() && array[at] == ',') ++at;
                else if (at >= array.size() || array[at] != ']') mappings_valid = false;
            }
        }
        if (!mappings_reported) page.items.push_back({"Mapping preview unavailable", "This service does not send its draft mappings."});
        else if (!mappings_valid) page.items.push_back({"Mapping preview unavailable", "The service returned an invalid draft preview."});
        else if (mappings.empty()) page.items.push_back({"No mappings to review yet", "Start the guide to learn your controls."});
        else page.items = mappings;
    }
    const auto target_raw = json_raw_value(view.setup_json, "target");
    const auto target = text(view.setup_json, "target");
    // The resident producer sends target names; the older producer sends an
    // integer (-1 means unarmed). Neither SETUP_STATUS producer reports a
    // measured step/total. Do not manufacture a percentage or completion.
    int target_number = -1;
    bool numeric_target = false;
    if (!target_raw.empty()) {
        const auto parsed = std::from_chars(target_raw.data(), target_raw.data() + target_raw.size(), target_number);
        numeric_target = parsed.ec == std::errc{} &&
            parsed.ptr == target_raw.data() + target_raw.size() && target_number >= 0;
    }
    const bool capturing = active && (!target.empty() || numeric_target);
    page.detail = reading(catalog, read, LiveSource::setup_message);
    const auto message = text(view.setup_json, "message");
    if (!message.empty()) page.detail = message;
    // A placeholder dash is not an explanation: every state below supplies the
    // sentence the person needs when the service has nothing to say.
    if (page.detail == "-" || page.detail == "Not reported") page.detail.clear();
    if (!active) {
        std::vector<ServiceSnapshot::SetupSource> sources;
        bool can_edit = false;
        if (ServiceSnapshot::parse_setup_sources(view.devices_json, sources)) {
            for (const auto &source : sources) if (source.edits_existing) can_edit = true;
        }
        page.title = can_edit ? "Edit your controller" : "Set up your controller";
        page.status = can_edit ? "Choose a controller to edit" : "Choose a controller to begin";
        const auto detected = reading(catalog, read, LiveSource::devices_summary);
        if (detected != "-" && detected != "Not reported" && !detected.empty())
            page.facts.push_back({"Detected devices", detected});
        if (page.detail.empty())
            page.detail = can_edit
                ? "A plugged controller already has a profile. Choose it to edit that profile."
                : "Plug the controller in with USB, then choose it. Setup opens a draft profile for that input.";
        task_button(page, catalog, "Choose controller", can_edit ? "Edit controller" : "Choose controller");
        return page;
    }
    if (json_raw_value(view.setup_json, "source_valid") == "false") {
        // The leased input stopped reporting. The draft is still the service's,
        // but no capture can land, so the wizard returns to the chooser rather
        // than promising a prompt that cannot be answered.
        page.status = "Controller source unavailable";
        page.tone = TaskTone::warning;
        if (page.detail.empty())
            page.detail = "The chosen input stopped reporting. Choose the controller again to restart the guide.";
        task_button(page, catalog, "Choose controller", "Choose controller");
        task_button(page, catalog, "Cancel", "Cancel");
        return page;
    }
    if (capturing) {
        page.step = 2;
        page.status = target.empty() ? "Follow the button prompt" : "Press " + target;
        page.tone = TaskTone::busy;
        // The step itself names the control the service is asking for, so the
        // rail and the prompt cannot disagree. What has already been learned is
        // the draft's own list, never a guess at how long the guide is.
        page.steps[2] = target.empty() ? "Follow the prompt" : "Press " + target;
        if (mappings_valid && !mappings.empty())
            page.steps[3] = "Save " + std::to_string(mappings.size()) +
                            (mappings.size() == 1 ? " mapping" : " mappings");
        if (page.detail.empty())
            page.detail = "Press the requested control on your controller. Skip asks the service for the next one.";
        task_button(page, catalog, "Skip", "Skip control");
        task_button(page, catalog, "Cancel", "Cancel");
    } else {
        // Empty target is also the initial Begin state. It does NOT establish
        // guide completion or a successful save. Keep both choices explicit.
        const bool preview = mappings_reported && mappings_valid && !mappings.empty();
        page.step = preview ? 3 : 1;
        page.status = preview ? "Review your mappings" : "Controller selected";
        page.tone = preview ? TaskTone::good : TaskTone::quiet;
        if (preview) {
            page.steps[3] = "Save " + std::to_string(mappings.size()) +
                            (mappings.size() == 1 ? " mapping" : " mappings");
            if (page.detail.empty())
                page.detail = "Press Square to inspect every draft mapping, then Save writes them to the profile.";
        } else if (page.detail.empty()) {
            page.detail = "Start guided learns each control in turn. Tools also has Learn one button and Learn one stick.";
        }
        if (preview) task_button(page, catalog, "Save", "Save mappings");
        task_button(page, catalog, "Start", "Start guided");
        if (!preview) task_button(page, catalog, "Save", "Save mappings");
        task_button(page, catalog, "Cancel", "Cancel");
    }
    return page;
}

TaskPage bluetooth_task(const Section &catalog, const ServiceSnapshot::View &view,
                        const TaskRead &read)
{
    TaskPage page;
    page.scene = TaskScene::bluetooth;
    page.eyebrow = "Bluetooth";
    page.title = "Connect over Bluetooth";
    page.lead = "DualSense: hold Create + PS. DualShock 4: hold SHARE + PS. Release when the light bar blinks and stay near the console.";
    if (!service_available(page, view)) return page;
    if (json_raw_value(json_raw_value(view.status_json, "capabilities"), "bluetooth") != "true") {
        page.status = "Bluetooth unavailable";
        page.detail = "The connected Ghosttooth API build does not provide Bluetooth. Pairing cannot start on this API build.";
        page.tone = TaskTone::warning;
        return page;
    }
    const auto state = text(view.bluetooth_json, "bluetooth");
    if (json_raw_value(view.bluetooth_json, "ok") != "true" || state.empty()) {
        page.status = "Waiting for the radio";
        page.detail = text(view.bluetooth_json, "error");
        if (page.detail.empty()) page.detail = "The service has not returned the radio status yet.";
        page.tone = TaskTone::warning;
        return page;
    }
    page.detail = text(view.bluetooth_json, "reason");
    page.facts.push_back({"Paired", reading(catalog, read, LiveSource::paired_count)});
    page.facts.push_back({"Connected", reading(catalog, read, LiveSource::connected_count)});
    unsigned long long seconds = 0;
    // Forget needs something to forget: its picker is fed by the saved device
    // list, so an empty list would offer a verb with nothing behind it and the
    // row stays in Tools until one is saved.
    const bool forgettable = !view.bluetooth_paired.empty();
    auto forget = [&] {
        if (forgettable) task_button(page, catalog, "Forget", "Forget controller");
    };
    if (positive_seconds(json_raw_value(view.bluetooth_json, "pairing"), seconds)) {
        page.status = "Searching for a controller";
        page.tone = TaskTone::busy;
        page.facts.insert(page.facts.begin(), {"Time remaining", std::to_string(seconds) + " seconds"});
        // Pair stays available either way, so a search that found nothing can
        // be restarted from the pad instead of only from Tools.
        task_button(page, catalog, "Pair", "Pair controller");
        forget();
    } else if (state == "failed") {
        page.status = "Radio failed";
        page.tone = TaskTone::warning;
        task_button(page, catalog, "Try again", "Retry radio");
        forget();
    } else if (state == "trying") {
        page.status = "Opening the radio";
        page.tone = TaskTone::busy;
    } else {
        page.status = state == "ready" ? "Radio ready" : state == "stopped" ? "Radio off" : state;
        page.tone = state == "ready" ? TaskTone::good : TaskTone::quiet;
        task_button(page, catalog, "Pair", "Pair controller");
        forget();
    }
    for (const auto &device : view.bluetooth_paired)
        page.items.push_back({device.label, "Saved controller"});
    return page;
}

} // namespace ghostcontrol

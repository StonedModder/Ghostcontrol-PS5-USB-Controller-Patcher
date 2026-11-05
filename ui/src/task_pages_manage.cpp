/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task_screen.hpp"
#include "json_reply.hpp"
#include <algorithm>

namespace ghostcontrol {
namespace {

bool reachable(const ServiceSnapshot::View &view) {
    return view.link == ServiceSnapshot::Link::up;
}

std::string unavailable(const ServiceSnapshot::View &view) {
    return view.link == ServiceSnapshot::Link::connecting
        ? "Waiting for Ghosttooth API" : "Ghosttooth API unavailable";
}

// View endpoints retain last-good replies. Never expose them as current while
// disconnected; even online these are explicitly the latest polled readings.
std::string reading(const Section &catalog, const TaskRead &read,
                    const ServiceSnapshot::View &view, LiveSource source) {
    if (!reachable(view)) return unavailable(view);
    const auto value = task_read(catalog, read, source);
    return value.empty() || value == "-" ? "Not reported" : value;
}

// PROFILES is a top-level string array, not nested metadata or object names.
// Reject the entire list on invalid shape, duplicate IDs or capacity overflow.
// The same service ID alphabet keeps decoded names safe without renaming them.
bool profile_ids(std::string_view json, std::vector<std::string> &ids) {
    ids.clear();
    if (json.size() > 65536) return false;
    const JsonReply document(json);
    if (!document.valid || document.raw("ok") == "false") return false;
    const auto array = json_raw_value(json, "profiles");
    if (array.size() < 2 || array.front() != '[' || array.back() != ']') return false;
    std::vector<std::string> parsed;
    std::size_t pos = 1;
    json_detail::skip_space(array, pos);
    if (pos < array.size() && array[pos] == ']') return true;
    for (;;) {
        if (parsed.size() == 128) return false;
        const auto start = pos;
        if (!json_detail::string_end(array, pos)) return false;
        auto id = JsonReply::decode(array.substr(start, pos - start));
        if (id.empty() || id.size() > 63 ||
            !std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            }) || std::find(parsed.begin(), parsed.end(), id) != parsed.end()) return false;
        parsed.push_back(std::move(id));
        json_detail::skip_space(array, pos);
        if (pos >= array.size()) return false;
        if (array[pos] == ']') {
            if (pos + 1 != array.size()) return false;
            ids = std::move(parsed);
            return true;
        }
        if (array[pos++] != ',') return false;
        json_detail::skip_space(array, pos);
    }
}

} // namespace

TaskPage profiles_task(const Section &catalog, const ServiceSnapshot::View &view,
                       const TaskRead &read) {
    TaskPage page;
    page.scene = TaskScene::profiles;
    page.eyebrow = "Your library";
    page.title = "Your button layouts";
    page.lead = "Create a layout, edit its buttons and stick settings, or share one through the imports and exports folders.";
    page.facts.push_back({"Saved default profile",
        reading(catalog, read, view, LiveSource::active_profile)});
    std::vector<std::string> ids;
    if (!reachable(view)) {
        page.status = unavailable(view);
        page.detail = "Open Ghosttooth API and start it to load your saved layouts.";
        page.tone = view.link == ServiceSnapshot::Link::connecting ? TaskTone::busy : TaskTone::warning;
    } else if (!profile_ids(view.profiles_json, ids)) {
        page.status = "Library not reported";
        page.detail = "Your saved layouts could not be loaded. Try Reload profiles in Tools.";
        page.tone = TaskTone::warning;
    } else {
        page.status = ids.empty() ? "Your library is empty" : "Saved profiles";
        page.detail = "Choose a profile to edit its mappings. Controllers can use different profiles.";
        page.facts.push_back({"Profiles in library", std::to_string(ids.size())});
        for (const auto &id : ids) page.items.push_back({id, "Saved profile ID"});
        page.tone = TaskTone::quiet;
    }
    /* The four verbs this page is for: make a layout, change one, bring one in
     * from the sharing folder, and write one out to it. The page cursor walks
     * these, so import and export are reachable without opening Tools; Rename
     * and Reload stay in Tools because they are repairs, not the job. */
    task_button(page, catalog, "Create profile", "Create profile");
    task_button(page, catalog, "Edit all profile settings", "Edit profile");
    task_button(page, catalog, "Import profile", "Import");
    task_button(page, catalog, "Export profile", "Export");
    return page;
}

TaskPage lights_task(const Section &catalog, const ServiceSnapshot::View &view,
                     const TaskRead &read) {
    TaskPage page;
    page.scene = TaskScene::lights;
    page.eyebrow = "Lights and rumble";
    page.title = "Lights and vibration";
    page.lead = "Choose a lightbar effect and base colour, or try a short rumble test.";
    page.status = reachable(view) ? "Saved effect settings" : unavailable(view);
    page.tone = reachable(view) ? TaskTone::quiet :
        view.link == ServiceSnapshot::Link::connecting ? TaskTone::busy : TaskTone::warning;
    page.detail = "Change the lightbar or run a short vibration test. Check the controller to confirm the result.";
    page.facts = {
        {"Saved lightbar enabled", reading(catalog, read, view, LiveSource::runtime_fx_enabled)},
        {"Saved effect", reading(catalog, read, view, LiveSource::fx_summary)},
        {"Saved base colour (RGB)", reading(catalog, read, view, LiveSource::fx_color)},
        {"Saved cycle length", reading(catalog, read, view, LiveSource::fx_period_ms)}
    };
    // Native results are app observations, never inferred from service FX
    // counters or current_r/current_g/current_b. Callback owns their wording.
    page.items.push_back({"Lightbar result",
        reading(catalog, read, view, LiveSource::native_light_result)});
    page.items.push_back({"Rumble result",
        reading(catalog, read, view, LiveSource::native_rumble_result)});
    task_button(page, catalog, "Lightbar effect", "Choose effect");
    task_button(page, catalog, "Lightbar colour", "Choose colour");
    task_button(page, catalog, "Test rumble", "Test rumble");
    return page;
}

TaskPage service_task(const Section &catalog, const ServiceSnapshot::View &view,
                      const TaskRead &read) {
    TaskPage page;
    page.scene = TaskScene::service;
    page.eyebrow = "Ghosttooth API";
    page.title = "Ghosttooth API";
    page.lead = "Start, stop or restart the controller API. Input controls and settings are in Tools.";
    page.facts.push_back({"Ghosttooth API connection", reachable(view) ? "Reachable" : unavailable(view)});
    page.facts.push_back({"Controller input",
        reading(catalog, read, view, LiveSource::service_state)});
    // Process lifecycle is the task of this page, independent of the logical
    // input state. Keep these discoverable even when the API is absent.
    task_button(page, catalog, "Start Ghosttooth API", "Start Ghosttooth API");
    task_button(page, catalog, "Stop Ghosttooth API", "Stop Ghosttooth API");
    task_button(page, catalog, "Restart Ghosttooth API", "Restart Ghosttooth API");
    task_button(page, catalog, "Start Ghosttooth API when app opens", "Toggle app autostart");
    if (!reachable(view)) {
        page.status = unavailable(view);
        page.tone = view.link == ServiceSnapshot::Link::connecting ? TaskTone::busy : TaskTone::warning;
        page.detail = "Start the console ELF loader, then select Start Ghosttooth API. App autostart allows one launch attempt when this app opens.";
        if (!view.detail.empty()) page.items.push_back({"Connection detail", view.detail});

        return page;
    }
    // STATUS.service describes input handling, not whether the IPC service
    // exists. An unknown state must not be guessed as stopped or running.
    const auto state = json_value(view.status_json, "service");
    if (state == "running") {
        page.status = "Controller input is running";
        page.tone = TaskTone::good;
        page.detail = "Ghosttooth API is connected. Stopping or restarting it can disconnect service-managed controllers.";
    } else if (state == "stopped") {
        page.status = "Ghosttooth API ready. Input stopped.";
        page.tone = TaskTone::quiet;
        page.detail = "Ghosttooth API is connected. Use Tools to start controller input independently of the API process.";
    } else {
        page.status = "Ghosttooth API connected";
        page.tone = TaskTone::warning;
        page.detail = "Controller input status is unavailable. Check the connection details.";

    }
    return page;
}

} // namespace ghostcontrol

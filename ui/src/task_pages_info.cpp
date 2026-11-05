/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task_screen.hpp"

namespace ghostcontrol {
namespace {

// Catalog indices are command identities, not the order or geometry of a page.
void info_tools(TaskPage &page, const Section &catalog)
{
    for (std::size_t i = 0; i < catalog.rows.size(); ++i) {
        if (!task_actionable(catalog.rows[i])) continue;
        bool primary = false;
        for (const auto &button : page.buttons)
            if (button.action == i) primary = true;
        if (!primary) page.more.push_back({i, catalog.rows[i].label});
    }
}

std::string report_value(const Section &catalog, const TaskRead &read,
                         const ServiceSnapshot::View &view, LiveSource source)
{
    // View retains last-good endpoint data. Never pass it off as current when
    // the overall service link is absent; endpoint freshness is not in View.
    if (view.link != ServiceSnapshot::Link::up) return "Not available while service is unconfirmed";
    if (!read) return "Not reported";
    auto value = task_read(catalog, read, source);
    if (value.empty() || value == "-" || value == "--") return "Not reported";
    return value;
}

} // namespace

TaskPage diagnostics_task(const Section &catalog, const ServiceSnapshot::View &view,
                          const TaskRead &read)
{
    TaskPage page;
    page.scene = TaskScene::diagnostics;
    page.eyebrow = "Diagnostics";
    page.title = "Device information";
    page.lead = "Check device information or open a service log.";
    if (view.link == ServiceSnapshot::Link::up) {
        page.status = "Ghosttooth API reachable";
        page.tone = TaskTone::quiet;
        page.detail = "Open the Bluetooth report to refresh the radio measurements.";
    } else if (view.link == ServiceSnapshot::Link::connecting) {
        page.status = "Waiting for Ghosttooth API";
        page.tone = TaskTone::busy;
        page.detail = "Waiting for the service. You can still open the reports.";
    } else {
        page.status = "Ghosttooth API not reachable";
        page.tone = TaskTone::warning;
        page.detail = view.detail.empty() ? "Start Ghosttooth API to load device information." : view.detail;
    }
    page.facts = {
        {"Console model", report_value(catalog, read, view, LiveSource::console_model)},
        {"Radio / interface", report_value(catalog, read, view, LiveSource::bt_radio)},
        {"Traffic / last activity", report_value(catalog, read, view, LiveSource::bt_traffic)},
        {"Dropped packets", report_value(catalog, read, view, LiveSource::bt_drops)},
        {"Last radio failure / reason", report_value(catalog, read, view, LiveSource::bt_last_failure)},
        {"Virtual slots (not app input)", report_value(catalog, read, view, LiveSource::active_slots)},
    };
    page.items = {
        {"Recent log", "Read recent service messages. A reader refresh is a new request, not a continuous stream."},
        {"Bluetooth report", "Inspect radio identity, traffic, drops and failure age together. Missing measurements are not zero counts."},
    };
    task_button(page, catalog, "Recent log", "View log");
    task_button(page, catalog, "Bluetooth radio", "Bluetooth report");
    info_tools(page, catalog);
    return page;
}

TaskPage audio_task(const Section &, const ServiceSnapshot::View &, const TaskRead &)
{
    TaskPage page;
    page.scene = TaskScene::audio;
    page.eyebrow = "Audio";
    page.title = "Headset capture is off.";
    page.lead = "Headset capture is not available in this version.";
    page.status = "Not included in this build";
    page.tone = TaskTone::quiet;
    page.detail = "Controller setup and Bluetooth do not need headset capture.";
    page.facts = {
        {"Headset capture", "Disabled in this build"},
        {"Capture controls", "Start, pause, volume and latency are unavailable"},
    };
    page.items = {
        {"Read-only by design", "No audio command is sent from this page. Reconnecting or restarting the service does not add the missing capture capability."},
    };
    // Do not promote even a future catalog action here: this page describes
    // the current no-capture build and must not offer nonfunctional controls.
    return page;
}

TaskPage donate_task(const Section &catalog, const ServiceSnapshot::View &, const TaskRead &)
{
    TaskPage page;
    page.scene = TaskScene::donate;
    page.eyebrow = "Support";
    page.title = "Keep the toolbox going";
    page.lead = "Console time, controller hardware and the community server come out of pocket. A tip helps keep the bench running.";
    page.status = "Three ways to help";
    page.detail = "Scan a code with a phone, or read the full string under it. Discord is the community; Bitcoin and Patreon keep the work funded.";
    page.tone = TaskTone::quiet;
    /* The three strings also live in the page's own report, because a card's
     * code and a camera do not always agree. */
    page.items = {
        {"Discord", "https://discord.com/invite/kcgQPxhde"},
        {"Bitcoin", "bc1qa9zfgnccajsw8vg7k287qz5a7apf8pefj5jjx5"},
        {"Patreon", "patreon.com/c/u4772250"},
    };
    /* Tools carries the panel that repeats every string in one place. */
    info_tools(page, catalog);
    return page;
}

TaskPage about_task(const Section &catalog, const ServiceSnapshot::View &view,
                    const TaskRead &read)
{
    TaskPage page;
    page.scene = TaskScene::about;
    page.eyebrow = "Ghostcontrol / about";
    page.title = "Ghostcontrol Toolbox";
    page.lead = "Controller setup, Bluetooth and saved button layouts.";
    page.status = "Standalone application";
    page.detail = "Ghostcontrol runs as a standalone app. Thank you to everyone who builds and tests it.";
#if defined(GC_EXISTING_SERVICE_ONLY) && GC_EXISTING_SERVICE_ONLY
    page.status = "Existing service required";
    page.detail = "UI test build. Uses the controller service already running. This build cannot load or replace a companion.";
#endif
    page.tone = TaskTone::quiet;
    page.facts = {
        {"Toolbox edition", "2.0"},
        {"App build number", "Not reported here"},
        {"Service version", report_value(catalog, read, view, LiveSource::service_version)},
    };
    page.items = {
        {"Made possible by people", "The credits name the contributors, dependencies and fonts. Font licences are packaged with the app."},
        {"Tested together", "The tester list is loaded from the packaged acknowledgements. Open it to see the names included in this build."},
    };
    if (view.link == ServiceSnapshot::Link::up)
        page.detail += "";
    else
        page.detail += "";
    task_button(page, catalog, "Credits", "Credits & licences");
    task_button(page, catalog, "Testers", "Testers");
    info_tools(page, catalog);
    return page;
}

} // namespace ghostcontrol

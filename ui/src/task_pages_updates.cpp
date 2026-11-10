/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task_screen.hpp"

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

} // namespace

TaskPage updates_task(const Section &catalog, const ServiceSnapshot::View &view,
                      const TaskRead &read) {
    TaskPage page;
    page.scene = TaskScene::updates;
    page.eyebrow = "Updates";
    page.title = "App updates";
    page.lead = "Check GitHub for a newer build, download a verified ZIP under "
                "/data/ghostcontrol/updates, then install. The title swap runs after you close the app.";
    page.facts.push_back({"Ghosttooth API connection",
        reachable(view) ? "Reachable" : unavailable(view)});
    page.facts.push_back({"Include prereleases",
        reading(catalog, read, view, LiveSource::runtime_include_prereleases)});
    page.items = {
        {"How it works",
         "Check looks at GitHub only. Download keeps a verified ZIP under "
         "/data/ghostcontrol/updates. Install stages the swap; it applies after you close the app. "
         "Include prereleases opts into beta builds."},
    };
    // Four primary verbs for this page. Roll back and the install help stay in Tools.
    task_button(page, catalog, "Check for updates", "Check for updates");
    task_button(page, catalog, "Download verified update", "Download update");
    task_button(page, catalog, "Install update now", "Install update");
    task_button(page, catalog, "Include prereleases", "Include prereleases");
    if (!reachable(view)) {
        page.status = unavailable(view);
        page.tone = view.link == ServiceSnapshot::Link::connecting ? TaskTone::busy
                                                                   : TaskTone::warning;
        page.detail = "Start Ghosttooth API from the Ghosttooth API workspace first. "
                      "Update check, download and install need the service.";
        if (!view.detail.empty()) page.items.push_back({"Connection detail", view.detail});
        return page;
    }
    page.status = "Ready to check for updates";
    page.tone = TaskTone::quiet;
    page.detail = "Check GitHub, download the verified ZIP, then install. "
                  "The staged package applies after you close this app. "
                  "Turn on Include prereleases to consider beta builds.";
    return page;
}

} // namespace ghostcontrol

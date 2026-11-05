/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "screens.hpp"
#include "service_snapshot.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ghostcontrol {

// Presentation is task-specific. The legacy Row aggregates are retained only
// as the internal, tested command/receipt catalog, never as screen geometry.
enum class TaskScene { controllers, setup, bluetooth, profiles, lights, service, diagnostics, audio, donate, about };
enum class TaskTone { quiet, good, busy, warning };
struct TaskFact { std::string label; std::string value; };
struct TaskItem { std::string title; std::string detail; };
struct TaskButton { std::size_t action; std::string label; };
struct TaskPage {
    TaskScene scene = TaskScene::controllers;
    std::string eyebrow;
    std::string title;
    std::string lead;
    std::string status;
    std::string detail;
    TaskTone tone = TaskTone::quiet;
    std::vector<TaskFact> facts;
    std::vector<TaskItem> items;
    std::vector<std::string> steps;
    std::size_t step = 0;
    float progress = -1.0f; // measured only; -1 means no determinate progress
    std::vector<TaskButton> buttons;
    std::vector<TaskButton> more;
};
using TaskRead = std::function<std::string(const Row &)>;

inline bool task_actionable(const Row &r) {
    return !r.read_only && !r.disabled && r.state != RowState::pending &&
        (r.command || r.overlay != OverlayKind::none || r.reader != ReaderKind::none ||
         !r.detail.empty() || r.starts_companion || r.stops_companion ||
         r.restarts_companion || r.app_launch_opt_in || r.shows_launch_result);
}
inline void task_button(TaskPage &page, const Section &catalog,
                        std::string_view catalog_label, std::string label) {
    for (std::size_t i = 0; i < catalog.rows.size(); ++i)
        if (catalog_label == catalog.rows[i].label && task_actionable(catalog.rows[i])) {
            page.buttons.push_back({i, std::move(label)});
            return;
        }
}
inline std::string task_read(const Section &catalog, const TaskRead &read, LiveSource source) {
    for (const auto &r : catalog.rows) if (r.live == source) return read(r);
    return "Not reported";
}

TaskPage controllers_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage setup_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage bluetooth_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage profiles_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage lights_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage service_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage diagnostics_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage audio_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage donate_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage about_task(const Section &, const ServiceSnapshot::View &, const TaskRead &);
TaskPage build_task_page(std::size_t page, const Section &, const ServiceSnapshot::View &, const TaskRead &);

} // namespace ghostcontrol

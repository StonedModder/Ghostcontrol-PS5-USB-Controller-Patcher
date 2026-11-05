/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task_screen.hpp"
#include <algorithm>

namespace ghostcontrol {
TaskPage build_task_page(std::size_t page, const Section &catalog,
                         const ServiceSnapshot::View &view, const TaskRead &read) {
    using Builder = TaskPage (*)(const Section &, const ServiceSnapshot::View &, const TaskRead &);
    static const Builder builders[] = {controllers_task, setup_task, bluetooth_task,
        profiles_task, lights_task, service_task, diagnostics_task, audio_task, donate_task,
        about_task};
    TaskPage result = builders[std::min(page, std::size(builders) - 1)](catalog, view, read);
    // Every working catalog action remains reachable, but never as a status
    // tile. The focused tools view supplies its description independently.
    result.more.clear();
    for (std::size_t i = 0; i < catalog.rows.size(); ++i) {
        if (!task_actionable(catalog.rows[i])) continue;
        result.more.push_back({i, catalog.rows[i].label});
    }
    return result;
}
} // namespace ghostcontrol

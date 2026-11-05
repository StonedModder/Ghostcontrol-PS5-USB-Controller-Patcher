/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "service_snapshot.hpp"
namespace ghostcontrol {
// The production modal dispatch boundary. Tests use the same snapshot FIFO.
inline bool queue_overlay_group(ServiceSnapshot &service, const char *command,
                                const std::vector<std::string> &arguments,
                                unsigned timeout_ms = 0)
{
    if (!command || arguments.empty()) return false;
    std::vector<std::pair<std::string, std::string>> group;
    group.reserve(arguments.size());
    for (const auto &argument : arguments) group.emplace_back(command, argument);
    return service.queue_commands(group, timeout_ms);
}
} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console-only factories for the companion bootstrap. Host tests build
 * service_launcher.cpp on its own and inject their own transport and assets,
 * so nothing here is linked into them.
 */
#pragma once

#include "service_launcher.hpp"

namespace ghostcontrol
{

/** Reads the companion out of the packaged assets. */
LaunchAssets platform_launch_assets();

/** Pushes a companion to the console loader on the title's own LAN address. */
LaunchTransport platform_launch_transport();

/** UDP route lookup. Sets the controller client address. No packet is sent. */
bool platform_network_init(std::string *detail = nullptr);

/** Dotted address from the last successful platform_network_init, or empty. */
const std::string &platform_loader_host();

} // namespace ghostcontrol

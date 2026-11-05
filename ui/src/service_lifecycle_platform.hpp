/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "service_lifecycle.hpp"

namespace ghostcontrol
{

/** Owned single-worker adapter over the real argument-bearing C client.
 *  Fresh diagnostic capability gates exact-instance conditional release.
 *  Terminal receipt state is distinct from STATUS capability observations.
 *  Call after platform_network_init so the probe targets the same address
 *  the snapshot worker uses. */
LifecycleProbe platform_lifecycle_probe();

} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gfx/renderer.hpp"
namespace ghostcontrol {
// Own-frame diagnostic hook: inert unless DevLoop creates /app0/devloop-shot.
void capture_task_frame(hui::gfx::Renderer &renderer);
}

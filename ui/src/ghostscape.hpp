/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ghostscape: a drift of spirits behind the workspace.
 *
 * Deliberately texture-free and deterministic: each ghost is a handful of
 * instanced shapes (glow, torso, scalloped skirt, eyes) so the layer costs a
 * few dozen draws a frame and never allocates after construction. Motion is a
 * slow horizontal drift plus a sine bob and sway, and the whole field wraps
 * around the canvas so it never empties.
 */
#pragma once

#include "gfx/draw_list.hpp"

#include <cstdint>
#include <vector>

namespace ghostcontrol
{

class GhostScape
{
  public:
    GhostScape();

    /** Advance the drift. dt in seconds; safe for any frame time. */
    void update(float dt);

    /** Paint the field. Call it right after the page fill, before any panel,
     *  so surfaces occlude the spirits instead of the reverse. */
    void draw(hui::gfx::DrawList &list, float dim = 1.0f) const;

    /** Turn the field off (reduced motion, tests). */
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

  private:
    struct Ghost
    {
        float x = 0.0f, y = 0.0f;   // centre in virtual pixels
        float vx = 0.0f, vy = 0.0f; // drift, px/s
        float size = 60.0f;         // half width
        float alpha = 0.1f;
        float phase = 0.0f;  // bob/sway phase
        float speed = 1.0f;  // phase speed
        float hue = 0.0f;    // 0..1 across the spectral palette
        float depth = 0.5f;  // 0 far .. 1 near
        float blink = 2.0f;  // seconds until the next blink
        float blink_left = 0.0f;
        float sway = 0.0f;   // current horizontal offset from the sway
    };

    float rand01();
    void respawn(Ghost &ghost, bool anywhere);
    void draw_ghost(hui::gfx::DrawList &list, const Ghost &ghost, float dim) const;

    std::vector<Ghost> ghosts_;
    float time_ = 0.0f;
    bool enabled_ = true;
    std::uint32_t rng_ = 0x9e3779b9u;
};

} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ghostscape.hpp"

#include <algorithm>
#include <cmath>

namespace ghostcontrol
{
namespace
{

using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

constexpr float kCanvasW = 1920.0f;
constexpr float kCanvasH = 1080.0f;
constexpr int kFar = 5;
constexpr int kMid = 4;
constexpr int kNear = 2;
constexpr int kGhosts = kFar + kMid + kNear;

/* The field is ink, not neon: every spirit is the primary text colour at a low
 * alpha, so it reads as a pale silhouette on the bench. Exactly one carries the
 * teal indicator, which is how the field says "this product" without a glow. */
Color spectral(float hue, float alpha, bool hero)
{
    if (hero) {
        Color c = Color::rgb(0x5fc8b8);
        c.a = alpha * 1.25f;
        return c;
    }
    /* A whisper of variation so the silhouettes do not look stamped: cooler or
     * slightly warmer paper grey, never a hue. */
    const Color cool = Color::rgb(0xdfe6f2);
    const Color warm = Color::rgb(0xe9e6df);
    Color c = hui::gfx::mix(cool, warm, hue);
    c.a = alpha;
    return c;
}

float depth_alpha(float depth)
{
    return 0.028f + depth * 0.042f;
}

} // namespace

GhostScape::GhostScape()
{
    ghosts_.reserve(kGhosts);
    for (int i = 0; i < kGhosts; ++i) {
        Ghost g;
        const bool near = i >= kFar + kMid;
        const bool mid = !near && i >= kFar;
        g.depth = near ? 0.78f + rand01() * 0.22f : mid ? 0.45f + rand01() * 0.25f
                                                        : 0.05f + rand01() * 0.3f;
        respawn(g, true);
        ghosts_.push_back(g);
    }
}

float GhostScape::rand01()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ & 0xffffffu) / static_cast<float>(0x1000000u);
}

void GhostScape::respawn(Ghost &g, bool anywhere)
{
    g.size = 20.0f + g.depth * g.depth * 48.0f + rand01() * 12.0f;
    g.alpha = depth_alpha(g.depth);
    g.hue = rand01() < 0.06f ? 0.9f + rand01() * 0.1f : rand01() * 0.86f;
    g.speed = 0.18f + rand01() * 0.42f;
    g.phase =rand01() * 6.2832f;
    g.blink = 1.5f + rand01() * 5.5f;
    g.blink_left = 0.0f;
    g.sway = 0.0f;
    const float speed = 3.5f + g.depth * 11.0f;
    g.vx = (rand01() < 0.5f ? -1.0f : 1.0f) * speed * (0.35f + rand01() * 0.65f);
    g.vy = (rand01() - 0.5f) * 4.0f;
    if (anywhere) {
        /* The inventory readout lives on the right; big spirits stay out of it
         * so the label and the list never sit on a silhouette. */
        g.x = g.depth > 0.45f ? 1020.0f + rand01() * 860.0f : rand01() * kCanvasW;
        g.y = 260.0f + rand01() * (kCanvasH - 380.0f);
    } else {
        /* Re-enter from the side it left, at a fresh height. */
        g.x = g.vx > 0.0f ? -g.size * 2.4f : kCanvasW + g.size * 2.4f;
        g.y = 260.0f + rand01() * (kCanvasH - 380.0f);
    }
}

void GhostScape::update(float dt)
{
    if (!enabled_)
        return;
    /* A long frame (a load stall) must not teleport the whole field. */
    dt = std::clamp(dt, 0.0f, 0.1f);
    time_ += dt;
    for (Ghost &g : ghosts_) {
        g.phase += g.speed * dt;
        g.sway = std::sin(g.phase) * (4.0f + 16.0f * g.depth);
        g.x += g.vx * dt;
        g.y += g.vy * dt + std::cos(g.phase * 0.7f) * 6.0f * g.depth * dt;
        if (g.blink_left > 0.0f)
            g.blink_left -= dt;
        else if ((g.blink -= dt) <= 0.0f) {
            g.blink_left = 0.16f;
            g.blink = 1.8f + rand01() * 6.0f;
        }
        const float margin = g.size * 2.6f;
        if (g.x < -margin || g.x > kCanvasW + margin || g.y < -margin ||
            g.y > kCanvasH + margin)
            respawn(g, false);
    }
}

void GhostScape::draw_ghost(DrawList &list, const Ghost &g, float dim) const
{
    const float w = g.size;
    const float h = g.size * 1.22f;
    const float x = g.x + g.sway;
    const float y = g.y + std::sin(g.phase * 0.9f) * (4.0f + 9.0f * g.depth);

    const bool hero = &g == &ghosts_.front();
    Color body = spectral(g.hue, g.alpha * dim, hero);
    const Rect torso{x - w, y - h, w * 2.0f, h * 1.62f};

    /* Flat silhouette: no glow, no aura. The bench stays quiet and the shapes
     * carry the depth. */
    list.rounded_rect(torso, w * 0.92f, body);

    /* Scalloped skirt: three overlapping discs under the torso. */
    const float skirt_r = w * 0.44f;
    const float skirt_y = y + h * 0.5f;
    for (int i = -1; i <= 1; ++i)
        list.circle(x + static_cast<float>(i) * w * 0.6f, skirt_y, skirt_r, body);

    /* Eyes: darker than the body, and they blink by flattening. */
    const float blink = g.blink_left > 0.0f
                            ? std::max(0.08f, std::abs(std::cos(g.blink_left / 0.16f * 1.5708f)))
                            : 1.0f;
    const float eye_w = w * 0.17f;
    const float eye_h = w * 0.23f * blink;
    const Color eye = Color::rgb(0x0b0d14, std::min(1.0f, g.alpha * 6.5f));
    list.rounded_rect({x - w * 0.42f - eye_w * 0.5f, y - h * 0.30f - eye_h * 0.5f,
                       eye_w, eye_h}, eye_w * 0.5f, eye);
    list.rounded_rect({x + w * 0.42f - eye_w * 0.5f, y - h * 0.30f - eye_h * 0.5f,
                       eye_w, eye_h}, eye_w * 0.5f, eye);
    /* A short mouth, only on the nearer spirits that have enough alpha. */
    if (g.depth > 0.55f)
        list.rounded_rect({x - w * 0.13f, y - h * 0.02f, w * 0.26f, w * 0.07f},
                          w * 0.035f,
                          Color::rgb(0x0b0d14, std::min(1.0f, g.alpha * 5.0f)));
}

void GhostScape::draw(DrawList &list, float dim) const
{
    if (!enabled_)
        return;
    /* Three depth passes so far spirits never paint over near ones, whatever
     * order respawns left the vector in. */
    for (const Ghost &g : ghosts_)
        if (g.depth < 0.45f) draw_ghost(list, g, dim);
    for (const Ghost &g : ghosts_)
        if (g.depth >= 0.45f && g.depth < 0.78f) draw_ghost(list, g, dim);
    for (const Ghost &g : ghosts_)
        if (g.depth >= 0.78f) draw_ghost(list, g, dim);
}

} // namespace ghostcontrol

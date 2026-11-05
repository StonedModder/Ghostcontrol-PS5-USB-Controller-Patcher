// Ghostcontrol Toolbox 2.0 - interactive DualSense 3D viewer.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small software-3D module: it owns the flattened controller asset that
// tools/convert-dualsense-model.py produced, orients it with the same gyro
// integration the studio's controller/gyro-orientation.js runs in the browser,
// flat-shades every triangle with the part's own base colour, and glows the
// parts a live controller is touching. It draws no GL of its own: triangles go
// into the UI kit's mesh primitive on the DrawList, in the virtual canvas, so
// the viewer composes with everything else the app paints.
//
// Nothing here talks to a service. The whole live surface is plain parameters
// (buttons mask, stick axes, trigger values, touchpad contact, orientation,
// gyro rates, acceleration, dt), so the host harness can drive scripted poses
// exactly as readily as the real snapshot worker can drive real ones.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hui::gfx
{
class DrawList;
struct Rect;
} // namespace hui::gfx

namespace ghostcontrol
{

// The ScePad button bits the service already speaks (core/src/gc_types.h).
// Repeated here so this module stays header-light; keep in step with that file.
namespace pads
{
constexpr std::uint32_t share = 0x00000001u;  // DualSense Create
constexpr std::uint32_t l3 = 0x00000002u;
constexpr std::uint32_t r3 = 0x00000004u;
constexpr std::uint32_t options = 0x00000008u;
constexpr std::uint32_t up = 0x00000010u;
constexpr std::uint32_t right = 0x00000020u;
constexpr std::uint32_t down = 0x00000040u;
constexpr std::uint32_t left = 0x00000080u;
constexpr std::uint32_t l2 = 0x00000100u;
constexpr std::uint32_t r2 = 0x00000200u;
constexpr std::uint32_t l1 = 0x00000400u;
constexpr std::uint32_t r1 = 0x00000800u;
constexpr std::uint32_t triangle = 0x00001000u;
constexpr std::uint32_t circle = 0x00002000u;
constexpr std::uint32_t cross = 0x00004000u;
constexpr std::uint32_t square = 0x00008000u;
constexpr std::uint32_t ps = 0x00010000u;
constexpr std::uint32_t create = share;
constexpr std::uint32_t touchpad = 0x00100000u;
} // namespace pads

// A direct port of the studio's GyroOrientation: the sensor basis is rotated
// +90 degrees about X into the mesh basis, gyro rates are integration into a
// quaternion, and gravity gently levels the tilt (never the heading) while the
// controller is moving slowly. Faithful to controller/gyro-orientation.js.
struct GyroOrientation
{
    float quat[4] = {1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    bool needs_tilt = true;

    void reset();
    // Marks the tilt stale so the next sample snaps flat instead of easing.
    void realign();
    // pitch/yaw/roll in degrees per second, acceleration in g (a null pointer
    // means "no accelerometer this sample"). Invalid rates or dt leave the
    // quaternion untouched. dt is seconds.
    void update(float pitch, float yaw, float roll, const float *acceleration, float dt);
};

// Everything the viewer needs about one controller this frame. All plain
// values: the same struct the harness scripts and the service worker fills.
struct Controller3dInput
{
    std::uint32_t buttons = 0; // pads:: bits, as the service reports them
    float stick_left_x = 0.0f; // -1..1
    float stick_left_y = 0.0f;
    float stick_right_x = 0.0f;
    float stick_right_y = 0.0f;
    float trigger_l2 = 0.0f; // 0..1
    float trigger_r2 = 0.0f;
    bool touchpad_touched = false;
    float touchpad_x = 0.0f; // 0..1 across the pad
    float touchpad_y = 0.0f;

    // When integrate_motion is set the viewer advances its own GyroOrientation
    // from gyro/accel/dt and draws the result; otherwise it draws quat as-is
    // (the resting pose, or an orientation the caller integrated itself).
    bool integrate_motion = false;
    float quat[4] = {1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    float gyro_pitch = 0.0f; // degrees per second
    float gyro_yaw = 0.0f;
    float gyro_roll = 0.0f;
    float accel[3] = {0.0f, 0.0f, 0.0f}; // g
    float dt = 1.0f / 60.0f;

    // Motion only levels the tilt while the controller is still; realign asks
    // for a fresh full snap on the next sample (the studio's "resume").
    bool realign_motion = false;
};

// Loads the .gc3d asset and renders it. One instance per on-screen controller.
class Controller3dView
{
  public:
    // Reads a .gc3d file (see tools/convert-dualsense-model.py for the format).
    // Returns false and leaves the view empty on any malformed input.
    bool load(const std::string &path);
    bool load_from_memory(const std::string &bytes);

    bool loaded() const { return !triangles_.empty(); }
    std::uint32_t triangle_count() const
    {
        return static_cast<std::uint32_t>(triangles_.size());
    }
    const std::vector<std::string> &part_names() const { return part_names_; }
    // Index of a part by name, or -1 when the asset has no such part.
    int part_index(const std::string &name) const;
    // Half-open triangle range a part covers in the asset, or {0,0} if unknown.
    std::uint32_t part_first(std::uint32_t part) const;
    std::uint32_t part_count(std::uint32_t part) const;
    // The eased highlight amount (0..1) for a part after the last draw.
    float part_glow(std::uint32_t part) const;
    // The orientation the last draw used, w, x, y, z.
    const float *orientation() const { return gyro_.quat; }
    GyroOrientation &gyro() { return gyro_; }

    // Emits the model into the DrawList inside rect (virtual pixels). glows
    // ease toward this frame's targets by dt, so calling every frame animates.
    void draw(hui::gfx::DrawList &list, const hui::gfx::Rect &rect,
              const Controller3dInput &input);

  private:
    struct Triangle
    {
        float p[3][3]; // three corners, model space
        float n[3];    // flat face normal, model space
        std::uint8_t material = 0;
        std::uint16_t part = 0;
    };

    void update_glow(const Controller3dInput &input);
    void update_orientation(const Controller3dInput &input);

    std::vector<Triangle> triangles_;
    std::vector<std::string> part_names_;
    std::vector<std::uint32_t> part_first_;
    std::vector<std::uint32_t> part_count_;
    std::vector<float> part_color_; // sRGB base colour per part, for fallbacks
    std::vector<float> material_;   // sRGB base colour per material
    std::vector<float> glow_;
    float center_[3] = {0.0f, 0.0f, 0.0f};
    float radius_ = 1.0f;
    GyroOrientation gyro_;
    std::vector<int> order_; // reused every frame so draw() does not allocate
};

} // namespace ghostcontrol

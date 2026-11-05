// Ghostcontrol Toolbox 2.0 - interactive DualSense 3D viewer.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Implementation notes: the gyro block is a line-for-line port of
// controller/gyro-orientation.js (same basis rotation, same magnitude gate,
// same one-shot tilt snap), the loader reads the .gc3d container the offline
// converter writes, and the drawing pass is a software rasteriser of sorts --
// it transforms, flat-shades and depth-sorts triangles on the CPU and hands
// them to the kit's mesh primitive, which owns the only GL involved.

#include "controller3d.hpp"

#include "gfx/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ghostcontrol
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
// The studio's highlight colour, verbatim.
constexpr float kAmber[3] = {255.0f / 255.0f, 191.0f / 255.0f, 71.0f / 255.0f};

void quat_identity(float q[4])
{
    q[0] = 1.0f;
    q[1] = 0.0f;
    q[2] = 0.0f;
    q[3] = 0.0f;
}

// out = a * b, Hamilton product, w-first (w, x, y, z).
void quat_mul(const float a[4], const float b[4], float out[4])
{
    const float w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    const float x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    const float y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    const float z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    out[0] = w;
    out[1] = x;
    out[2] = y;
    out[3] = z;
}

void quat_normalize(float q[4])
{
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length <= 0.0f)
    {
        quat_identity(q);
        return;
    }
    const float inverse = 1.0f / length;
    q[0] *= inverse;
    q[1] *= inverse;
    q[2] *= inverse;
    q[3] *= inverse;
}

// three.js Quaternion.setFromAxisAngle; axis must be a unit vector.
void quat_from_axis_angle(const float axis[3], float angle, float out[4])
{
    const float half = angle * 0.5f;
    const float s = std::sin(half);
    out[0] = std::cos(half);
    out[1] = axis[0] * s;
    out[2] = axis[1] * s;
    out[3] = axis[2] * s;
}

// three.js Quaternion.setFromUnitVectors.
void quat_from_unit_vectors(const float from[3], const float to[3], float out[4])
{
    float r = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + 1.0f;
    float x;
    float y;
    float z;
    if (r < 1.0e-8f)
    {
        r = 0.0f;
        if (std::fabs(from[0]) > std::fabs(from[2]))
        {
            x = -from[1];
            y = from[0];
            z = 0.0f;
        }
        else
        {
            x = 0.0f;
            y = -from[2];
            z = from[1];
        }
    }
    else
    {
        x = from[1] * to[2] - from[2] * to[1];
        y = from[2] * to[0] - from[0] * to[2];
        z = from[0] * to[1] - from[1] * to[0];
    }
    out[0] = r;
    out[1] = x;
    out[2] = y;
    out[3] = z;
    quat_normalize(out);
}

// three.js Quaternion.slerp into out; a is the "this" side.
void quat_slerp(const float a[4], const float b[4], float t, float out[4])
{
    if (t <= 0.0f)
    {
        std::memcpy(out, a, sizeof(float) * 4);
        return;
    }
    if (t >= 1.0f)
    {
        std::memcpy(out, b, sizeof(float) * 4);
        return;
    }
    float bx = b[1];
    float by = b[2];
    float bz = b[3];
    float bw = b[0];
    float cos_half = a[0] * bw + a[1] * bx + a[2] * by + a[3] * bz;
    if (cos_half < 0.0f)
    {
        cos_half = -cos_half;
        bx = -bx;
        by = -by;
        bz = -bz;
        bw = -bw;
    }
    if (cos_half >= 1.0f)
    {
        std::memcpy(out, a, sizeof(float) * 4);
        return;
    }
    const float sqr_sin_half = 1.0f - cos_half * cos_half;
    if (sqr_sin_half <= 1.0e-8f)
    {
        const float s = 1.0f - t;
        out[0] = s * a[0] + t * bw;
        out[1] = s * a[1] + t * bx;
        out[2] = s * a[2] + t * by;
        out[3] = s * a[3] + t * bz;
        quat_normalize(out);
        return;
    }
    const float sin_half = std::sqrt(sqr_sin_half);
    const float half = std::atan2(sin_half, cos_half);
    const float ratio_a = std::sin((1.0f - t) * half) / sin_half;
    const float ratio_b = std::sin(t * half) / sin_half;
    out[0] = a[0] * ratio_a + bw * ratio_b;
    out[1] = a[1] * ratio_a + bx * ratio_b;
    out[2] = a[2] * ratio_a + by * ratio_b;
    out[3] = a[3] * ratio_a + bz * ratio_b;
}

// three.js Vector3.applyQuaternion, writing back through the same pointer.
void quat_apply(const float q[4], const float v[3], float out[3])
{
    const float x = v[0];
    const float y = v[1];
    const float z = v[2];
    const float qx = q[1];
    const float qy = q[2];
    const float qz = q[3];
    const float qw = q[0];
    const float ix = qw * x + qy * z - qz * y;
    const float iy = qw * y + qz * x - qx * z;
    const float iz = qw * z + qx * y - qy * x;
    const float iw = -qx * x - qy * y - qz * z;
    out[0] = ix * qw + iw * -qx + iy * -qz - iz * -qy;
    out[1] = iy * qw + iw * -qy + iz * -qx - ix * -qz;
    out[2] = iz * qw + iw * -qz + ix * -qy - iy * -qx;
}

bool finite3(const float *v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

std::uint16_t read_u16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// A single stick's worth of highlight: the button press, or how far the axis
// is pushed. 0.9 of full throw counts as full so the rim lights before the
// stick reaches the gate.
float stick_press(float x, float y)
{
    const float magnitude = std::sqrt(x * x + y * y) / 0.9f;
    return clamp01(magnitude);
}

} // namespace

void GyroOrientation::reset()
{
    quat_identity(quat);
    realign();
}

void GyroOrientation::realign()
{
    needs_tilt = true;
}

void GyroOrientation::update(float pitch, float yaw, float roll, const float *acceleration,
                             float dt)
{
    if (!std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll) ||
        !std::isfinite(dt) || dt <= 0.0f || dt > 0.1f)
        return;

    // The sensor basis is rotated +90 degrees about X into the mesh basis:
    // (x, y, z) -> (x, -z, y). The pitch/roll swap and the sign on roll are
    // that remap showing through the rate vector.
    const float axis[3] = {pitch, -roll, yaw};
    const float speed = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (speed > 0.0f)
    {
        const float unit[3] = {axis[0] / speed, axis[1] / speed, axis[2] / speed};
        float delta[4];
        quat_from_axis_angle(unit, speed * kDegToRad * dt, delta);
        quat_mul(quat, delta, quat);
        quat_normalize(quat);
    }

    if (acceleration != nullptr && finite3(acceleration))
    {
        const float gravity[3] = {acceleration[0], -acceleration[2], acceleration[1]};
        const float magnitude =
            std::sqrt(gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]);
        // Gravity establishes tilt, never compass heading. Ignore strong linear
        // acceleration; gently correct tilt only during slow/stationary motion.
        if (magnitude > 0.9f && magnitude < 1.1f && (needs_tilt || speed < 20.0f))
        {
            float level[3] = {gravity[0] / magnitude, gravity[1] / magnitude, gravity[2] / magnitude};
            quat_apply(quat, level, level);
            const float up[3] = {0.0f, 1.0f, 0.0f};
            float delta[4];
            quat_from_unit_vectors(level, up, delta);
            float correction[4];
            quat_identity(correction);
            const float t = needs_tilt ? 1.0f : (1.0f - std::exp(-1.5f * dt));
            quat_slerp(correction, delta, t, correction);
            quat_mul(correction, quat, quat); // premultiply
            quat_normalize(quat);
            needs_tilt = false;
        }
    }
}

// ---------------------------------------------------------------------------
// asset
// ---------------------------------------------------------------------------

bool Controller3dView::load(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::string bytes;
    char buffer[65536];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        bytes.append(buffer, got);
    std::fclose(file);
    return load_from_memory(bytes);
}

bool Controller3dView::load_from_memory(const std::string &bytes)
{
    const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
    const std::size_t size = bytes.size();
    if (size < 40 || std::memcmp(data, "GC3D", 4) != 0 || read_u16(data + 4) != 1)
        return false;

    const std::uint16_t part_total = read_u16(data + 6);
    const std::uint16_t material_total = read_u16(data + 8);
    const std::uint32_t triangle_total = read_u32(data + 12);
    const std::uint8_t *cursor = data + 40;

    std::vector<float> material(static_cast<std::size_t>(material_total) * 3);
    for (std::size_t i = 0; i < material.size(); ++i)
        material[i] = cursor[i] / 255.0f;
    cursor += material.size();

    // bounds check every read against the end of the buffer
    const auto *end = data + size;
    const auto fits = [end](const std::uint8_t *p, std::size_t n) {
        return p + n <= end;
    };

    std::vector<float> lo(3);
    std::vector<float> hi(3);
    std::memcpy(lo.data(), data + 16, sizeof(float) * 3);
    std::memcpy(hi.data(), data + 28, sizeof(float) * 3);

    std::vector<std::string> names;
    std::vector<std::uint32_t> first;
    std::vector<std::uint32_t> count;
    std::vector<float> part_color(static_cast<std::size_t>(part_total) * 3, 0.6f);
    names.reserve(part_total);
    for (std::uint16_t i = 0; i < part_total; ++i)
    {
        if (!fits(cursor, 1))
            return false;
        const std::uint8_t name_len = *cursor++;
        if (!fits(cursor, name_len + 3 + 8))
            return false;
        names.emplace_back(reinterpret_cast<const char *>(cursor), name_len);
        cursor += name_len;
        for (int c = 0; c < 3; ++c)
            part_color[static_cast<std::size_t>(i) * 3 + c] = cursor[c] / 255.0f;
        cursor += 3;
        first.push_back(read_u32(cursor));
        count.push_back(read_u32(cursor + 4));
        cursor += 8;
    }

    constexpr std::size_t kTriSize = 24; // u16 + u8 + 3*i8 + 9*u16
    if (!fits(cursor, static_cast<std::size_t>(triangle_total) * kTriSize))
        return false;

    std::vector<Triangle> triangles(triangle_total);
    const float span[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    for (std::uint32_t t = 0; t < triangle_total; ++t)
    {
        const std::uint8_t *record = cursor + static_cast<std::size_t>(t) * kTriSize;
        Triangle &tri = triangles[t];
        tri.part = read_u16(record);
        tri.material = record[2];
        tri.n[0] = static_cast<float>(static_cast<int8_t>(record[3])) / 127.0f;
        tri.n[1] = static_cast<float>(static_cast<int8_t>(record[4])) / 127.0f;
        tri.n[2] = static_cast<float>(static_cast<int8_t>(record[5])) / 127.0f;
        for (int corner = 0; corner < 3; ++corner)
        {
            const std::uint8_t *q = record + 6 + corner * 6;
            for (int axis = 0; axis < 3; ++axis)
            {
                const std::uint16_t quant = read_u16(q + axis * 2);
                tri.p[corner][axis] = lo[axis] + (quant / 65535.0f) * span[axis];
            }
        }
    }

    // fit the model into the destination rect: centre it, and use the furthest
    // vertex as the radius so a rotation can never push it outside.
    float low[3] = {triangles[0].p[0][0], triangles[0].p[0][1], triangles[0].p[0][2]};
    float high[3] = {low[0], low[1], low[2]};
    for (const Triangle &tri : triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis)
            {
                low[axis] = std::min(low[axis], tri.p[corner][axis]);
                high[axis] = std::max(high[axis], tri.p[corner][axis]);
            }
    float center[3];
    for (int axis = 0; axis < 3; ++axis)
        center[axis] = (low[axis] + high[axis]) * 0.5f;
    float radius = 0.0f;
    for (const Triangle &tri : triangles)
        for (int corner = 0; corner < 3; ++corner)
        {
            const float dx = tri.p[corner][0] - center[0];
            const float dy = tri.p[corner][1] - center[1];
            const float dz = tri.p[corner][2] - center[2];
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
        }

    triangles_ = std::move(triangles);
    part_names_ = std::move(names);
    part_first_ = std::move(first);
    part_count_ = std::move(count);
    part_color_ = std::move(part_color);
    material_ = std::move(material);
    glow_.assign(part_names_.size(), 0.0f);
    center_[0] = center[0];
    center_[1] = center[1];
    center_[2] = center[2];
    radius_ = radius > 0.0f ? radius : 1.0f;
    gyro_.reset();
    return true;
}

int Controller3dView::part_index(const std::string &name) const
{
    for (std::size_t i = 0; i < part_names_.size(); ++i)
        if (part_names_[i] == name)
            return static_cast<int>(i);
    return -1;
}

std::uint32_t Controller3dView::part_first(std::uint32_t part) const
{
    return part < part_first_.size() ? part_first_[part] : 0u;
}

std::uint32_t Controller3dView::part_count(std::uint32_t part) const
{
    return part < part_count_.size() ? part_count_[part] : 0u;
}

float Controller3dView::part_glow(std::uint32_t part) const
{
    return part < glow_.size() ? glow_[part] : 0.0f;
}

// ---------------------------------------------------------------------------
// per-frame update
// ---------------------------------------------------------------------------

void Controller3dView::update_orientation(const Controller3dInput &input)
{
    if (input.realign_motion)
        gyro_.realign();
    if (input.integrate_motion)
        gyro_.update(input.gyro_pitch, input.gyro_yaw, input.gyro_roll, input.accel, input.dt);
}

void Controller3dView::update_glow(const Controller3dInput &input)
{
    // Fast enough to read as "this is pressed" without snapping; the studio's
    // highlight is a smooth blend, not a toggle.
    const float blend = 1.0f - std::exp(-16.0f * std::max(input.dt, 0.0f));
    for (std::size_t i = 0; i < part_names_.size(); ++i)
    {
        const std::string &name = part_names_[i];
        float target = 0.0f;
        const std::uint32_t b = input.buttons;
        if (name == "btn_cross")
            target = (b & pads::cross) != 0 ? 1.0f : 0.0f;
        else if (name == "btn_circle")
            target = (b & pads::circle) != 0 ? 1.0f : 0.0f;
        else if (name == "btn_triangle")
            target = (b & pads::triangle) != 0 ? 1.0f : 0.0f;
        else if (name == "btn_square")
            target = (b & pads::square) != 0 ? 1.0f : 0.0f;
        else if (name == "btn_create")
            target = (b & pads::create) != 0 ? 1.0f : 0.0f;
        else if (name == "btn_options")
            target = (b & pads::options) != 0 ? 1.0f : 0.0f;
        else if (name == "btn_ps")
            target = (b & pads::ps) != 0 ? 1.0f : 0.0f;
        else if (name == "dpad_up")
            target = (b & pads::up) != 0 ? 1.0f : 0.0f;
        else if (name == "dpad_down")
            target = (b & pads::down) != 0 ? 1.0f : 0.0f;
        else if (name == "dpad_left")
            target = (b & pads::left) != 0 ? 1.0f : 0.0f;
        else if (name == "dpad_right")
            target = (b & pads::right) != 0 ? 1.0f : 0.0f;
        else if (name == "bumper_l1")
            target = (b & pads::l1) != 0 ? 1.0f : 0.0f;
        else if (name == "bumper_r1")
            target = (b & pads::r1) != 0 ? 1.0f : 0.0f;
        else if (name == "trigger_l2")
            target = std::max((b & pads::l2) != 0 ? 1.0f : 0.0f, clamp01(input.trigger_l2));
        else if (name == "trigger_r2")
            target = std::max((b & pads::r2) != 0 ? 1.0f : 0.0f, clamp01(input.trigger_r2));
        else if (name == "stick_left")
            target = std::max((b & pads::l3) != 0 ? 1.0f : 0.0f,
                              stick_press(input.stick_left_x, input.stick_left_y));
        else if (name == "stick_right")
            target = std::max((b & pads::r3) != 0 ? 1.0f : 0.0f,
                              stick_press(input.stick_right_x, input.stick_right_y));
        else if (name == "touchpad" || name == "touchpad_frame" || name == "touchpad_light")
            target = (input.touchpad_touched || (b & pads::touchpad) != 0) ? 1.0f : 0.0f;
        glow_[i] += (target - glow_[i]) * blend;
    }
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void Controller3dView::draw(hui::gfx::DrawList &list, const hui::gfx::Rect &rect,
                            const Controller3dInput &input)
{
    if (triangles_.empty() || rect.w <= 0.0f || rect.h <= 0.0f)
        return;

    update_orientation(input);
    update_glow(input);

    // A gentle fixed camera tilt. The sensor orientation is a pure rotation, so
    // seen straight on the controller would read as a flat outline; a few
    // degrees off square lets the shells and the stick wells catch the light.
    const float ax = -16.0f * kDegToRad;
    const float ay = 10.0f * kDegToRad;
    const float cx1 = std::cos(ax);
    const float sx1 = std::sin(ax);
    const float cy1 = std::cos(ay);
    const float sy1 = std::sin(ay);

    const float scale = std::min(rect.w, rect.h) * 0.5f * 0.95f;
    const float origin_x = rect.cx();
    const float origin_y = rect.cy();

    const std::size_t total = triangles_.size();
    std::vector<float> view(static_cast<std::size_t>(total) * 3 * 3);
    std::vector<float> screen(static_cast<std::size_t>(total) * 3 * 2);
    std::vector<float> depth(total);

    const float inverse_radius = 1.0f / radius_;
    for (std::size_t t = 0; t < total; ++t)
    {
        const Triangle &tri = triangles_[t];
        for (int corner = 0; corner < 3; ++corner)
        {
            float u[3];
            for (int axis = 0; axis < 3; ++axis)
                u[axis] = (tri.p[corner][axis] - center_[axis]) * inverse_radius;
            quat_apply(gyro_.quat, u, u); // physical orientation
            // camera rotation: Rx(ax) * Ry(ay)
            const float rx = cy1 * u[0] + sy1 * u[2];
            const float ry = sx1 * sy1 * u[0] + cx1 * u[1] - sx1 * cy1 * u[2];
            const float rz = -cx1 * sy1 * u[0] + sx1 * u[1] + cx1 * cy1 * u[2];
            const std::size_t base = (t * 3 + static_cast<std::size_t>(corner)) * 3;
            view[base] = rx;
            view[base + 1] = ry;
            view[base + 2] = rz;
            const std::size_t s = (t * 3 + static_cast<std::size_t>(corner)) * 2;
            screen[s] = origin_x + rx * scale;
            screen[s + 1] = origin_y - ry * scale;
        }
        depth[t] = (view[t * 9 + 2] + view[t * 9 + 5] + view[t * 9 + 8]) / 3.0f;
    }

    // No depth buffer exists on this path, so paint far triangles first.
    order_.resize(total);
    for (std::size_t t = 0; t < total; ++t)
        order_[t] = static_cast<int>(t);
    std::sort(order_.begin(), order_.end(), [&depth](int a, int b) {
        return depth[static_cast<std::size_t>(a)] < depth[static_cast<std::size_t>(b)];
    });

    const float light[3] = {0.35f, 0.55f, 0.75f};
    for (std::size_t slot = 0; slot < total; ++slot)
    {
        const std::size_t t = static_cast<std::size_t>(order_[slot]);
        const float *a = &view[t * 9];
        const float *b = &view[t * 9 + 3];
        const float *c = &view[t * 9 + 6];
        float normal[3] = {(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
                           (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                           (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])};
        const float length =
            std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (length <= 1.0e-12f)
            continue;
        normal[0] /= length;
        normal[1] /= length;
        normal[2] /= length;
        // Two-sided (there is no culling here), so the light hits whichever
        // face is showing.
        const float lambert =
            std::fabs(normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2]);
        const float shade = 0.34f + 0.66f * lambert;

        const Triangle &tri = triangles_[t];
        const std::size_t m = static_cast<std::size_t>(tri.material) * 3;
        float r = material_[m] * shade;
        float g = material_[m + 1] * shade;
        float bl = material_[m + 2] * shade;
        const float glow = std::min(1.0f, glow_[tri.part] * 0.9f);
        r += (kAmber[0] - r) * glow;
        g += (kAmber[1] - g) * glow;
        bl += (kAmber[2] - bl) * glow;

        const float xy[6] = {screen[t * 6],     screen[t * 6 + 1], screen[t * 6 + 2],
                             screen[t * 6 + 3], screen[t * 6 + 4], screen[t * 6 + 5]};
        list.polygon(xy, 3, hui::gfx::Color{r, g, bl, 1.0f});
    }
}

} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One raw RGBA plate, the way the toolbox ships every picture: <name>.rgba
 * holds straight, top-down RGBA and <name>.txt holds "<width> <height>". The
 * pair is checked before anything is uploaded, so a truncated or resized file
 * is refused instead of being stretched across a card. Shared by the app and
 * the host snapshot harness so a screenshot and the console show the same
 * pixels.
 */
#pragma once

#include "toolbox_ui.hpp"

#include "core/save_file.hpp"
#include "gfx/renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ghostcontrol
{

/** Uploads <base>/<name> and returns the texture with its pixel size. A
 *  missing, malformed or wrongly sized pair returns an absent QrCode, which
 *  the cards report as "unavailable in this build". */
inline QrCode load_qr_asset(hui::gfx::Renderer &renderer, const std::string &base,
                            const char *name)
{
    QrCode code;
    std::string meta;
    std::string pixels;
    const std::string path = base + "/" + name;
    if (!hui::save::read_file(path + ".txt", &meta) ||
        !hui::save::read_file(path + ".rgba", &pixels) ||
        std::sscanf(meta.c_str(), "%d %d", &code.width, &code.height) != 2 ||
        code.width <= 0 || code.height <= 0 ||
        pixels.size() != static_cast<std::size_t>(code.width) *
                static_cast<std::size_t>(code.height) * 4)
        return QrCode{};
    code.texture = renderer.batch().create_texture(
        code.width, code.height,
        reinterpret_cast<const std::uint8_t *>(pixels.data()));
    if (code.texture == 0) code = QrCode{};
    return code;
}

} // namespace ghostcontrol

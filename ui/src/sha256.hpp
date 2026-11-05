/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * FIPS 180-4 SHA-256, used to check the packaged companion against its
 * manifest before it is handed to the loader (APP-SVC-04). Written for
 * Ghostcontrol from the published standard; no library dependency.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ghostcontrol
{

class Sha256
{
  public:
    Sha256();
    void update(const void *data, std::size_t length);
    std::array<std::uint8_t, 32> finish();

    /** Lowercase hex digest of `data`. */
    static std::string hex(std::string_view data);

  private:
    void block(const std::uint8_t *chunk);
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

} // namespace ghostcontrol

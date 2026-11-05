/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Donate screen: where the work is funded and where the community lives,
 * each as a scannable code. The row below keeps every string in full, so the
 * panel still works when a camera does not.
 */
#include "screens.hpp"

namespace ghostcontrol
{

static constexpr const char *kDiscord = "https://discord.com/invite/kcgQPxhde";
static constexpr const char *kBtc = "bc1qa9zfgnccajsw8vg7k287qz5a7apf8pefj5jjx5";
static constexpr const char *kPatreon = "patreon.com/c/u4772250";

Section build_screen_donate()
{
    return {"Donate", "Keep the toolbox funded",
            {
                {"Support and community", "",
                 "Every code on this page is also written out in full. Select this "
                 "panel to read or type an address when a scan is not possible.",
                 RowState::action,
                 {std::string("Discord invite: ") + kDiscord,
                  std::string("Bitcoin address: ") + kBtc,
                  std::string("Patreon page: ") + kPatreon}},
            }};
}

} // namespace ghostcontrol


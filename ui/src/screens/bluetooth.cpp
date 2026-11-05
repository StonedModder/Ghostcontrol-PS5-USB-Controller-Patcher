/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bluetooth is one task. Pair is the action. Status is a reading: a stopped
 * radio shows as Off, and Pair stays available either way.
 */
#include "screens.hpp"

namespace ghostcontrol
{

Section build_screen_bluetooth()
{
    return {"Bluetooth", "Hold Create and PS until the light bar blinks. Stay near the console.",
            {
                {.label="Pair", .value="", .note="",
                 .state=RowState::action, .command="BLUETOOTH_PAIR"},
                {.label="Status", .value="-", .note="",
                 .state=RowState::live,
                 .live=LiveSource::bluetooth_state, .read_only=true},
                {.label="Forget", .value="", .note="",
                 .state=RowState::action,
                 .overlay=OverlayKind::picker, .overlay_command="BLUETOOTH_FORGET",
                 .overlay_arguments={"{address}"}, .picker_source=PickerSource::bluetooth_paired},
                {.label="Try again", .value="", .note="",
                 .state=RowState::action, .command="BLUETOOTH_RETRY"},
                {.label="Turn off", .value="", .note="",
                 .state=RowState::action, .command="BLUETOOTH_STOP"},
                {.label="Paired", .value="-", .note="",
                 .state=RowState::live, .live=LiveSource::paired_count, .read_only=true},
                {.label="Connected", .value="-", .note="",
                 .state=RowState::live, .live=LiveSource::connected_count, .read_only=true},
            }};
}

} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "screens.hpp"
namespace ghostcontrol {
Section build_screen_controllers() {
    return {"Controllers", "What's connected right now", {
        {.label="Connected controller", .value="-", .note="",
         .state=RowState::live,
         .detail={"This app currently uses one controller assigned to its initial user.", "Saved controllers and virtual slots are listed separately."},
         .live=LiveSource::native_pad_available, .read_only=true},
        {.label="Saved controllers", .value="-", .note="",
         .state=RowState::live,
         .detail={"Saved rules describe controller settings, not current connections.", "Choosing a virtual controller type saves a preference; game output is unavailable."}, .live=LiveSource::controller_count},
        {.label="Virtual controller type", .value="Choose",
         .note="",
         .state=RowState::action, .overlay=OverlayKind::picker,
         .overlay_command="CONTROLLER_OUTPUT_SET", .overlay_arguments={"{parent}|{address}"},
         .picker_source=PickerSource::controller_rules, .picker_command="CONTROLLERS",
         .picker_key="controllers", .overlay_next=OverlayKind::picker},
        {.label="Virtual slot 1", .value="-", .note="",
         .state=RowState::live, .live=LiveSource::slot1},
        {.label="Virtual slot 2", .value="-", .note="",
         .state=RowState::live, .live=LiveSource::slot2},
        {.label="Virtual slot 3", .value="-", .note="",
         .state=RowState::live, .live=LiveSource::slot3},
        {.label="Virtual slot 4", .value="-", .note="",
         .state=RowState::live, .live=LiveSource::slot4},
        {.label="Detected devices", .value="-", .note="",
         .state=RowState::live, .live=LiveSource::devices_summary},
        {.label="Refresh devices", .value="", .note="",
         .state=RowState::action, .command="DEVICES", .command_timeout_ms=15000},
        {.label="Controller help", .value="", .note="",
         .state=RowState::action,
         .detail={"Use the console controller assignment screen to choose a user.", "Virtual slots are separate from the controller this app uses.", "Virtual controller reset and game bridge controls are unavailable here."}},
    }};
}
} // namespace ghostcontrol

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "screens.hpp"
namespace ghostcontrol {
Section build_screen_lightfx() {
    return {"Lights", "Lightbar and rumble on the controller in your hands", {
        {.label="Lightbar enabled", .value="-", .note="Turn lightbar effects on or off.",
         .state=RowState::live, .live=LiveSource::runtime_fx_enabled,
         .command="RUNTIME_SET", .command_argument=LiveSource::rt_toggle_fx},
        {.label="Lightbar effect", .value="-", .note="Choose an effect, then turn Lightbar enabled on.",
         .state=RowState::live, .detail={"Solid", "Breathe", "Rainbow", "Police", "Strobe"},
         .live=LiveSource::fx_summary, .overlay=OverlayKind::picker,
         .overlay_command="RUNTIME_SET", .overlay_arguments={"dualsense_fx_effect={index}"}},
        {.label="Lightbar colour", .value="-", .note="Choose the base colour for the lightbar.",
         .state=RowState::live, .live=LiveSource::fx_color, .overlay=OverlayKind::color,
         .overlay_command="RUNTIME_SET",
         .overlay_arguments={"dualsense_fx_r={r}", "dualsense_fx_g={g}", "dualsense_fx_b={b}"}},
        {.label="Effect period", .value="-", .note="Set the cycle length; Solid does not use this setting.",
         .state=RowState::live, .live=LiveSource::fx_period_ms, .overlay=OverlayKind::range,
         .overlay_min=100, .overlay_max=60000, .overlay_step=100, .overlay_value=1000,
         .overlay_unit=" ms", .overlay_command="RUNTIME_SET", .overlay_arguments={"dualsense_fx_period_ms={value}"}},
        {.label="Test rumble", .value="", .note="Choose a strength for a short test, even with the lightbar off.",
         .state=RowState::action, .overlay=OverlayKind::range,
         .overlay_min=0, .overlay_max=255, .overlay_step=1, .overlay_value=64,
         .overlay_command="DUALSENSE_RUMBLE", .overlay_arguments={"{value}|{value}|400|100|1"}},
        {.label="Stop rumble", .value="", .note="Stop the rumble test without changing the lightbar.",
         .state=RowState::action, .command="DUALSENSE_RUMBLE_STOP"},
        {.label="Lightbar status", .value="-", .note="Connection and latest lightbar result.",
         .state=RowState::live, .live=LiveSource::native_light_result, .read_only=true},
        {.label="Rumble status", .value="-", .note="Latest rumble test result.",
         .state=RowState::live, .live=LiveSource::native_rumble_result, .read_only=true},
        {.label="Motion", .value="-", .note="Motion readings are not available in this app.",
         .state=RowState::live, .live=LiveSource::motion_state, .read_only=true},
        {.label="Lights and rumble help", .value="", .note="Use a connected controller while this app is open.",
         .state=RowState::action,
         .detail={"Turn Lightbar enabled on, then choose an effect or colour.", "Test rumble works separately from the lightbar setting.", "Check the status rows and your controller to confirm the result.", "Rumble ends when this app closes; background effects are not guaranteed.", "Motion readings are unavailable."}},
    }};
}
} // namespace ghostcontrol

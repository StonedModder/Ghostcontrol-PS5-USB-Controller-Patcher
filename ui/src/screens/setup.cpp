/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Setup is one task: choose a controller, then start, skip, save, or cancel.
 * One button and one stick use the same non-guided arm. Their detail lines are
 * the service target names, not help text.
 */
#include "screens.hpp"

namespace ghostcontrol
{

Section build_screen_setup()
{
    return {"Setup", "Choose a controller, then follow the button prompts.",
            {
                {.label="Choose controller", .value="",
                 .note="Pick a plugged controller. If it already has a profile, this edits that profile.",
                 .state=RowState::action,
                 .overlay=OverlayKind::picker, .overlay_command="SETUP_BEGIN",
                 .overlay_arguments={"{address}"}, .picker_source=PickerSource::setup_sources,
                 .picker_command="DEVICES", .picker_key="devices"},
                {.label="Start", .value="",
                 .note="Learn every control in turn, following the prompts on this page.",
                 .state=RowState::action, .command="SETUP_ARM", .argument="guided"},
                {.label="Skip", .value="",
                 .note="Ask the service for the next control without learning this one.",
                 .state=RowState::action, .command="SETUP_SKIP"},
                {.label="Save", .value="",
                 .note="Write the mappings you learned into the controller's profile.",
                 .state=RowState::action, .command="SETUP_SAVE"},
                {.label="Cancel", .value="",
                 .note="Close the draft without saving anything.",
                 .state=RowState::action, .command="SETUP_CANCEL"},
                /* Custom setup: the guided walk without the running order. The
                 * core's non-guided arm (SETUP_ARM <target>) captures exactly
                 * one control, so the row is a picker of the button targets the
                 * service knows; the options ARE the detail lines. Begin a setup
                 * session first (Guided setup selects physical input and opens a profile),
                 * or the service refuses the arm and says so. */
                {"Learn one button", "", "", RowState::action,
                 {"CROSS", "CIRCLE", "TRIANGLE", "SQUARE", "UP", "DOWN", "LEFT", "RIGHT",
                  "L1", "R1", "L2", "R2", "L3", "R3", "OPTIONS", "SHARE", "PS", "TOUCHPAD"},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "SETUP_ARM", {"{label}"},
                 ReaderKind::none, PickerSource::detail, nullptr, nullptr, 0u},
                /* Real-time mapping: the same non-guided arm, for the controls a
                 * pad with no profile most often gets wrong -- the sticks and the
                 * analog triggers. The service learns from original decoded input;
                 * raw bytes remain diagnostics, not an arbitrary report decoder. */
                {"Learn one stick", "", "", RowState::action,
                 {"AXIS_LX", "AXIS_LY", "AXIS_RX", "AXIS_RY", "AXIS_L2", "AXIS_R2"},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "SETUP_ARM", {"{label}"},
                 ReaderKind::none, PickerSource::detail, nullptr, nullptr, 0u},

            }};
}

} // namespace ghostcontrol

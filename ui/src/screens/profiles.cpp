/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Profiles screen: saved mappings, and sharing them.
 *
 * The active profile and the count on disk are live readings, and reload is
 * wired because it re-reads everything without an argument. The actions below
 * are wired through the modal host: a name is typed on the keyboard, a profile
 * is chosen off a picker fed by PROFILES, a binding is captured with the key
 * binder, and a shared file is chosen off a picker fed by PROFILE_FILES. Where
 * a command needs two answers in order (rename, edit) the row names a second
 * modal (Row::overlay_next), so one row posts one command from both results.
 */
#include "screens.hpp"

namespace ghostcontrol
{

Section build_screen_profiles()
{
    Section section{"Profiles", "Saved button layouts",
            {
                {"Default profile", "-", "Saved default; individual controllers may use other profiles.", RowState::live, {},
                 LiveSource::active_profile},
                {"Profiles loaded", "-", "Profiles found on disk.", RowState::live, {},
                 LiveSource::profile_count},
                {"Reload profiles", "", "Load changes made outside this app.", RowState::action, {"Load saved configuration and profile files again.", "Use this after editing files outside the app; no service restart is needed."},
                 LiveSource::none, "RELOAD", nullptr},
                /* Create: type a name, which the service takes as the profile id.
                 * The id rules (letters, numbers, dot, hyphen, underscore; under
                 * 64 characters; not auto) are the service's, so an invalid name
                 * is refused there and reported back rather than guessed at here.
                 * The keyboard is capped at the id limit. */
                {"Create profile", "", "Enter a new profile ID to create default mappings.",
                 RowState::action, {},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::text,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "PROFILE_CREATE", {"{text}"},
                 ReaderKind::none, PickerSource::detail, nullptr, nullptr, 0u,
                 OverlayKind::none, nullptr, nullptr, nullptr, 63u, false},
                /* Edit: pick a profile, then capture a button on its real
                 * bindings. The key binder's rows come from PROFILE_GET of the
                 * chosen profile (a fetch between the two modals), so the row
                 * names the profile with {parent}, the binding's key with
                 * {text}, and the captured button with {action}. */
                {"Edit button mappings", "", "Choose a profile, then capture a replacement button.",
                 RowState::action, {},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "PROFILE_EDIT",
                 {"{parent}|{text}|{action}"},
                 ReaderKind::none, PickerSource::fetch, "PROFILES", "profiles", 0u,
                 OverlayKind::keybinder, "PROFILE_GET", "{label}", nullptr, 0u, false},
                /* Rename: pick a profile, then edit its name. The keyboard is
                 * seeded with the current name ({parent} of the first modal), so
                 * the player tweaks rather than retypes. PROFILE_RENAME decodes
                 * its argument from hex, so the row says so. */
                {"Rename profile", "", "Choose a profile, then enter its new name.",
                 RowState::action, {},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "PROFILE_RENAME", {"{parent}|{text}"},
                 ReaderKind::none, PickerSource::fetch, "PROFILES", "profiles", 0u,
                 OverlayKind::text, nullptr, nullptr, nullptr, 63u, true},
                /* Import: choose a shared file. PROFILE_IMPORT takes the
                 * basename, which is exactly the picker's label, so no argument
                 * massaging is needed. The list is fed by PROFILE_FILES. */
                {"Import profile", "", "Choose a shared file from the imports folder.",
                 RowState::action, {},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "PROFILE_IMPORT", {"{label}"},
                 ReaderKind::none, PickerSource::fetch, "PROFILE_FILES", "imports", 0u},
                /* Export: the service writes a profile to exports/<id>.ini, so
                 * the honest flow is to choose which profile, not a destination
                 * path the console has no picker for. */
                {"Export profile", "", "Choose a profile to copy to the exports folder.",
                 RowState::action, {},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "PROFILE_EXPORT", {"{label}"},
                 ReaderKind::none, PickerSource::fetch, "PROFILES", "profiles", 0u},
            }};
    Row editor{"Edit all profile settings", "", "Choose a profile to change mappings and other settings.", RowState::action};
    editor.overlay = OverlayKind::picker;
    editor.overlay_command = "PROFILE_EDIT"; // capability; form owns one-key submissions
    editor.overlay_arguments = {"{parent}|{text}|{address}"}; // ID, field key, selected value
    editor.picker_source = PickerSource::fetch;
    editor.picker_command = "PROFILES";
    editor.picker_key = "profiles";
    editor.overlay_next = OverlayKind::picker;
    editor.overlay_next_command = "PROFILE_GET";
    editor.overlay_next_argument = "{label}";
    editor.full_profile_editor = true;
    section.rows.push_back(std::move(editor));
    return section;
}

} // namespace ghostcontrol

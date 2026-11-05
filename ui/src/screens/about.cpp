/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The About screen: what this is, and who it is built on.
 *
 * The service version is live so a build can be told apart at a glance. The
 * reader panels carry the written content the earlier pass added, including
 * the Tester list the shell fills from a packaged text file.
 */
#include "screens.hpp"

namespace ghostcontrol
{

Section build_screen_about()
{
    return {"About", "About Ghostcontrol and its contributors",
            {
                {"Ghostcontrol Toolbox", "", "About this app.", RowState::action, {"Ghostcontrol Toolbox 2.0.", "The app build number is not shown here; Service version is listed separately."}},
                {"Service version", "-", "Version reported by the running service.", RowState::live, {},
                 LiveSource::service_version},
                {"About this version", "", "A standalone app, without changes to the console interface.", RowState::action, {"This version runs as a standalone app on the home screen.", "It does not patch the console Home or Settings interface.", "Legacy privileged game bridges are not available in this app."}},
                {"Credits", "", "Projects and people behind Ghostcontrol.", RowState::action, {"Ghostcontrol: controller service, Bluetooth, mappings and toolbox.", "ps5-homebrew-ui by blackbearreloaded: interface rendering, animation and sound.", "OpenProspero: application runtime, linker and package tools.", "ps5-opengl: graphics support.", "PacBrew: the service json-c dependency.", "Fonts: Inter, Montserrat, DejaVu Sans Mono, Press Start 2P and Patrick Hand.", "Font licences are packaged with the app."}},
                {.label="Testers", .value="", .note="Thanks to everyone testing Ghostcontrol.",
                 .state=RowState::action, .reader=ReaderKind::detail},
            }};
}

} // namespace ghostcontrol

# Ghostcontrol — task workspace UI

This supersedes the rejected rail/rows, card deck and full-width settings list.
Changing their geometry or labels is not a replacement UI.

## Architecture

- task_screen.hpp defines dedicated task scenes: instructions, noninteractive
  readings and separate verb-only buttons.
- task_pages_connect/manage/info.cpp build page-specific state-driven models.
- task_workspace.cpp paints the nine scenes. No generic row/card renderer,
  permanent sidebar or tab strip remains.
- toolbox_ui.cpp retains validated asynchronous command dispatch and request-owned
  persistence guards. screens/*.cpp and Row are internal command metadata, not
  visible screen layout. Working command identities remain reachable in Tools.
- overlay.cpp hosts full-screen focused editors. Reports also occupy the screen.
- IPC, lifecycle, native-pad ownership and service code are unchanged. This UI
  replacement does not repair a failing Bluetooth transport.

## Navigation

Open directly on Controllers. D-pad moves only between actions. Cross invokes
the named verb. Circle opens Workspaces, or backs out of an editor/report/tools.
L1/R1 switch workspace directly. Options opens a separate Tools view, with command
names apart from their descriptions/current readings.

Order: Controllers, Setup, Bluetooth, Profiles, Lights, Service, Diagnostics,
Audio, About. About is last. The directory is a destination, not permanent chrome.

## Screens

- Controllers: controller schematic, app-native observation, detected sources
  and saved rules distinguished; refresh/output actions.
- Setup: source selection, guided input prompts, review/save; service-driven
  stepper. Never infer completion or saved output from an ACK.
- Bluetooth: real radio state, full-width reason, measured search countdown,
  DualSense Create+PS and DS4 SHARE+PS instructions; separate Pair/Retry actions.
- Profiles: library and dedicated create/edit actions; existing guarded
  PROFILE_GET/EDIT persistence and conflict checks remain intact.
- Lights: effect studio and controller schematic; saved preference separate from
  observed native light/rumble results.
- Service: endpoint reachability separate from controller-input running state;
  offline Start service and explicit opt-in startup settings.
- Diagnostics: report layout and full-screen log/radio readers.
- Audio: read-only no-capture explanation; no bogus Start/Volume controls.
- About: logo, credits, testers and real service-version information.

## Rules

1920x1080 canvas, 96 px side safe area, readable 24–32 px body text. Black/navy
background, blue focus/actions, mint measured readings. One visible action focus;
no status joins focus traversal. Buttons contain verbs only, never live values
or vague Open/Read labels. Wrap errors and reports within clipped bounds. Keep
service up, offline, pending, accepted and applied distinct. No blocking service
calls in update/draw. DevLoop own-frame capture is inert without a request file.

## Acceptance

Real HOST build/navigation assertions; actual renderer images of all nine pages,
tools, editors, reports, offline/failure/search/setup states; unchanged-backend
regressions; native app compile/link/package. If deployed, require exact-title
launch, real task presses/captures/logs and virtual-pad release before handback.
HOST fixture views establish presentation only, never console/hardware success.

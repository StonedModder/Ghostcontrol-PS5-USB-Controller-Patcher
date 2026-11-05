# Getting Started

## What you need

- A jailbroken PS5 with an ELF loader (port **9021**).
- **GhosttoothAPI.elf** — the controller companion service.
- The **Ghostcontrol Toolbox** homebrew title (ZIP / folder under `/data/homebrew`).
- A supported USB or Bluetooth controller.
- A normal DualSense (or another working pad) to operate the Toolbox while you configure a custom controller.

Ghostcontrol 2.0 is **two pieces**: the companion ELF and the native Toolbox app. It does **not** inject into ShellUI and does **not** require a combined “XMB” payload.

## Install the Toolbox

1. Copy the release title folder (for example `PPSA62838`) to `/data/homebrew/` on the console (FTP, USB, or your usual homebrew installer).
2. Refresh the home screen if needed so **Ghostcontrol Toolbox** appears as a normal title.

## Load the companion

1. Jailbreak and start the ELF loader.
2. Send `GhosttoothAPI.elf` to port **9021**.
3. Wait for the service notification (if enabled).

You can also start or restart the companion later from the Toolbox **Service** page when a packaged copy is present in the app assets.

## Open the Toolbox

1. Launch **Ghostcontrol Toolbox** from the home screen.
2. Beta builds may show a launch notice — choose **Continue**, or **Don’t show again** to save that preference.
3. Open **Service** and confirm the companion is connected / running. Use **Start service** if it is not.

## Connect and assign a controller

1. Plug in the custom USB controller (or pair Bluetooth from the Bluetooth page).
2. Open **Controllers** and refresh if needed — you should see the device.
3. When Ghostcontrol creates a virtual DualSense, the PS5 shows **Who's using this controller?**
4. Press the requested button and pick the account that should own that virtual pad.
5. After assignment, input from the custom controller is streamed through that virtual DualSense.

The account is chosen by the system UI. Ghostcontrol does not ask you to type a user ID.

## Confirm basic operation

On **Controllers** / status rows, check:

- Service is running.
- At least one USB or BT controller is listed.
- The slot is no longer waiting for assignment.

Test D-pad, face buttons, shoulders, triggers, sticks, L3, and R3. If the pad is found but mappings are wrong, run [Guided Controller Setup](Guided-Controller-Setup.md). If it is not found, add or fix a rule — see [Toolbox Reference](Toolbox-Reference.md#controller-rules).

## Profiles and setup

- **Profiles** — create, edit, import, and export mapping INIs.
- **Guided setup** — teach Ghostcontrol every control on an unknown device.
- **Lights & Rumble** — DualSense lightbar FX and motor tests.
- **Bluetooth** — pair / forget / retry wireless pads.
- **Service** — start/stop/restart, autostart, diagnostics.

## Logs

- App log (while the title runs): `/mnt/sandbox/<TITLE_ID>_000/download0/ghostcontrol/app.log`
- Service log: under `/data/ghostcontrol/` (exact path depends on companion variant)

See [Troubleshooting and Logs](Troubleshooting-and-Logs.md).

## Recover

- **Restart service** from the Service page if input dies after unplug/replug.
- Close the Toolbox from the control center (**Close App**) before replacing the installed title folder.
- Reload the companion on **9021** after a full reboot / new jailbreak session.

# Ghostcontrol Toolbox Reference

## Dashboard

Dashboard shows the service version and state, connected USB controller count, console model, serial number, OpenPSID, controller slots, and recent service messages.

- **Rebind controllers** reapplies the current profile and binding decisions.
- **Reset controller connections** recreates virtual controller connections and may reopen the PS5 user-assignment screen.
- A slot that says **press a button to assign** has not completed the PS5 assignment flow.

OpenPSID is shown once. It is console identity information, not controller information.

## Profiles

A profile controls button mapping, axes, deadzones, sensitivity, inversion, and trigger thresholds.

- **Automatic** chooses the closest enabled profile for each controller's VID, PID, and protocol.
- Selecting a profile ID forces that profile as the active profile.
- **Create profile** makes a new profile from the default settings.
- **Import profile** validates an INI from `/data/ghostcontrol/imports` and installs it into the live profile folder.
- **Export active profile** writes a shareable INI to `/data/ghostcontrol/exports`.
- **Reload profile files** parses newly copied INIs without reloading the ELF.
- The profile inventory lists every loaded `.ini` from `/data/ghostcontrol/profiles`.

See [Profile Sharing and Import](Profile-Sharing-and-Import.md) before copying INI files by hand.

## Edit Selected Profile

This section edits the selected profile. In Automatic mode it displays the default profile.

- **Controller vendor**, **Controller product**, and **Protocol** control automatic matching.
- **Deadzone** ignores small stick movement near the center.
- **Sensitivity** changes how quickly stick output reaches its maximum.
- **Trigger threshold** sets when an analog trigger becomes a button press.
- **Invert X/Y** reverses an axis.
- Each **maps to** row changes the PlayStation output produced by a source button. Choose **Disabled** to suppress a source.
- **Rename profile file** changes the selected filename and its internal ID together.
- Learned input binding rows expose the saved raw button and axis records for advanced repairs.

Use Guided Setup for a new controller. The editor is best for small corrections after the controller already works.

## Controller Rules

Rules tell Ghostcontrol how to open a USB device. Profiles tell it how to interpret and remap the input.

1. Connect the controller and select **Refresh connected controllers**.
2. Select its VID and PID.
3. Choose the detected protocol.
4. Leave input and output endpoints on **Automatic** unless hardware research proves a particular endpoint is required.
5. Turn on **Use this rule** and save it.
6. Reconnect the controller or restart the service.

Turn on **Ignore this controller** when Ghostcontrol must leave a matching USB device alone.

## Custom Controller Setup

The guided option is at the top of this section. It learns every PlayStation control from real input, shows a review, and saves a new profile without overwriting an existing ID.

Use the normal DualSense to operate the menu and the custom pad only for requested inputs. Follow [Guided Controller Setup](Guided-Controller-Setup.md) for the complete sequence.

The manual **Button to learn** option is useful for repairing one mapping. **Skip current control** is only for a control the hardware does not have.

## DualSense Lightbar FX

Enable a solid, police, breathe, rainbow, or strobe effect on detected DualSense controllers. RGB values run from `0` to `255`. Cycle period controls the full animation time; color step controls update spacing. **Release lightbar to PS5** disables the effect and gives normal lightbar control back to the system.

## DualSense Motion

Opens a live page for orientation quaternion, gravity, and gyroscope values from a connected DualSense. It updates once every 10 seconds. See [DualSense Tools](DualSense-Tools.md).

## Manual Rumble

Opens a page for testing the large and small DualSense rumble motors, pulse timing, and repetition. See [DualSense Tools](DualSense-Tools.md).

## Wireless DS4

Enables paired DualShock 4 input through the selected profile. The status shows connected controllers and active game routes. Use **Refresh status** after changing connections.

## GhostPad Remote Input

Turn on **Allow GhostPad network input** to accept the GhostPad desktop,
browser/ESP32 bridge, or another compatible client. TCP port `6967` is the
default and can be changed here; clients must use the same port. The
main controller service must also be running. Connect the client, then use its
controls to choose a user on the normal PS5 assignment screen.

The listener is off by default and accepts one client at a time. Turn it on
only on a trusted local network. Turning the toggle or controller service off
sends a neutral report, closes the connection, and removes Ghostcontrol's
virtual controller.

**Stuck-input release** controls how long Ghostcontrol waits without a complete
GPAD frame before sending a neutral state. `250 ms` is the recommended default
for clients streaming at 60 Hz. GhostPad's low-latency latest-state behavior is
always active: when several complete frames are queued, obsolete intermediates
are skipped and only the newest state is injected. The status counters show
received input, skipped stale frames, rejected packets, and automatic releases.

The Service section has a separate **USB stuck-input release** setting for USB
HID and XInput caches. The default `400 ms` matches GhostPad's proven two-cycle
release guard. Set it to `0` only when a change-only controller must hold a
button indefinitely and reliably sends its release report.

## Service

- **Start input when payload loads** controls autostart.
- **Start service** begins USB input and virtual controller creation.
- **Stop service** ends controller input.
- **Restart service** closes and reopens controller connections.
- **Reload configuration** rereads runtime settings, controller rules, and profile files.
- **Refresh status** updates the displayed service state.

## Credits

Credits shows `Created by StonedModder` and `https://vice.se7ensins.com/`. It also contains the nested Updates and Debug pages.

### Updates

Update controls check for and download a later build. **Include prereleases** permits test builds. Downloads are stored for a later manual launch; downloading does not silently replace the currently running injected code. **Restore previous download** rolls back the downloaded candidate.

### Debug

Debug shows recent service log lines and the important paths. Use it first when a controller is missing, a profile is rejected, or setup fails. Full diagnostic collection is covered in [Troubleshooting and Logs](Troubleshooting-and-Logs.md).

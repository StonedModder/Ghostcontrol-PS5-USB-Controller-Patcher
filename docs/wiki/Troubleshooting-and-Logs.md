# Troubleshooting and Logs

## The Toolbox change is missing

Reboot the PS5, jailbreak again, and load the new combined ELF. Sending another build in the same boot preserves an already loaded ShellUI module, so UI changes may not replace the old menu.

## Controller is connected but has no input

1. Check that **Service** is running.
2. Open Dashboard and look for the controller VID/PID and assignment state.
3. If the slot says **press a button to assign**, complete the PS5 user-assignment screen.
4. Select **Rebind controllers**.
5. Reconnect the controller or select **Restart service**.
6. If the device is still absent, refresh **Controller Rules** and verify its protocol. Leave endpoints on Automatic unless the hardware requires an override.

## A copied profile does not appear

- Confirm it is in `/data/ghostcontrol/profiles/`, with lowercase `ghostcontrol`.
- Confirm the filename ends in `.ini`.
- Confirm the filename base exactly matches `[profile] id` inside the file.
- Reconnect the controller. Ghostcontrol reloads profile files when a newly
  connected controller has no current match.
- You can also select **Service > Reload configuration** before reconnecting.
- Read Debug for the first validation error. One malformed direct-copy file prevents a new set from replacing the previous working in-memory set.

## Automatic chose the wrong profile

Check the profile's VID, PID, and protocol against Dashboard or the connected-device list. When several profiles have the same match, the alphabetically first ID wins. Select the wanted profile explicitly or disable the unwanted duplicate match.

## Guided Setup will not advance

- Wait for the highlighted instruction to change before pressing the next input.
- Digital controls need three matching press/release cycles.
- L3 and R3 require three real stick clicks and never advance automatically.
- Release sticks and triggers fully before an axis step.
- If a controller is asleep or incomplete at CROSS, cancel and restart Guided Setup so both wake cycles run.

## Useful log locations

The Toolbox Debug page displays recent service lines. Full console files can be downloaded over the console FTP server, commonly port `2120`.

All Ghostcontrol data and diagnostic captures:

```text
/data/ghostcontrol/
```

Keep each downloaded test capture in its own named folder under the repository's `payload/logs/` directory. Store kernel logs and release artifacts in the matching `test-builds/<version>/` folder. Do not drop ELFs or loose log files into the repository root.

When reporting a problem, include the build version, ShellUI setup marker, controller model and mode, VID/PID, the action that caused the issue, the attached kernel log, and the matching FTP log folder.

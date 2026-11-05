# Profile Sharing and Import

## Correct profile path

The live profile directory is:

```text
/data/ghostcontrol/profiles/
```

The lowercase `ghostcontrol` is required. Ghostcontrol test 60 uses this same
root for every file it creates, including logs.

## Install a shared profile directly

1. Copy the INI to `/data/ghostcontrol/profiles/<id>.ini` through FTP or another file manager.
2. Reconnect the controller. On the first unmatched connection, Ghostcontrol
   reloads the directory and admits an enabled exact VID/PID/protocol match.
3. Under **Profiles**, confirm the filename appears in the profile inventory,
   then choose **Automatic** or select the profile ID directly.
4. If the controller was already connected, reload configuration and reconnect
   it, or restart the service.

For the test 57 Logitech F310 profile, the correct destination is:

```text
/data/ghostcontrol/profiles/my-controller.ini
```

Its exact match is VID `046d`, PID `c21d`, protocol `xusb`, so Automatic selects it for that controller mode. An enabled exact-match profile also identifies and admits the controller without requiring a separately shared controller rule.

## Import through the Toolbox

Toolbox import keeps incoming files separate until Ghostcontrol validates them.

1. Copy `<id>.ini` to `/data/ghostcontrol/imports/`.
2. Enter `<id>` in **Profiles > Import profile**.
3. Run the import action.
4. Ghostcontrol validates and copies it to `/data/ghostcontrol/profiles/`.

Exports are written to `/data/ghostcontrol/exports/<id>.ini` and are ready to share.

## File rules

- The filename must end in `.ini`; extension matching is case-insensitive.
- The filename before `.ini` must exactly match `[profile] id` inside the file, including letter case.
- IDs may contain letters, numbers, `_`, and `-`, and must fit the Toolbox's 63-character field.
- The profile schema and every mapping must parse successfully.
- The file must be a regular file, not a symbolic link.
- An input file may be at most 64 KiB.
- Duplicate IDs are rejected because each ID has one filename.

Ghostcontrol loads the whole profile directory as one consistent set. If a directly copied file is malformed, Reload reports the error and continues using the previous in-memory set. Remove or repair the bad file before the next reload.

## Capacity and selection behavior

Ghostcontrol test 60 supports 128 loaded profiles, including the two built-in profiles. That leaves room for 126 user profiles. A host regression check fills all 128 slots, verifies deterministic ordering, rejects a 129th profile, and confirms a failed save is rolled back without poisoning later reloads.

The Toolbox lists every loaded `.ini` under **Profiles**. Select a profile to edit its identity matching, mappings, stick settings, and learned button/axis bindings. **Rename profile file** changes the filename and internal profile ID together; `default.ini` remains fixed because Automatic mode uses it as the fallback.

Automatic selection scores enabled profiles against controller VID, PID, and protocol. An exact match wins over a generic profile. If several profiles have the same best exact match, the alphabetically first profile ID wins. Select a specific profile manually when you want a different one.

## Before sharing

1. Export the active profile from the Toolbox.
2. Confirm its filename matches its `[profile] id`.
3. Record the controller model, connection mode, VID, PID, and protocol with the download.
4. Test a clean import or direct copy followed by Reload configuration.
5. Verify face buttons, D-pad, shoulders, triggers, both sticks, L3, and R3 in a game or input test.

One physical controller can expose different VID/PID or report layouts in XInput, DirectInput, Switch, and vendor modes. State the tested mode so another user can reproduce the match.

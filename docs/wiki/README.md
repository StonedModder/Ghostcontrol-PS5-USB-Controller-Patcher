# Ghostcontrol Toolbox Wiki

Ghostcontrol lets a jailbroken PS5 use supported USB controllers through a virtual DualSense. One combined ELF starts the controller service and adds the **Ghostcontrol** icon and **Ghostcontrol Toolbox** to the PS5 home screen.

This folder is written as a set of Markdown chapters so each page can be copied into a XenForo post. Begin with [Getting Started](Getting-Started.md), then use the reference pages when you need a particular feature.

## Wiki pages

- [Getting Started](Getting-Started.md) — load Ghostcontrol, assign the virtual controller, and confirm input.
- [Toolbox Reference](Toolbox-Reference.md) — every section of the Toolbox and when to use it.
- [Guided Controller Setup](Guided-Controller-Setup.md) — create a controller profile by pressing each control.
- [Profile Sharing and Import](Profile-Sharing-and-Import.md) — install, export, share, and safely load profile INI files.
- [DualSense Tools](DualSense-Tools.md) — motion readings, manual rumble, and lightbar effects.
- [Troubleshooting and Logs](Troubleshooting-and-Logs.md) — common recovery steps and log locations.

## Fast path

1. Jailbreak the PS5 and start the ELF loader.
2. Send the combined `Ghostcontrol-<version>.elf` to the loader.
3. Connect the custom USB controller.
4. Open **Ghostcontrol > Ghostcontrol Toolbox**.
5. In **Service**, select **Start service** if it is not already running.
6. When the PS5 asks who is using the controller, select the account that should receive its input.
7. If the controller is unknown, use **Custom Controller Setup > Guided Controller Setup**.

## Important data paths

PS5 paths are case-sensitive. Ghostcontrol uses one lowercase data root for
profiles, settings, runtime files, updates, exports, and logs:

```text
/data/ghostcontrol/
```

ShellUI may display the same physical directory through the PS5 alias
`/user/data/ghostcontrol/`; this is not a second data folder.

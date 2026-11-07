# Getting Started

## What you need

- A jailbroken PS5 with an ELF loader.
- The combined Ghostcontrol ELF for your firmware and test version.
- A supported USB controller and its cable or receiver.
- A normal DualSense to operate the Toolbox while the custom controller is being configured.

Ghostcontrol is distributed as one combined ELF. It contains the controller service and the PS5 home-screen integration. Do not send a separate Ghostcontrol UI payload afterward.

## Load Ghostcontrol

1. Jailbreak the console and start its ELF loader.
2. Send `Ghostcontrol-<version>.elf` to the loader port, normally `9021`.
3. Wait for the Ghostcontrol notification.
4. Open the new **Ghostcontrol** home-screen item and enter **Ghostcontrol Toolbox**.

If a newly built Toolbox does not appear after sending another ELF, reboot, jailbreak again, and load the new ELF. Reloading an ELF does not replace an already injected ShellUI module in the current boot.

## Connect and assign a controller

1. Plug in the custom USB controller before starting the service.
2. Open **Service** in the Toolbox.
3. Turn on **Start input when payload loads** if you want controller input to start automatically on later launches.
4. Select **Start service**.
5. Ghostcontrol creates a virtual DualSense. The PS5 opens its normal **Who's using this controller?** screen.
6. Press the requested button and choose the account that should use the virtual controller.
7. After assignment, Ghostcontrol keeps sending the custom controller's input through that virtual controller.

The account is chosen by the PS5 screen. Ghostcontrol does not need you to type a user ID. If more than one custom controller is connected, assign each virtual controller when its prompt appears. The Dashboard shows each connected slot and whether it is waiting for assignment.

## Confirm basic operation

Open **Dashboard** and check:

- **Service** reports that it is running.
- **USB controllers** is greater than zero.
- A connected slot shows the controller VID and PID.
- The slot no longer says **press a button to assign** after account assignment.

Test the D-pad, face buttons, shoulders, triggers, both sticks, L3, and R3. If the controller is found but its controls are wrong, create a profile with [Guided Controller Setup](Guided-Controller-Setup.md). If it is not found, refresh connected controllers and review [Controller Rules](Toolbox-Reference.md#controller-rules).

## Reconnect or recover assignment

- **Rebind controllers** reapplies profile selection and controller bindings.
- **Reset controller connections** recreates the controller connections. The PS5 may ask for user assignment again.
- **Restart service** is the quickest recovery when input stopped after unplugging or changing a rule.
- **Reload configuration** reads profile and rule files copied to the console after Ghostcontrol started.

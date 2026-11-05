# Guided Controller Setup

Guided Setup creates a profile for a connected controller by observing the physical signal for every requested control. L3 and R3 are learned from real presses like every other digital button. They are never inserted automatically.

## Before starting

1. Connect the custom controller by USB or its USB receiver.
2. Keep a normal DualSense connected for navigating the Toolbox.
3. Start the Ghostcontrol service.
4. Open **Custom Controller Setup**.
5. Select **Refresh connected controllers**.
6. Select the custom controller by its VID and PID.
7. Enter a unique profile ID using letters, numbers, `_`, or `-`. The file and profile ID use this name.
8. Choose **Default button and stick settings** or copy an existing profile as a starting point.
9. Select **Start guided setup** at the top of the page.

## Wake check

Some controllers do not send their full input report immediately after opening. Guided Setup begins with a wake check to prevent the first CROSS press from being consumed while the controller is still waking.

Press and release a button twice when the wake prompt asks. These two wake cycles do not create a mapping. CROSS begins only after the second release has been observed.

## Digital controls

For each requested button:

1. Read the highlighted status line.
2. Press the requested physical control and release it.
3. Repeat until the status reports `3/3` confirmations.
4. Wait for the prompt to change before pressing the next control.

The first press counts. The three signals must match. If another physical signal appears during confirmation, Guided Setup resets that step instead of silently assigning the wrong signal.

The digital order is:

```text
CROSS, CIRCLE, SQUARE, TRIANGLE
UP, DOWN, LEFT, RIGHT
L1, R1, L2, R2
L3, R3
OPTIONS, SHARE, PS, TOUCHPAD
```

For L3 and R3, click the left or right stick three separate times and fully release it after each click. Guided Setup must remain on that step until those three presses are received.

If the controller reports a signal that was already assigned, Guided Setup transfers that signal to the current target and then asks you to repair the displaced target. Follow the repair prompt; skipping is not required.

## Sticks and triggers

After the buttons, Guided Setup learns six axes:

- Center all controls, then move the left stick fully right for `AXIS_LX`.
- Center it, then move the left stick fully down for `AXIS_LY`.
- Center all controls, then move the right stick fully right for `AXIS_RX`.
- Center it, then move the right stick fully down for `AXIS_RY`.
- Release everything, then fully press L2 for `AXIS_L2`.
- Release everything, then fully press R2 for `AXIS_R2`.

Return the requested control to rest between steps. This gives Ghostcontrol both the center and full-range direction needed to normalize the axis.

## Missing controls

Use **Skip current control** only when the controller truly lacks that control. A skipped control keeps the starting profile's behavior when possible. Do not skip L3 or R3 merely because the first click was missed; click the stick again and wait for the confirmation counter.

## Completion and review

When step 24 completes, the Toolbox automatically sends the finished profile
through Ghostcontrol's transactional save. This writes the profile, creates or
updates its controller rule, and enables Automatic matching together. A save
failure leaves the previous configuration intact and reports the reason.

1. Open **Review learned controls** and check the physical-source to PlayStation-target mappings.
2. Confirm that L3 and R3 are present when the controller has stick-click buttons.
3. Open **Profiles > Edit Selected Profile** for any correction.
4. Restart or rebind controllers if the current connection still uses the previous profile.

The saved file is `/data/ghostcontrol/profiles/<profile-id>.ini`. Existing profile IDs for another controller are not overwritten by Guided Setup.

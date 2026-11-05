# DualSense Tools

These tools operate on a connected real DualSense. They are top-level items in Ghostcontrol Toolbox.

## Live DualSense Motion

Open **DualSense Motion** to view:

- Orientation quaternion: `x`, `y`, `z`, `w`.
- Gravity vector: `x`, `y`, `z`.
- Gyroscope: `x`, `y`, `z`.

The page refreshes once every 10 seconds. Hold or rotate the DualSense, wait for the next update, and compare the values. A steady controller can legitimately report nearly unchanged values.

If every field is unavailable, connect or wake a real DualSense, close and reopen the page, and check Debug for the motion-source result.

## Manual Rumble

Manual Rumble tests the two motors independently:

- **Large motor** sets the low-frequency/heavier vibration from `0` to `255`.
- **Small motor** sets the high-frequency/lighter vibration from `0` to `255`.
- **On time** controls how long each pulse runs.
- **Off time** controls the gap between pulses.
- **Repeat** runs once, 5 times, 10 times, or forever.

Set at least one motor above zero, choose the timing and repeat count, then select **Start**. Select **Stop** to end it immediately, including a forever pattern. Use moderate values first and raise one motor at a time when comparing their feel.

## DualSense Lightbar FX

The Lightbar section controls solid, police, breathe, rainbow, and strobe effects. For solid color and effect tint, enter red, green, and blue values from `0` to `255`. Period controls the animation cycle; step controls how often its color changes.

Select **Release lightbar to PS5** when testing is complete so the system can resume its normal player and game colors.

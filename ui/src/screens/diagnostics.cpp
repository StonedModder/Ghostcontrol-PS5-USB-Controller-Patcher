/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Diagnostics screen: what the service is seeing.
 *
 * Model, serial, openpsid and the active-slot count are read from the service
 * now. Recent log and Setup capture log retain their historical LOG_TAIL
 * readers. Selected source raw packet reads the bounded SETUP_INPUT cache for
 * a valid bound physical setup source, never the unrelated app-native sample.
 * All three fetch through the worker, refresh on Cross and show explicit
 * failure/unavailable states; cached packets are snapshots, not live output.
 *
 * The four Bluetooth rows (BT-R01) read the "diagnostics" object of the
 * worker's BLUETOOTH_STATUS poll: what the service's own USB transport, host
 * and engine measured -- radio USB identity and interface, traffic and drop
 * counters, and the latest failure with its age. No IPC happens on the
 * frame; a stalled service Bluetooth thread shows as stale.
 */
#include "screens.hpp"

namespace ghostcontrol
{

Section build_screen_diagnostics()
{
    return {"Diagnostics", "Device details and logs for troubleshooting",
            {
                {"Console model", "-", "Model reported by the service, if available.", RowState::live, {},
                 LiveSource::console_model},
                {"Console serial", "-", "Serial reported by the service, if available.", RowState::live, {},
                 LiveSource::console_serial},
                {"Console OpenPSID", "-", "Console identifier reported by the service, if available.", RowState::live, {},
                 LiveSource::console_openpsid},
                {"Connected virtual slots", "-", "Service-reported virtual slots, not this app's controller.", RowState::live, {},
                 LiveSource::active_slots},
                {"Bluetooth radio", "-", "Radio identity and interface details.",
                 RowState::live, {}, LiveSource::bt_radio, nullptr, nullptr, LiveSource::none, OverlayKind::none,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, {}, ReaderKind::bluetooth_diagnostics},
                {"Bluetooth traffic", "-", "Received packets and time since the last activity.",
                 RowState::live, {}, LiveSource::bt_traffic, nullptr, nullptr, LiveSource::none, OverlayKind::none,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, {}, ReaderKind::bluetooth_diagnostics},
                {"Bluetooth drops", "-", "Packets dropped by the service.",
                 RowState::live, {}, LiveSource::bt_drops, nullptr, nullptr, LiveSource::none, OverlayKind::none,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, {}, ReaderKind::bluetooth_diagnostics},
                {"Last Bluetooth error", "-", "Latest radio or connection error and when it happened.",
                 RowState::live, {}, LiveSource::bt_last_failure, nullptr, nullptr, LiveSource::none, OverlayKind::none,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, {}, ReaderKind::bluetooth_diagnostics},
                /* The reader rows open a panel rather than reading a value;
                 * RowState::action tags them OPEN so they never look like a
                 * measurement. The reader's lines come from a fetched
                 * LOG_TAIL, not the row table. */
                {"Recent log", "", "Recent service log messages.", RowState::action, {},
                 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::none,
                 0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, {}, ReaderKind::log_tail},
                {"Setup capture log", "", "Past setup messages, not a live input view.",
                 RowState::action, {}, LiveSource::none, nullptr, nullptr, LiveSource::none,
                 OverlayKind::none, 0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, {},
                 ReaderKind::raw_input},
                {"Selected device packet", "", "View cached setup input bytes; press Cross to refresh.",
                 RowState::action, {}, LiveSource::none, nullptr, nullptr, LiveSource::none,
                 OverlayKind::none, 0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, {},
                 ReaderKind::setup_raw},
            }};
}

} // namespace ghostcontrol

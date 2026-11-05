/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Console HCI opener for the in-core Bluetooth source.
 *
 * Kept in its own translation unit so the engine (gc_bt.c) carries no USB or
 * SDK dependency and stays unit-testable against the simulated controller.
 * This file is compiled only into the PS5 build, where it opens the console's
 * Bluetooth USB-HCI function. Tests provide their own gc_bt_open_hci. */
#include "gc_bt.h"
#include "hci.h"
#include "hci_usb.h"

int gc_bt_open_hci(void *out_hci)
{
    return hci_usb_open((hci_t *)out_hci);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Positive values are not interchangeable publication authority. */
enum GcUsbDecodeResult {
    GC_USB_DECODE_REJECTED = 0,
    GC_USB_DECODE_CONTROLS = 1,
    GC_USB_DECODE_SKIPPED = 2,
    GC_USB_DECODE_NEUTRAL_PROBE = 3
};

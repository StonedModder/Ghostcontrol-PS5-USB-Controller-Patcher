/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A log the console can hand back.
 *
 * The kit's sys::log goes to stdout and the kernel debug channel, neither of
 * which is readable from a PC. A title can write to its own storage, and while
 * it runs that storage is visible over FTP as
 *
 *     /mnt/sandbox/<TITLE_ID>_000/download0/ghostcontrol/app.log
 *
 * so a stall has evidence instead of guesswork. Every line is flushed, because
 * the interesting case is the one where the process never gets any further.
 */
#pragma once

namespace ghostcontrol
{

/** Opens the log. Safe to call once; failure is silent and harmless. */
void app_log_open();

/** Appends one line. printf-style. */
void app_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/** Closes it. */
void app_log_close();

} // namespace ghostcontrol

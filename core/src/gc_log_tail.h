/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

/* LOG_TAIL takes an optional line count: "GC1 LOG_TAIL 40". With no argument
 * it returns the default number of lines. The count is clamped to
 * GC_LOG_TAIL_MAX_LINES and the returned byte window is capped at
 * GC_LOG_TAIL_MAX_BYTES so the hex-encoded reply always fits inside
 * GC_IPC_MAX_RESPONSE (65536): two hex characters per byte plus the JSON
 * wrapper stays well under the socket's response buffer. */
#define GC_LOG_TAIL_DEFAULT_LINES 200
#define GC_LOG_TAIL_MAX_LINES     1000
#define GC_LOG_TAIL_MAX_BYTES     16384

/* Parses the optional LOG_TAIL argument into a line count. An empty argument
 * selects GC_LOG_TAIL_DEFAULT_LINES; a blank, non-numeric, zero or negative
 * argument is rejected (returns non-zero). Values above the ceiling clamp. */
int gc_log_tail_parse_lines(const char *argument, int *lines_out);

/* Returns the byte offset in [text, text + length) at which the last `lines`
 * lines begin, or 0 when the buffer holds fewer lines than requested. A line
 * starts at offset 0 and after every '\n'. */
size_t gc_log_tail_start(const char *text, size_t length, int lines);

/* Writes the LOG_TAIL reply for the already-read tail buffer:
 *   {"ok":true,"encoding":"hex","lines":N,"data":"<hex>"}\n
 * `lines` is the number of lines actually returned. Returns 0 on success and
 * -1 when the reply would not fit, so a short buffer fails instead of
 * overflowing. */
int gc_log_tail_format(const char *text, size_t length, int lines,
                       char *response, size_t response_size);

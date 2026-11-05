/* SPDX-License-Identifier: GPL-3.0-or-later
 * LOG_TAIL reply shaping, split out of gc_main.c so the argument parsing and
 * the tail windowing can be unit-tested with no file or socket dependency.
 * The caller reads at most GC_LOG_TAIL_MAX_BYTES from the end of the log and
 * hands that buffer here; the hex encoding keeps the reply one ASCII line. */
#include "gc_log_tail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gc_log_tail_parse_lines(const char *argument, int *lines_out)
{
    if (!lines_out)
        return -1;
    if (!argument || !*argument) {
        *lines_out = GC_LOG_TAIL_DEFAULT_LINES;
        return 0;
    }
    char *end = NULL;
    long value = strtol(argument, &end, 10);
    if (end == argument || *end != '\0' || value <= 0)
        return -1;
    if (value > GC_LOG_TAIL_MAX_LINES)
        value = GC_LOG_TAIL_MAX_LINES;
    *lines_out = (int)value;
    return 0;
}

size_t gc_log_tail_start(const char *text, size_t length, int lines)
{
    if (!text || lines <= 0)
        return 0;
    size_t remaining = (size_t)lines;
    size_t index = length;
    while (index > 0) {
        --index;
        if (index != 0 && text[index - 1] != '\n')
            continue;
        /* index is a line start (offset 0 or the byte after a '\n'). */
        if (--remaining == 0)
            return index;
    }
    return 0;
}

static int count_lines(const char *text, size_t length)
{
    if (length == 0)
        return 0;
    int lines = 0;
    for (size_t index = 0; index < length; ++index)
        if (text[index] == '\n')
            ++lines;
    /* A final line without a terminating newline still counts. */
    if (text[length - 1] != '\n')
        ++lines;
    return lines;
}

int gc_log_tail_format(const char *text, size_t length, int lines,
                       char *response, size_t response_size)
{
    if (!response || response_size == 0)
        return -1;
    if (!text)
        length = 0;
    size_t start = gc_log_tail_start(text, length, lines);
    size_t window = length - start;
    if (window > GC_LOG_TAIL_MAX_BYTES) {
        /* Defensive: never emit more than the documented byte ceiling. */
        start = length - GC_LOG_TAIL_MAX_BYTES;
        window = GC_LOG_TAIL_MAX_BYTES;
    }
    const char *data = text + start;
    static const char hex[] = "0123456789abcdef";
    int actual = count_lines(data, window);
    size_t prefix = (size_t)snprintf(
        response, response_size,
        "{\"ok\":true,\"encoding\":\"hex\",\"lines\":%d,\"data\":\"", actual);
    if (prefix >= response_size ||
        prefix + window * 2 + 4 >= response_size)
        return -1;
    for (size_t index = 0; index < window; ++index) {
        response[prefix + index * 2] = hex[(unsigned char)data[index] >> 4];
        response[prefix + index * 2 + 1] = hex[(unsigned char)data[index] & 15];
    }
    snprintf(response + prefix + window * 2,
             response_size - prefix - window * 2, "\"}\n");
    return 0;
}

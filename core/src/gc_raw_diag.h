/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline int gc_raw_report_changed(const uint8_t *previous,
                                        size_t previous_len,
                                        const uint8_t *current,
                                        size_t current_len)
{
    if (!current && current_len) return 1;
    if (!previous) return current_len != 0 || previous_len != 0;
    if (previous_len != current_len) return 1;
    return current_len && memcmp(previous, current, current_len) != 0;
}

static inline int gc_raw_delta_append(char *output, size_t output_size,
                                      size_t *offset, const char *format,
                                      size_t index, int old_value,
                                      int new_value)
{
    if (!output || !offset || *offset >= output_size) return -1;
    int written;
    if (old_value < 0)
        written = snprintf(output + *offset, output_size - *offset,
                           format, index, new_value);
    else if (new_value < 0)
        written = snprintf(output + *offset, output_size - *offset,
                           format, index, old_value);
    else
        written = snprintf(output + *offset, output_size - *offset,
                           format, index, old_value, new_value);
    if (written < 0 || (size_t)written >= output_size - *offset) return -1;
    *offset += (size_t)written;
    return 0;
}

static inline int gc_raw_delta_format(const uint8_t *previous,
                                      size_t previous_len,
                                      const uint8_t *current,
                                      size_t current_len,
                                      char *output, size_t output_size)
{
    if (!output || !output_size || (!current && current_len)) return -1;
    output[0] = '\0';
    size_t offset = 0;
    if (previous_len != current_len) {
        int written = snprintf(output, output_size, "len:%zu>%zu",
                               previous_len, current_len);
        if (written < 0 || (size_t)written >= output_size) return -1;
        offset = (size_t)written;
    }
    const size_t count = previous_len > current_len ? previous_len : current_len;
    for (size_t index = 0; index < count; ++index) {
        int old_value = index < previous_len && previous ? previous[index] : -1;
        int new_value = index < current_len ? current[index] : -1;
        if (old_value == new_value) continue;
        const char *format;
        if (old_value < 0)
            format = offset ? ",%zu:-->%02x" : "%zu:-->%02x";
        else if (new_value < 0)
            format = offset ? ",%zu:%02x>--" : "%zu:%02x>--";
        else
            format = offset ? ",%zu:%02x>%02x" : "%zu:%02x>%02x";
        if (gc_raw_delta_append(output, output_size, &offset, format,
                                index, old_value, new_value) != 0)
            return -1;
    }
    if (!offset) {
        if (output_size < 5) return -1;
        memcpy(output, "none", 5);
    }
    return 0;
}

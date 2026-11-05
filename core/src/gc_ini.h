/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

typedef int (*GcIniEntryFn)(void *context, const char *section,
                            const char *key, const char *value,
                            char *error, size_t error_size);

/* Parse bounded UTF-8-ish INI text. Values are passed verbatim except for
 * surrounding whitespace and one matching pair of single/double quotes. */
int gc_ini_parse(const char *text, size_t length, GcIniEntryFn on_entry,
                 void *context, char *error, size_t error_size);


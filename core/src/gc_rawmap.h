#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gc_profile.h"

#define GC_RAWMAP_PAGE 0
#define GC_RAWMAP_REPORT 0xffu

int gc_rawmap_is_xusb(const uint8_t *report, size_t length);
uint32_t gc_rawmap_digital_mask(const uint8_t *report, size_t length);
/* Little-endian up-to-4-byte window starting at a byte offset; 0 when out of range. */
uint32_t gc_rawmap_window(const uint8_t *report, size_t length, unsigned offset);
int gc_rawmap_source_from_mask(const uint8_t *report, size_t length,
                               uint32_t bit, GcHidSource *source);
int gc_rawmap_bit_on(const uint8_t *report, size_t length,
                     const GcHidSource *source);
void gc_rawmap_apply(const GcProfile *profile, const uint8_t *report,
                     size_t length, ScePadData *pad);

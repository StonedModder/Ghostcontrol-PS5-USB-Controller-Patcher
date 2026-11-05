#include "gc_rawmap.h"

#include <string.h>

int gc_rawmap_is_xusb(const uint8_t *report, size_t length)
{
    return report && length >= 14 && report[0] == 0x00 && report[1] == 0x14;
}

static void digital_range(const uint8_t *report, size_t length,
                          unsigned *offset, unsigned *count)
{
    *offset = 0;
    *count = 0;
    if (!report || !length)
        return;
    if (gc_rawmap_is_xusb(report, length)) {
        *offset = 2;
        *count = 2;
        return;
    }
    if (length == 8) {
        *offset = 4;
        *count = 2;
        return;
    }
    *offset = 0;
    *count = length > 4 ? 4 : (unsigned)length;
}

uint32_t gc_rawmap_digital_mask(const uint8_t *report, size_t length)
{
    unsigned offset = 0, count = 0, i;
    uint32_t mask = 0;
    digital_range(report, length, &offset, &count);
    if (!count || offset + count > length)
        return 0;
    if (count > 4)
        count = 4;
    for (i = 0; i < count; ++i)
        mask |= (uint32_t)report[offset + i] << (8u * i);
    return mask;
}

uint32_t gc_rawmap_window(const uint8_t *report, size_t length, unsigned offset)
{
    uint32_t value = 0;
    unsigned i;
    if (!report || !length || offset >= length)
        return 0;
    for (i = 0; i < 4 && offset + i < length; ++i)
        value |= (uint32_t)report[offset + i] << (8u * i);
    return value;
}

int gc_rawmap_source_from_mask(const uint8_t *report, size_t length,
                               uint32_t bit, GcHidSource *source)
{
    unsigned offset = 0, count = 0, index = 0;
    unsigned byte, shift;
    if (!source || !bit || (bit & (bit - 1)))
        return -1;
    digital_range(report, length, &offset, &count);
    while (bit > 1u) {
        bit >>= 1;
        ++index;
    }
    byte = offset + index / 8u;
    shift = index % 8u;
    if (byte >= length || byte >= offset + count)
        return -1;
    memset(source, 0, sizeof(*source));
    source->offset = (uint16_t)(byte * 8u + shift);
    source->bits = 1;
    source->report = GC_RAWMAP_REPORT;
    source->page = GC_RAWMAP_PAGE;
    source->minimum = 0;
    source->maximum = 1;
    return 0;
}

int gc_rawmap_bit_on(const uint8_t *report, size_t length,
                     const GcHidSource *source)
{
    unsigned byte, shift;
    if (!report || !source || source->page != GC_RAWMAP_PAGE ||
        source->report != GC_RAWMAP_REPORT || source->bits != 1)
        return 0;
    byte = source->offset / 8u;
    shift = source->offset % 8u;
    if (byte >= length)
        return 0;
    return (report[byte] >> shift) & 1u;
}

static uint32_t xusb_decoder_bit(unsigned byte, unsigned shift)
{
    static const uint32_t byte2[] = {
        SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_DOWN, SCE_PAD_BUTTON_LEFT,
        SCE_PAD_BUTTON_RIGHT, SCE_PAD_BUTTON_OPTIONS, SCE_PAD_BUTTON_SHARE,
        SCE_PAD_BUTTON_L3, SCE_PAD_BUTTON_R3
    };
    static const uint32_t byte3[] = {
        SCE_PAD_BUTTON_L1, SCE_PAD_BUTTON_R1, SCE_PAD_BUTTON_PS, 0,
        SCE_PAD_BUTTON_CROSS, SCE_PAD_BUTTON_CIRCLE, SCE_PAD_BUTTON_SQUARE,
        SCE_PAD_BUTTON_TRIANGLE
    };
    if (shift > 7)
        return 0;
    if (byte == 2)
        return byte2[shift];
    if (byte == 3)
        return byte3[shift];
    return 0;
}

void gc_rawmap_apply(const GcProfile *profile, const uint8_t *report,
                     size_t length, ScePadData *pad)
{
    int target;
    uint32_t replaced = 0;
    if (!profile || !report || !pad)
        return;
    for (target = 0; target < GC_BUTTON_INPUT15; ++target) {
        const GcHidButtonBinding *binding = &profile->hid_buttons[target];
        unsigned byte, shift;
        if (!binding->present || binding->source.page != GC_RAWMAP_PAGE ||
            binding->source.report != GC_RAWMAP_REPORT)
            continue;
        replaced |= gc_button_mask((GcButton)target);
        byte = binding->source.offset / 8u;
        shift = binding->source.offset % 8u;
        pad->buttons &= ~xusb_decoder_bit(byte, shift);
    }
    if (!replaced)
        return;
    pad->buttons &= ~replaced;
    for (target = 0; target < GC_BUTTON_INPUT15; ++target) {
        const GcHidButtonBinding *binding = &profile->hid_buttons[target];
        if (!binding->present || binding->source.page != GC_RAWMAP_PAGE ||
            binding->source.report != GC_RAWMAP_REPORT)
            continue;
        if (gc_rawmap_bit_on(report, length, &binding->source))
            pad->buttons |= gc_button_mask((GcButton)target);
    }
}

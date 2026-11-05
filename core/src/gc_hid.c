/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_hid.h"
#include <string.h>
#include <limits.h>

static const GcButton hid_buttons[] = {GC_BUTTON_CROSS, GC_BUTTON_CIRCLE, GC_BUTTON_SQUARE,
    GC_BUTTON_TRIANGLE, GC_BUTTON_L1, GC_BUTTON_R1, GC_BUTTON_L2, GC_BUTTON_R2,
    GC_BUTTON_SHARE, GC_BUTTON_OPTIONS, GC_BUTTON_L3, GC_BUTTON_R3, GC_BUTTON_PS,
    GC_BUTTON_TOUCHPAD, GC_BUTTON_INPUT15, GC_BUTTON_INPUT16, GC_BUTTON_INPUT17, GC_BUTTON_INPUT18};

#define HID_CANONICAL_BUTTONS (sizeof(hid_buttons) / sizeof(hid_buttons[0]))

static uint32_t hid_hat_mask(int32_t minimum, int32_t maximum, int32_t value)
{
    static const uint32_t hats8[] = {
        SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_RIGHT,
        SCE_PAD_BUTTON_RIGHT, SCE_PAD_BUTTON_RIGHT | SCE_PAD_BUTTON_DOWN,
        SCE_PAD_BUTTON_DOWN, SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT,
        SCE_PAD_BUTTON_LEFT, SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_UP,
    };
    static const uint32_t hats4[] = {
        SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_RIGHT,
        SCE_PAD_BUTTON_DOWN, SCE_PAD_BUTTON_LEFT,
    };
    if (value < minimum || value > maximum)
        return 0;
    const int64_t position = (int64_t)value - minimum;
    const int64_t count = (int64_t)maximum - minimum + 1;
    if (count == 4)
        return hats4[position];
    if (count == 8)
        return hats8[position];
    return 0;
}

static int hid_array_usage(const GcHidField *field, int32_t value,
                           int is_null, uint16_t *usage)
{
    if (is_null || value < field->minimum || value > field->maximum)
        return 0;
    int64_t first = field->minimum;
    /* Button selector arrays commonly reserve logical zero for "none" while
     * declaring Button 1 as the first usage. */
    if (first == 0 && field->array_usage_min > 0) {
        if (value == 0)
            return 0;
        first = 1;
    }
    const int64_t selected = field->array_usage_min +
                             ((int64_t)value - first);
    if (selected < field->array_usage_min ||
        selected > field->array_usage_max || selected > UINT16_MAX)
        return 0;
    *usage = (uint16_t)selected;
    return *usage != 0;
}

static int32_t signed_item(uint32_t value, unsigned bytes)
{
    if (bytes && bytes < 4 && (value & (1u << (bytes * 8 - 1))))
        value |= UINT32_MAX << (bytes * 8);
    return (int32_t)value;
}

int gc_hid_parse_descriptor(const uint8_t *data, size_t size, GcHidLayout *layout)
{
    struct Global { uint32_t page, bits, count, report; int32_t min, max; } g = {0}, stack[8];
    unsigned stack_size = 0, depth = 0;
    int collections[16] = {0};
    uint32_t offsets[256] = {0}, usages[96], usage_count = 0, usage_min = 0, usage_max = 0;
    if (!layout || (!data && size)) return -1;
    memset(layout, 0, sizeof(*layout));
    for (size_t pos = 0; pos < size;) {
        uint8_t prefix = data[pos++];
        if (prefix == 0xfe) return -1; /* Unsupported long items must not be guessed. */
        unsigned bytes = prefix & 3u;
        if (bytes == 3) bytes = 4;
        if (bytes > size - pos) return -1;
        uint32_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value |= (uint32_t)data[pos++] << (i * 8);
        const unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: g.page = value; break;
            case 1: g.min = signed_item(value, bytes); break;
            case 2: g.max = g.min < 0 ? signed_item(value, bytes) :
                            value > INT32_MAX ? INT32_MAX : (int32_t)value; break;
            case 7: g.bits = value; break;
            case 8:
                if (!value || value > 255 ||
                    (!layout->numbered && offsets[0]))
                    return -1;
                g.report = value;
                layout->numbered = 1;
                break;
            case 9: g.count = value; break;
            case 10: if (stack_size == 8) return -1; stack[stack_size++] = g; break;
            case 11: if (!stack_size) return -1; g = stack[--stack_size]; break;
            default: break;
            }
        } else if (type == 2) {
            if (tag == 0) { if (usage_count == 96) return -1; usages[usage_count++] = value; }
            else if (tag == 1) usage_min = value;
            else if (tag == 2) usage_max = value;
        } else if (type == 0) {
            if (tag == 10) {
                if (depth == 16) return -1;
                int gamepad = depth ? collections[depth - 1] : 0;
                if (value == 1) {
                    uint32_t usage = usage_count ? usages[0] : usage_min;
                    const uint32_t page = usage > 65535 ? usage >> 16 : g.page;
                    usage &= 65535;
                    /* A pointer, mouse, keyboard or keypad is not a gamepad.
                     * Its fields carry no gamepad usages, so the positional
                     * fallback would invent buttons and sticks out of them and
                     * the wizard would offer a keyboard as a controller. */
                    if (!depth && page == 1 && usage >= 1 && usage <= 7 &&
                        usage != 4 && usage != 5)
                        return -1;
                    gamepad = (page == 1 &&
                               (usage == 4 || usage == 5 || usage == 8)) ||
                              (page == 2 && usage >= 1 && usage <= 0x0c);
                }
                collections[depth++] = gamepad;
            } else if (tag == 12) {
                if (!depth) return -1;
                --depth;
            } else if (tag == 8) {
                /* HID report 0 cannot coexist with numbered input reports.
                 * Global Push/Pop may otherwise restore report 0 after a
                 * Report ID and create fields the runtime can never address. */
                if ((layout->numbered && !g.report) ||
                    !g.bits || g.bits > 32 || g.count > 1024 ||
                    g.bits * g.count > 8192 || offsets[g.report] + g.bits * g.count > 8192)
                    return -1;
                for (uint32_t i = 0; i < g.count; ++i) {
                    uint32_t usage = i < usage_count ? usages[i] :
                        usage_max >= usage_min && i <= usage_max - usage_min ? usage_min + i : 0;
                    uint32_t page = usage > 65535 ? usage >> 16 : g.page;
                    usage &= 65535;
                    const int variable_absolute = (value & 7u) == 2;
                    const int button_array = (value & 7u) == 0 &&
                        g.page == 9 && usage_count == 0 &&
                        usage_min >= 1 && usage_max >= usage_min &&
                        usage_min <= 0xffffu && usage_max <= 0xffffu;
                    const int supported_variable = variable_absolute &&
                        ((page == 9 && usage >= 1) ||
                         (page == 1 &&
                          ((usage >= 0x30 && usage <= 0x38) ||
                           usage == 0x39 ||
                           (usage >= 0x90 && usage <= 0x93))) ||
                         (page == 2 &&
                          (usage == 0xba || usage == 0xbb ||
                           usage == 0xc4 || usage == 0xc5 ||
                           usage == 0xc8)));
                    /* Data, absolute game controls only. Variable controls and
                     * bounded Button-page selector arrays are both supported;
                     * padding, relative axes, and vendor data remain ignored. */
                    /* Anything no standard usage claimed is kept for the
                     * positional fallback: an unmapped pad must still produce
                     * input for setup, not look like a dead device. */
                    const int claimed = depth && collections[depth - 1] &&
                        (supported_variable || button_array);
                    if (!claimed && variable_absolute && g.bits >= 1 &&
                        g.bits <= 32 && g.max > g.min &&
                        layout->raw_count < GC_HID_MAX_FIELDS) {
                        layout->raw_fields[layout->raw_count++] = (GcHidField){
                            .offset = (uint16_t)(offsets[g.report] + i * g.bits),
                            .page = (uint16_t)page,
                            .usage = (uint16_t)usage,
                            .bits = (uint8_t)g.bits,
                            .report = (uint8_t)g.report,
                            .minimum = g.min,
                            .maximum = g.max,
                            .null_state = (uint8_t)((value & 0x40u) != 0)
                        };
                    }
                    if (claimed) {
                        if (layout->count == GC_HID_MAX_FIELDS || g.max <= g.min) return -1;
                        GcHidField *f = &layout->fields[layout->count++];
                        *f = (GcHidField){
                             .offset = (uint16_t)(offsets[g.report] +
                                                  i * g.bits),
                            .array_base = (uint16_t)offsets[g.report],
                            .page = (uint16_t)page,
                            .usage = (uint16_t)usage,
                            .bits = (uint8_t)g.bits,
                            .report = (uint8_t)g.report,
                            .minimum = g.min,
                            .maximum = g.max,
                            .null_state = (uint8_t)((value & 0x40u) != 0)
                        };
                        if (button_array) {
                            f->page = 9;
                            f->usage = 0;
                            f->array_usage_min = (uint16_t)usage_min;
                            f->array_usage_max = (uint16_t)usage_max;
                            f->is_array = 1;
                        }
                        if (page == 1 && usage >= 0x30 && usage <= 0x35)
                            layout->axes |= 1u << (usage - 0x30);
                        if (page == 2 && (usage == 0xc5))
                            layout->features |= GC_HID_FEATURE_L2_ANALOG;
                        if (page == 2 &&
                            (usage == 0xbb || usage == 0xc4))
                            layout->features |= GC_HID_FEATURE_R2_ANALOG;
                    }
                }
                offsets[g.report] += g.bits * g.count;
            }
            usage_count = usage_min = usage_max = 0;
        }
    }
    return depth || stack_size || (!layout->count && !layout->raw_count) ? -1 : 0;
}

/* Positional mapping for a report that declared no standard usages.
 *
 * Order decides, which is the only information such a descriptor gives: single
 * bit fields are buttons in canonical order, a small 2-4 bit range is a hat, and
 * wide fields are the left stick, right stick, then the triggers. Everything is
 * scaled through the field's declared range. This is deliberately a starting
 * point rather than a guess at the truth: it makes an unmapped pad produce input
 * that the setup wizard can remap into a custom profile, instead of the device
 * reporting no input at all and looking broken. */
static int gcid_generic_input(const GcHidLayout *layout, unsigned report,
                              const uint8_t *data, size_t size, ScePadData *pad)
{
    int found = 0;
    unsigned wide = 0, buttons = 0;
    for (size_t i = 0; i < layout->raw_count; ++i) {
        const GcHidField *f = &layout->raw_fields[i];
        if (f->report != report) continue;
        if ((size_t)f->offset + f->bits > size * 8) return 0;
        uint32_t raw = 0;
        for (unsigned bit = 0; bit < f->bits; ++bit)
            raw |= (uint32_t)((data[(f->offset + bit) / 8] >>
                               ((f->offset + bit) % 8)) & 1u) << bit;
        if (f->minimum < 0 && f->bits < 32 && (raw & (1u << (f->bits - 1))))
            raw |= UINT32_MAX << f->bits;
        const int64_t value = f->minimum < 0 ? (int64_t)(int32_t)raw : (int64_t)raw;
        const int64_t span = (int64_t)f->maximum - f->minimum;
        found = 1;
        if (f->bits == 1) {
            if (buttons < HID_CANONICAL_BUTTONS) {
                const uint32_t mask = gc_button_mask(hid_buttons[buttons]);
                pad->buttons = (pad->buttons & ~mask) |
                               (value && !(f->null_state && value < f->minimum) ? mask : 0);
            }
            ++buttons;
            continue;
        }
        if (f->bits <= 4 && span >= 1 && span <= 15) {
            pad->buttons |= hid_hat_mask(f->minimum, f->maximum, (int32_t)value);
            continue;
        }
        if (f->bits < 8 || span <= 0) continue;
        int64_t offset = value - f->minimum;
        if (offset < 0) offset = 0;
        if (offset > span) offset = span;
        const uint8_t scaled = (uint8_t)((offset * 255 + span / 2) / span);
        switch (wide) {
        case 0: pad->leftStick.x = scaled; break;
        case 1: pad->leftStick.y = scaled; break;
        case 2: pad->rightStick.x = scaled; break;
        case 3: pad->rightStick.y = scaled; break;
        case 4: pad->analogButtons.l2 = scaled; break;
        case 5: pad->analogButtons.r2 = scaled; break;
        default: break;
        }
        ++wide;
    }
    return found;
}

int gc_hid_input(const GcHidLayout *layout, const uint8_t *data, size_t size, ScePadData *pad)
{
    if (!layout || !data || !pad || !size ||
        layout->count > GC_HID_MAX_FIELDS)
        return 0;
    unsigned report = layout->numbered ? *data++ : 0;
    if (layout->numbered) --size;
    if (!layout->count) {
        /* No standard usage claimed anything: a vendor-defined report. Map it by
         * descriptor order so the pad produces input for setup and profiles. */
        return gcid_generic_input(layout, report, data, size, pad);
    }
    int found = 0;
    /* Validate the complete report before changing cached state. */
    for (size_t i = 0; i < layout->count; ++i) {
        const GcHidField *f = &layout->fields[i];
        if (!f->bits || f->bits > 32) return 0;
        if (f->report == report &&
            (size_t)f->offset + f->bits > size * 8)
            return 0;
    }
    uint32_t array_mask = 0;
    unsigned dpad_fields = 0;
    int has_hat = 0;
    for (size_t i = 0; i < layout->count; ++i) {
        const GcHidField *f = &layout->fields[i];
        if (f->report != report) continue;
        if (f->is_array) {
            const uint16_t last = f->array_usage_max < HID_CANONICAL_BUTTONS
                ? f->array_usage_max : (uint16_t)HID_CANONICAL_BUTTONS;
            for (uint16_t usage = f->array_usage_min;
                 usage <= last; ++usage)
                array_mask |= gc_button_mask(hid_buttons[usage - 1]);
        } else if (f->page == 1 && f->usage == 0x39) {
            has_hat = 1;
        } else if (f->page == 1 && f->usage >= 0x90 &&
                   f->usage <= 0x93) {
            dpad_fields |= 1u << (f->usage - 0x90);
        }
    }
    pad->buttons &= ~array_mask;
    const int complete_discrete_dpad = dpad_fields == 0x0fu;
    const int use_hat = has_hat && !complete_discrete_dpad;
    const int use_discrete_dpad = !has_hat || complete_discrete_dpad;
    if (use_hat || complete_discrete_dpad)
        pad->buttons &= ~(SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_DOWN |
                          SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT);
    for (size_t i = 0; i < layout->count; ++i) {
        const GcHidField *f = &layout->fields[i];
        if (f->report != report) continue;
        found = 1;
        uint32_t raw = 0;
        for (unsigned bit = 0; bit < f->bits; ++bit)
            raw |= (uint32_t)((data[(f->offset + bit) / 8] >> ((f->offset + bit) % 8)) & 1u) << bit;
        if (f->minimum < 0 && f->bits < 32 && (raw & (1u << (f->bits - 1))))
            raw |= UINT32_MAX << f->bits;
        const int64_t value = f->minimum < 0 ? (int32_t)raw : (int64_t)raw;
        const int is_null = f->null_state &&
                            (value < f->minimum || value > f->maximum);
        if (f->is_array) {
            uint16_t usage = 0;
            if (hid_array_usage(f, (int32_t)value, is_null, &usage) &&
                usage <= HID_CANONICAL_BUTTONS)
                pad->buttons |= gc_button_mask(hid_buttons[usage - 1]);
        } else if (f->page == 9) {
            if (f->usage <= HID_CANONICAL_BUTTONS) {
                const uint32_t mask = gc_button_mask(hid_buttons[f->usage - 1]);
                pad->buttons = (pad->buttons & ~mask) |
                               (!is_null && value ? mask : 0);
            }
        } else if (f->page == 1 && f->usage == 0x39) {
            if (!use_hat) continue;
            if (!is_null)
                pad->buttons |= hid_hat_mask(f->minimum, f->maximum,
                                             (int32_t)value);
        } else if (f->page == 1 && f->usage >= 0x90 &&
                   f->usage <= 0x93) {
            if (!use_discrete_dpad) continue;
            static const uint32_t dpad[] = {
                SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_DOWN,
                SCE_PAD_BUTTON_RIGHT, SCE_PAD_BUTTON_LEFT
            };
            const uint32_t mask = dpad[f->usage - 0x90];
            pad->buttons = (pad->buttons & ~mask) |
                           (!is_null && value ? mask : 0);
        } else {
            const int four_stick_axes =
                (layout->axes & 0x18u) == 0x18u; /* Rx and Ry */
            const int combined_trigger = four_stick_axes &&
                (layout->axes & 0x04u) && !(layout->axes & 0x20u);
            if (is_null) {
                if (f->page == 2) {
                    switch (f->usage) {
                    case 0xba: case 0xc8:
                        pad->leftStick.x = 128;
                        break;
                    case 0xbb: case 0xc4:
                        pad->analogButtons.r2 = 0;
                        break;
                    case 0xc5:
                        pad->analogButtons.l2 = 0;
                        break;
                    default: break;
                    }
                } else {
                    switch (f->usage) {
                    case 0x30: pad->leftStick.x = 128; break;
                    case 0x31: pad->leftStick.y = 128; break;
                    case 0x33: pad->rightStick.x = 128; break;
                    case 0x34: pad->rightStick.y = 128; break;
                    case 0x32:
                        if (combined_trigger) {
                            pad->analogButtons.l2 = 0;
                            pad->analogButtons.r2 = 0;
                        } else if (four_stick_axes) {
                            pad->analogButtons.l2 = 0;
                        } else {
                            pad->rightStick.x = 128;
                        }
                        break;
                    case 0x35:
                        if (four_stick_axes)
                            pad->analogButtons.r2 = 0;
                        else
                            pad->rightStick.y = 128;
                        break;
                    default: break;
                    }
                }
                continue;
            }
            const int64_t range = (int64_t)f->maximum - f->minimum;
            int64_t bounded = value;
            if (bounded < f->minimum) bounded = f->minimum;
            if (bounded > f->maximum) bounded = f->maximum;
            /* Round, rather than floor, so zero in a symmetric signed range
             * maps to the PlayStation neutral value 128 instead of 127. */
            const int64_t scaled = ((bounded - f->minimum) * 255 + range / 2) /
                                   range;
            const uint8_t axis = (uint8_t)scaled;
            if (f->page == 2) {
                switch (f->usage) {
                case 0xba: case 0xc8:
                    pad->leftStick.x = axis;
                    break;
                case 0xbb: case 0xc4:
                    pad->analogButtons.r2 = axis;
                    break;
                case 0xc5:
                    pad->analogButtons.l2 = axis;
                    break;
                default: break;
                }
                continue;
            }
            switch (f->usage) {
            case 0x30: pad->leftStick.x = axis; break;
            case 0x31: pad->leftStick.y = axis; break;
            case 0x33: pad->rightStick.x = axis; break;
            case 0x34: pad->rightStick.y = axis; break;
            case 0x32:
                if (combined_trigger) {
                    /* Legacy DirectInput commonly folds both triggers onto a
                     * centered Z axis.  A small midpoint deadband prevents a
                     * neutral 127/128 report from looking permanently held. */
                    pad->analogButtons.l2 = axis < 124u ?
                        (uint8_t)(((123u - axis) * 255u + 61u) / 123u) : 0;
                    pad->analogButtons.r2 = axis > 132u ?
                        (uint8_t)(((axis - 133u) * 255u + 61u) / 122u) : 0;
                } else if (four_stick_axes) pad->analogButtons.l2 = axis;
                else pad->rightStick.x = axis;
                break;
            case 0x35:
                if (four_stick_axes) pad->analogButtons.r2 = axis;
                else pad->rightStick.y = axis;
                break;
            default: break;
            }
        }
    }
    if (!found) return 0;
    const int four_stick_axes = (layout->axes & 0x18u) == 0x18u;
    const int combined_trigger = four_stick_axes &&
        (layout->axes & 0x04u) && !(layout->axes & 0x20u);
    if ((!four_stick_axes || !(layout->axes & 0x04u)) &&
        !(layout->features & GC_HID_FEATURE_L2_ANALOG))
        pad->analogButtons.l2 = (pad->buttons & SCE_PAD_BUTTON_L2) ? 255 : 0;
    if ((!four_stick_axes || !(layout->axes & 0x20u)) && !combined_trigger &&
        !(layout->features & GC_HID_FEATURE_R2_ANALOG))
        pad->analogButtons.r2 = (pad->buttons & SCE_PAD_BUTTON_R2) ? 255 : 0;
    return found;
}

int gc_hid_source_equal(const GcHidSource *left, const GcHidSource *right)
{
    return left && right && left->report == right->report &&
        left->offset == right->offset && left->bits == right->bits &&
        left->page == right->page && left->usage == right->usage &&
        left->minimum == right->minimum && left->maximum == right->maximum;
}

static int hid_field_value(const GcHidField *field, const uint8_t *data,
                           size_t size, int32_t *value, int *is_null)
{
    if (!field || !data || !value || !is_null || !field->bits ||
        field->bits > 32 || (size_t)field->offset + field->bits > size * 8)
        return -1;
    uint32_t raw = 0;
    for (unsigned bit = 0; bit < field->bits; ++bit)
        raw |= (uint32_t)((data[(field->offset + bit) / 8] >>
                 ((field->offset + bit) % 8)) & 1u) << bit;
    if (field->minimum < 0 && field->bits < 32 &&
        (raw & (1u << (field->bits - 1))))
        raw |= UINT32_MAX << field->bits;
    int64_t decoded = field->minimum < 0 ? (int32_t)raw : (int64_t)raw;
    if (decoded < INT32_MIN || decoded > INT32_MAX)
        return -1;
    *value = (int32_t)decoded;
    *is_null = field->null_state &&
        (decoded < field->minimum || decoded > field->maximum);
    return 0;
}

int gc_hid_values(const GcHidLayout *layout, const uint8_t *data, size_t size,
                  GcHidValue *values, size_t capacity, size_t *count)
{
    if (!layout || !data || !values || !count || !size ||
        layout->count > GC_HID_MAX_FIELDS)
        return 0;
    unsigned report = layout->numbered ? *data++ : 0;
    if (layout->numbered)
        --size;
    unsigned dpad_fields = 0;
    int has_hat = 0;
    for (size_t index = 0; index < layout->count; ++index) {
        const GcHidField *field = &layout->fields[index];
        if (field->report != report || field->is_array)
            continue;
        if (field->page == 1 && field->usage == 0x39)
            has_hat = 1;
        else if (field->page == 1 && field->usage >= 0x90 &&
                 field->usage <= 0x93)
            dpad_fields |= 1u << (field->usage - 0x90);
    }
    const int complete_discrete_dpad = dpad_fields == 0x0fu;
    size_t used = 0;
    for (size_t index = 0; index < layout->count; ++index) {
        const GcHidField *field = &layout->fields[index];
        if (field->report != report)
            continue;
        if ((field->page == 1 && field->usage == 0x39 &&
             complete_discrete_dpad) ||
            (field->page == 1 && field->usage >= 0x90 &&
             field->usage <= 0x93 && has_hat && !complete_discrete_dpad))
            continue;
        if (used == capacity)
            return 0;
        int32_t value;
        int is_null;
        if (hid_field_value(field, data, size, &value, &is_null) != 0)
            return 0;
        GcHidValue *item = &values[used++];
        memset(item, 0, sizeof(*item));
        item->source = (GcHidSource){
            .offset = field->is_array ? field->array_base : field->offset,
            .page = field->page,
            .usage = field->usage,
            .bits = field->bits,
            .report = field->report,
            .minimum = field->minimum,
            .maximum = field->maximum,
        };
        item->field_offset = field->offset;
        item->value = value;
        item->is_null = is_null;
        if (field->is_array) {
            uint16_t usage = 0;
            if (hid_array_usage(field, value, is_null, &usage)) {
                item->source.usage = usage;
                item->active = 1;
            }
        } else if (field->page == 9) {
            item->active = !is_null && value != 0;
        } else if (field->page == 1 && field->usage == 0x39) {
            item->active = !is_null &&
                hid_hat_mask(field->minimum, field->maximum, value) != 0;
        } else if (field->page == 1 && field->usage >= 0x90 &&
                   field->usage <= 0x93) {
            item->active = !is_null && value != 0;
        }
    }
    *count = used;
    return used != 0;
}

static int hid_same_coordinate(const GcHidValue *left,
                               const GcHidValue *right)
{
    return left->field_offset == right->field_offset &&
        left->source.report == right->source.report &&
        left->source.bits == right->source.bits &&
        left->source.page == right->source.page &&
        left->source.minimum == right->source.minimum &&
        left->source.maximum == right->source.maximum;
}

int gc_hid_state_update(GcHidState *state, const GcHidLayout *layout,
                        const uint8_t *data, size_t size)
{
    if (!state)
        return 0;
    GcHidValue current[GC_HID_MAX_FIELDS];
    size_t count = 0;
    if (!gc_hid_values(layout, data, size, current, GC_HID_MAX_FIELDS,
                       &count))
        return 0;
    for (size_t index = 0; index < count; ++index) {
        size_t stored = 0;
        while (stored < state->count &&
               !hid_same_coordinate(&state->values[stored],
                                    &current[index]))
            ++stored;
        if (stored == state->count) {
            if (state->count == GC_HID_MAX_FIELDS)
                return 0;
            ++state->count;
        }
        state->values[stored] = current[index];
    }
    return 1;
}

static uint8_t hid_bound_axis(const GcHidAxisBinding *binding, GcAxis target,
                              int32_t value)
{
    int64_t directed = (int64_t)binding->polarity *
                       ((int64_t)value - binding->center);
    int64_t positive = binding->polarity > 0
                           ? (int64_t)binding->source.maximum - binding->center
                           : (int64_t)binding->center - binding->source.minimum;
    if (target == GC_AXIS_L2 || target == GC_AXIS_R2) {
        if (directed <= 0 || positive <= 0) return 0;
        if (directed >= positive) return 255;
        return (uint8_t)((directed * 255 + positive / 2) / positive);
    }
    if (directed >= 0) {
        if (positive <= 0) return 128;
        if (directed >= positive) return 255;
        return (uint8_t)(128 + (directed * 127 + positive / 2) / positive);
    }
    int64_t negative = binding->polarity > 0
                           ? (int64_t)binding->center - binding->source.minimum
                           : (int64_t)binding->source.maximum - binding->center;
    directed = -directed;
    if (negative <= 0) return 128;
    if (directed >= negative) return 0;
    return (uint8_t)(128 - (directed * 128 + negative / 2) / negative);
}

static int hid_bound_button_active(const GcHidButtonBinding *binding,
                                   const GcHidValue *value)
{
    if (!gc_hid_source_equal(&value->source, &binding->source))
        return 0;
    if (binding->source.page == 1 && binding->source.usage == 0x39) {
        const uint32_t expected = hid_hat_mask(binding->source.minimum,
                                               binding->source.maximum,
                                               binding->active_value);
        const uint32_t current = hid_hat_mask(value->source.minimum,
                                              value->source.maximum,
                                              value->value);
        return expected && !(expected & (expected - 1)) &&
               (current & expected) != 0;
    }
    return value->active && value->value == binding->active_value;
}

static void reset_axis(ScePadData *pad, GcAxis axis)
{
    switch (axis) {
    case GC_AXIS_LX: pad->leftStick.x = 128; break;
    case GC_AXIS_LY: pad->leftStick.y = 128; break;
    case GC_AXIS_RX: pad->rightStick.x = 128; break;
    case GC_AXIS_RY: pad->rightStick.y = 128; break;
    case GC_AXIS_L2: pad->analogButtons.l2 = 0; break;
    case GC_AXIS_R2: pad->analogButtons.r2 = 0; break;
    default: break;
    }
}

static void clear_axis_source_fallback(const GcHidLayout *layout,
                                       ScePadData *pad,
                                       const GcHidSource *source,
                                       GcAxis target)
{
    if (source->page == 1) {
        switch (source->usage) {
        case 0x30: reset_axis(pad, GC_AXIS_LX); break;
        case 0x31: reset_axis(pad, GC_AXIS_LY); break;
        case 0x32: {
            const int four_stick_axes = layout &&
                (layout->axes & 0x18u) == 0x18u;
            const int combined_trigger = four_stick_axes &&
                (layout->axes & 0x04u) && !(layout->axes & 0x20u);
            if (combined_trigger) {
                if (target == GC_AXIS_L2 || target == GC_AXIS_R2)
                    reset_axis(pad, target);
                else {
                    reset_axis(pad, GC_AXIS_L2);
                    reset_axis(pad, GC_AXIS_R2);
                }
            } else if (four_stick_axes) {
                reset_axis(pad, GC_AXIS_L2);
            } else {
                reset_axis(pad, GC_AXIS_RX);
            }
            break;
        }
        case 0x33: reset_axis(pad, GC_AXIS_RX); break;
        case 0x34: reset_axis(pad, GC_AXIS_RY); break;
        case 0x35:
            reset_axis(pad, layout && (layout->axes & 0x18u) == 0x18u
                                ? GC_AXIS_R2 : GC_AXIS_RY);
            break;
        default: break;
        }
    } else if (source->page == 2) {
        switch (source->usage) {
        case 0xba: case 0xc8: reset_axis(pad, GC_AXIS_LX); break;
        case 0xbb: case 0xc4: reset_axis(pad, GC_AXIS_R2); break;
        case 0xc5: reset_axis(pad, GC_AXIS_L2); break;
        default: break;
        }
    }
}

void gc_hid_apply_profile_values(const GcHidLayout *layout,
                                 const GcHidValue *values, size_t count,
                                 const GcProfile *profile, ScePadData *pad)
{
    if (!values || !profile || !pad || profile->schema < GC_PROFILE_SCHEMA)
        return;
    uint32_t replaced = 0;
    for (int target = 0; target < GC_BUTTON_INPUT15; ++target) {
        const GcHidButtonBinding *binding = &profile->hid_buttons[target];
        if (!binding->present)
            continue;
        replaced |= gc_button_mask((GcButton)target);
        if (binding->source.page == 9 && binding->source.usage >= 1 &&
            binding->source.usage <= HID_CANONICAL_BUTTONS) {
            replaced |= gc_button_mask(hid_buttons[binding->source.usage - 1]);
        } else if (binding->source.page == 1 &&
                   binding->source.usage == 0x39) {
            replaced |= hid_hat_mask(binding->source.minimum,
                                     binding->source.maximum,
                                     binding->active_value);
        } else if (binding->source.page == 1 &&
                   binding->source.usage >= 0x90 &&
                   binding->source.usage <= 0x93) {
            static const uint32_t dpad[] = {
                SCE_PAD_BUTTON_UP, SCE_PAD_BUTTON_DOWN,
                SCE_PAD_BUTTON_RIGHT, SCE_PAD_BUTTON_LEFT,
            };
            replaced |= dpad[binding->source.usage - 0x90];
        }
    }
    pad->buttons &= ~replaced;
    for (int target = 0; target < GC_BUTTON_INPUT15; ++target) {
        const GcHidButtonBinding *binding = &profile->hid_buttons[target];
        if (!binding->present)
            continue;
        const uint32_t mask = gc_button_mask((GcButton)target);
        for (size_t index = 0; index < count; ++index)
            if (hid_bound_button_active(binding, &values[index])) {
                pad->buttons |= mask;
                break;
            }
    }
    for (int target = 0; target < GC_AXIS_COUNT; ++target) {
        const GcHidAxisBinding *binding = &profile->hid_axes[target];
        if (!binding->present)
            continue;
        reset_axis(pad, (GcAxis)target);
        clear_axis_source_fallback(layout, pad, &binding->source,
                                   (GcAxis)target);
    }
    for (int target = 0; target < GC_AXIS_COUNT; ++target) {
        const GcHidAxisBinding *binding = &profile->hid_axes[target];
        if (!binding->present)
            continue;
        for (size_t index = 0; index < count; ++index) {
            if (!gc_hid_source_equal(&values[index].source,
                                     &binding->source))
                continue;
            if (values[index].is_null)
                break;
            uint8_t output = hid_bound_axis(binding, (GcAxis)target,
                                            values[index].value);
            switch (target) {
            case GC_AXIS_LX: pad->leftStick.x = output; break;
            case GC_AXIS_LY: pad->leftStick.y = output; break;
            case GC_AXIS_RX: pad->rightStick.x = output; break;
            case GC_AXIS_RY: pad->rightStick.y = output; break;
            case GC_AXIS_L2: pad->analogButtons.l2 = output; break;
            case GC_AXIS_R2: pad->analogButtons.r2 = output; break;
            default: break;
            }
            break;
        }
    }
}

int gc_hid_input_profile(const GcHidLayout *layout, const uint8_t *data,
                         size_t size, GcHidState *state,
                         const GcProfile *profile, ScePadData *pad)
{
    if (!gc_hid_input(layout, data, size, pad))
        return 0;
    if (!profile || profile->schema < GC_PROFILE_SCHEMA)
        return 1;
    if (!gc_hid_state_update(state, layout, data, size))
        return 0;
    gc_hid_apply_profile_values(layout, state->values, state->count,
                                profile, pad);
    return 1;
}

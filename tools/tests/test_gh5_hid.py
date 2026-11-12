#!/usr/bin/env python3
"""Replay sdlemos's GH5 descriptor and test published PS3 guitar button mapping."""
from pathlib import Path
import json
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = json.loads((root / 'tools/tests/fixtures/gh5-hid.json').read_text())
arrays = 'static const uint8_t descriptor[] = {' + ','.join(str(x) for x in bytes.fromhex(fixture['descriptor'])) + '};\n'
for i, report in enumerate(fixture['reports']):
    arrays += 'static const uint8_t recorded%d[] = {' % i + ','.join(str(x) for x in bytes.fromhex(report['hex'])) + '};\n'
harness = r'''
#include <assert.h>
#include <string.h>
#include "gc_hid.h"
ARRAYS
int main(void) {
    GcHidLayout layout;
    assert(!gc_hid_parse_descriptor(descriptor, sizeof descriptor, &layout));
    assert(layout.count == 18 && layout.raw_count == 16 && !layout.numbered);
    gc_hid_set_device(&layout, 0x12ba, 0x0100);
    ScePadData pad = {0};
    uint8_t report[27]; memcpy(report, recorded0, sizeof report);
    static const uint32_t masks[] = {
        SCE_PAD_BUTTON_SQUARE, SCE_PAD_BUTTON_CROSS, SCE_PAD_BUTTON_CIRCLE,
        SCE_PAD_BUTTON_TRIANGLE, SCE_PAD_BUTTON_L1, SCE_PAD_BUTTON_R1
    };
    for (unsigned bit = 0; bit < 6; ++bit) {
        report[0] = 1u << bit;
        assert(gc_hid_input(&layout, report, sizeof report, &pad));
        assert(pad.buttons == masks[bit]);
        report[0] = 0;
        assert(gc_hid_input(&layout, report, sizeof report, &pad) && !pad.buttons);
    }
    report[0] = 0x1f;
    assert(gc_hid_input(&layout, report, sizeof report, &pad));
    assert(pad.buttons == (SCE_PAD_BUTTON_SQUARE | SCE_PAD_BUTTON_CROSS |
        SCE_PAD_BUTTON_CIRCLE | SCE_PAD_BUTTON_TRIANGLE | SCE_PAD_BUTTON_L1));
    report[0] = 0; report[1] = 3;
    assert(gc_hid_input(&layout, report, sizeof report, &pad));
    assert(pad.buttons == (SCE_PAD_BUTTON_SHARE | SCE_PAD_BUTTON_OPTIONS));
    report[1] = 0; report[2] = 0;
    assert(gc_hid_input(&layout, report, sizeof report, &pad) && pad.buttons == SCE_PAD_BUTTON_UP);
    report[2] = 4;
    assert(gc_hid_input(&layout, report, sizeof report, &pad) && pad.buttons == SCE_PAD_BUTTON_DOWN);
    report[2] = 0x1f;
    assert(gc_hid_input(&layout, report, sizeof report, &pad) && !pad.buttons);
    assert(gc_hid_input(&layout, recorded0, sizeof recorded0, &pad) && !pad.buttons);
    /* Captured button changes are decoded against the published mapping,
     * not asserted to correspond to a time-labelled physical tester action. */
    assert(gc_hid_input(&layout, recorded1, sizeof recorded1, &pad));
    uint32_t expected = 0;
    for (unsigned bit = 0; bit < 6; ++bit) if (recorded1[0] & (1u << bit)) expected |= masks[bit];
    assert((pad.buttons & (SCE_PAD_BUTTON_SQUARE | SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE |
        SCE_PAD_BUTTON_TRIANGLE | SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R1)) == expected);
    ScePadData saved = pad;
    assert(!gc_hid_input(&layout, report, 3, &pad) && !memcmp(&saved, &pad, sizeof pad));
    /* HID usages stay original; custom profiles can override the default. */
    assert(layout.fields[0].usage == 1 && layout.fields[1].usage == 2);
    GcProfile profile; gc_profile_defaults(&profile);
    profile.hid_buttons[GC_BUTTON_R2].present = 1;
    profile.hid_buttons[GC_BUTTON_R2].source.bits = 1;
    profile.hid_buttons[GC_BUTTON_R2].source.maximum = 1;
    profile.hid_buttons[GC_BUTTON_R2].active_value = 1;
    profile.hid_buttons[GC_BUTTON_R2].source.page = 9;
    profile.hid_buttons[GC_BUTTON_R2].source.usage = 1;
    GcHidState state = {0};
    report[0] = 1; report[1] = 0; report[2] = 0x0f;
    assert(gc_hid_input_profile(&layout, report, sizeof report, &state, &profile, &pad));
    assert((pad.buttons & SCE_PAD_BUTTON_R2) && !(pad.buttons & SCE_PAD_BUTTON_SQUARE));
    /* An unrelated receiver retains the generic button order; reset on reuse. */
    gc_hid_set_device(&layout, 0x12ba, 0x0200);
    assert(gc_hid_input(&layout, report, sizeof report, &pad));
    assert(pad.buttons == SCE_PAD_BUTTON_CROSS);
    gc_hid_set_device(&layout, 0x057e, 0x2009);
    assert(!layout.ps3_gh_guitar);
    return 0;
}
'''.replace('ARRAYS', arrays)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-gh5-hid-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
        '-fdata-sections', '-I', str(root / 'core/src'), str(work / 'test.c'),
        str(root / 'core/src/gc_hid.c'), str(root / 'core/src/gc_profile.c'),
        '-Wl,--gc-sections', '-lm', '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: captured GH5 HID layout, frets/chords/releases, strum, custom mapping and device isolation')

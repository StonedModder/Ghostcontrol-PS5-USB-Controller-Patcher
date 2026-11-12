#!/usr/bin/env python3
"""Compile the real vpad log parser on a host, without the PS5 runtime."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/bt/ps5_vpad.c').read_text()
# These adjacent pure helpers have no PS5 dependencies. Extract their actual definitions
# so the test cannot silently exercise a separate implementation of discovery.
parser = source.split('static uint64_t hex_after(', 1)[1]
assert 'static int log_source_read(' in parser
parser = 'static uint64_t hex_after(' + parser.split('static int log_source_read(', 1)[0]
harness = r'''
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
PARSER
int main(void) {
    assert(virtual_device_in("event=DEVICE_ADDED deviceId=0x1f030d deviceType=22 userId=0x0(0)") == 0x1f030d);
    assert(virtual_device_in("DEVICE_ADDED DeviceId:0x1234 type:1 subType:22 capabilityBattery:0") == 0x1234);
    assert(virtual_device_in("DEVICE_ADDED DeviceId:0x1234 type:1 subType:22 capabilityBattery:1") == 0);
    assert(virtual_device_in("DEVICE_ADDED deviceId=0x1234 deviceType=0x16") == 0x1234);
    assert(virtual_device_in("DEVICE_ADDED deviceId=0x1234 deviceType=022") == 0x1234);
    assert(virtual_device_in("DEVICE_ADDED deviceId=0x1234 deviceType=34") == 0);
    assert(virtual_device_in("DEVICE_ADDED deviceId=0x1234 deviceType=0x22") == 0);
    assert(virtual_device_in("DEVICE_ADDED deviceId=0x1234 deviceType=220") == 0);
    assert(virtual_device_in("DEVICE_ADDED deviceId=0x1234 deviceType=22junk") == 0);
    assert(virtual_device_in("DEVICE_ADDED deviceId=0x1234 deviceType=") == 0);
    assert(virtual_device_in("DEVICE_ADDED deviceType=22") == 0);
    assert(virtual_device_in("DEVICE_REMOVED deviceId=0x1234 deviceType=22") == 0);
    assert(virtual_device_in("") == 0);
    return 0;
}
'''.replace('PARSER', parser)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-vpad-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(work / 'test.c'),
        '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: virtual_device_in: current and legacy logs, numeric bases, invalid types and missing fields')

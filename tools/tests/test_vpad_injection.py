#!/usr/bin/env python3
"""Exercise the production InsertData path with host ABI assertions and fake API."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/bt/ps5_vpad.c').read_text()
abi = source[source.index('typedef struct {'):source.index('/* ---- system interfaces')]
slots = source[source.index('enum { VP_FREE'):source.index('static mbus_bind_fn')]
constants = '\n'.join(re.findall(r'^#define (?:ACCEL_PER_G|GYRO_PER_DEG_S|DEG_TO_RAD)\s+.+$',source,re.M))
block = source[source.index('int vpad_live('):source.index('/* Removes only what')]
harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pad.h"
#define HOST_MAX_PADS 1
ABI
SLOTS
CONSTANTS
static vslot g_slot[1];
static int g_user=42, result, calls, confirmations, recoveries;
static ScePadData captured;
static void log_line(const char *fmt, ...) {
    if (strstr(fmt,"first InsertData accepted")) ++confirmations;
    if (strstr(fmt,"InsertData recovered")) ++recoveries;
}
static int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data) {
    assert(handle==123); captured=*(const ScePadData *)data; ++calls; return result;
}
BLOCK
int main(void) {
    pad_state st={.buttons=PAD_CROSS|PAD_L1|PAD_UP,.lx=128,.ly=128,.rx=255,.ry=128};
    vpad_update(0,&st); vpad_update(-1,&st); assert(!calls);
    g_slot[0].st=VP_READY; g_slot[0].handle=123; g_slot[0].dev=0x7030d;
    result=-1; vpad_update(0,&st);
    assert(calls==1 && !confirmations && g_slot[0].reject_count==1 && vpad_live(0));
    result=0; vpad_update(0,&st);
    assert(calls==2 && confirmations==1 && recoveries==1 && !g_slot[0].reject_count);
    assert(captured.buttons==st.buttons && captured.lx==128 && captured.rx==255);
    assert(captured.connected && captured.connectedCount==1 && captured.timestamp);
    assert(captured.orientation[3]==1.0f && captured.acceleration[1]==-1.0f);
    st.buttons=0; st.rx=128; vpad_update(0,&st);
    assert(!captured.buttons && captured.rx==128 && confirmations==1);
    result=-1; vpad_update(0,&st); result=0; vpad_update(0,&st);
    assert(confirmations==1 && recoveries==2); /* No repeated success spam. */
    memset(g_slot,0,sizeof g_slot);
    g_slot[0].st=VP_READY; g_slot[0].handle=123;
    vpad_update(0,&st); assert(confirmations==2); /* Replacement has its own proof. */
    return 0;
}
'''.replace('ABI',abi).replace('SLOTS',slots).replace('CONSTANTS',constants).replace('BLOCK',block)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-vpad-inject-') as directory:
    work=Path(directory); (work/'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
        '-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',
        '-I'+str(root/'core/src/bt'),str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: production input injection ABI, press/release, refusal recovery and per-device confirmation')

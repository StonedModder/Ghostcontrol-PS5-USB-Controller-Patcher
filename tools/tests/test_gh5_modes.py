#!/usr/bin/env python3
"""Compile production GH5 dispatch in all modes; verify explicit publication."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'core/src/gc_service_inject.c').read_text()
wrapper=source[source.index('static int usb_decode(UsbFeed'):source.index('/* Switch Pro and compatibles:')]
harness=r'''
#include <assert.h>
#include <string.h>
#include "bt/pad.h"
#include "gc_gh5_mode.h"
#include "gc_usb_progress.h"
typedef struct {uint16_t vid,pid;} GcUsbDevice;
typedef struct {uint8_t report[64];uint32_t lenp;} UsbFeed;
static int calls,accepted=1;
static int usb_decode_report(UsbFeed *feed,const GcUsbDevice *device,pad_state *state,
                             const uint8_t *data,uint32_t length,int interrupt) {
    (void)feed;(void)device;(void)data;assert(length==27 && interrupt);
    ++calls;if(accepted) {pad_state_reset(state);state->buttons=PAD_CROSS;} return accepted;
}
WRAPPER
int main(void) {
    UsbFeed feed={.lenp=27};GcUsbDevice d={.vid=0x12ba,.pid=0x0100};
    pad_state state;memset(&state,0xa5,sizeof state);
    GcUsbProgress p={0};int result=usb_decode(&feed,&d,&state);
#if GC_GH5_MODE == GC_GH5_RAW
    assert(result==GC_USB_DECODE_SKIPPED && !calls && !gc_gh5_allows_fallback(d.vid,d.pid));
    assert(!gc_usb_progress_decode(&p,result) && p.skipped==1 && !p.decode_attempts);
#elif GC_GH5_MODE == GC_GH5_NEUTRAL
    assert(result==GC_USB_DECODE_NEUTRAL_PROBE && !calls && !gc_gh5_allows_fallback(d.vid,d.pid));
    assert(gc_usb_progress_decode(&p,result) && p.neutral_probes==1 && !p.decode_attempts);
    assert(!state.buttons && state.lx==128 && state.rx==128 && !state.l2);
#else
    assert(result==GC_USB_DECODE_CONTROLS && calls==1 && state.buttons==PAD_CROSS);
    assert(gc_usb_progress_decode(&p,result) && p.decoded==1 && p.decode_attempts==1);
    assert(gc_gh5_allows_fallback(d.vid,d.pid));
    accepted=0;assert(!gc_usb_progress_decode(&p,usb_decode(&feed,&d,&state)));
    assert(p.rejected==1 && p.decode_attempts==2);accepted=1;
#endif
    calls=0;d.pid=0x0200;
    assert(usb_decode(&feed,&d,&state)==GC_USB_DECODE_CONTROLS && calls==1);
    assert(gc_gh5_allows_fallback(d.vid,d.pid));
    return 0;
}
'''.replace('WRAPPER',wrapper)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-gh5-modes-') as directory:
    work=Path(directory);(work/'test.c').write_text(harness)
    for mode in [1,2,3]:
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
            '-std=c11','-Wall','-Wextra','-Werror','-DGC_GH5_MODE='+str(mode),
            '-I'+str(root/'core/src'),str(work/'test.c'),'-o',str(work/'test')],check=True)
        subprocess.run([str(work/'test')],check=True)
# Expand real Makefile variants without compiling/linking/deploying any app.
with tempfile.TemporaryDirectory(prefix='ghostcontrol-make-mode-') as directory:
    sdk=Path(directory);(sdk/'toolchain').mkdir()
    (sdk/'toolchain/prospero.mk').write_text('CC := clang\n')
    for name,mode in [('gh5-raw',1),('gh5-neutral',2),('gh5-mapped',3)]:
        result=subprocess.check_output(['make','--no-print-directory','-n','-C',str(root/'GhosttoothAPI'),
            'PS5_PAYLOAD_SDK='+str(sdk),'VARIANT='+name],text=True)
        assert '-DGC_GH5_MODE='+str(mode) in result
        assert 'gc_service_inject.c' in result and 'bt/ps5_vpad.c' in result
        assert 'GhosttoothAPI-'+name+'.elf' in result
        assert 'GC_BUILD_REVISION=' in result and 'GC_BUILD_VARIANT=' in result
print('PASS: GH5 raw/neutral/mapped dispatch, decode accounting, fallback and unrelated-device isolation')

#!/usr/bin/env python3
"""Validate USB progress observation with simulated healthy/quiet/recovery traces."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'core/src/gc_service_inject.c').read_text()
logger=source[source.index('static void usb_progress_log('):source.index('static int usb_release_fd(')]
harness=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "gc_usb_fallback.h"
#include "gc_usb_progress.h"
static uint64_t clock_ms;
static long now_ms(void) {return (long)clock_ms;}
typedef struct {
    uint16_t vid,pid; int fd,interface_number,busy; uint8_t in_ep; char path[32];
    GcUsbProgress progress; GcUsbFallback fallback;
} UsbFeed;
static unsigned log_calls;
static char logged[2048];
static void log_line(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void log_line(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vsnprintf(logged,sizeof logged,fmt,ap); va_end(ap);
    ++log_calls;
}
LOGGER
int main(void) {
    GcUsbProgress p={.opened_ms=100,.generation=1};
    assert(gc_usb_progress_due(&p,100));
    assert(!gc_usb_progress_due(&p,1099));
    assert(gc_usb_progress_due(&p,1100));
    /* Many busy returns do not indicate stopped progress. */
    for (uint64_t now=1200;now<4200;now+=18) {
        gc_usb_progress_start(&p,now); ++p.busy_returns;
        assert(!gc_usb_progress_suspect(&p,now+8));
        gc_usb_progress_complete(&p,now+16);
        gc_usb_progress_input(&p,now+16);
    }
    assert(p.busy_returns>100 && p.completions==p.inputs);
    /* An outstanding transfer with no progress is only a diagnostic suspect. */
    gc_usb_progress_start(&p,5000);
    assert(!gc_usb_progress_suspect(&p,5999));
    assert(gc_usb_progress_suspect(&p,6000) && p.outstanding);
    gc_usb_progress_complete(&p,6010);
    gc_usb_progress_input(&p,6010);
    assert(!gc_usb_progress_suspect(&p,6010));
    /* A connected feed with no outstanding request is not a transfer stall. */
    assert(!gc_usb_progress_suspect(&p,10000));
    assert(gc_usb_progress_age(10,20)==0); /* No unsigned underflow. */
    memset(&p,0,sizeof p); p.generation=2; p.opened_ms=20000;
    assert(!p.starts && !p.completions && !p.busy_returns && !p.have_input);
    gc_usb_progress_start(&p,20000);
    assert(!gc_usb_progress_suspect(&p,20999));
    assert(gc_usb_progress_suspect(&p,21000));
    UsbFeed feed={.vid=0x12ba,.pid=0x0100,.fd=12,.in_ep=0x81,.busy=1};
    feed.progress.generation=7; strcpy(feed.path,"/dev/ugen2.2");
    gc_usb_progress_start(&feed.progress,0);
    clock_ms=1000; usb_progress_log(&feed);
    assert(log_calls==1 && strstr(logged,"possible-stall") && strstr(logged,"endpoint_address=0x81"));
    assert(strstr(logged,"transfer_index=0") && strstr(logged,"generation=7"));
    assert(strstr(logged,"since_input_ms=-1"));
    for(clock_ms=1001;clock_ms<2000;++clock_ms) usb_progress_log(&feed);
    assert(log_calls==1);
    gc_usb_progress_complete(&feed.progress,2000);
    gc_usb_progress_input(&feed.progress,2000); feed.busy=0;
    usb_progress_log(&feed);
    assert(log_calls==2 && strstr(logged,"progress-resumed") && strstr(logged,"since_input_ms=0"));
    feed.pid=0x0200; clock_ms=4000; usb_progress_log(&feed);
    assert(log_calls==2); /* No extra logging for unrelated devices. */
    return 0;
}
'''.replace('LOGGER',logger)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-progress-') as directory:
    work=Path(directory); (work/'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
        '-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'core/src'),
        str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: healthy EBUSY progress, pending-age observation, recovery, sample bounds and reconnect reset')

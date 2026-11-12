#!/usr/bin/env python3
"""Exercise production completion ownership with fake USB completions."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/gc_service_inject.c').read_text()
block = source[source.index('        memset(&done, 0, sizeof done);'):source.index('        gc_usb_fallback_interrupt(&feed->fallback,')]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#define USB_FS_COMPLETE 1
struct usb_fs_complete { unsigned ep_index; };
typedef struct {
    int fd, busy; char path[32]; uint32_t lenp; uint8_t report[64];
    struct { int status; unsigned aFrames; } ep[2];
} Feed;
static int complete_errno, notes, status_seen, short_seen;
static unsigned completed_index;
static void log_line(const char *format, ...) { (void)format; }
static int ioctl(int fd, int command, struct usb_fs_complete *done) {
    (void)fd; assert(command == USB_FS_COMPLETE);
    if (complete_errno) { errno = complete_errno; return -1; }
    done->ep_index = completed_index; return 0;
}
static int usb_note_io(Feed *feed, int err, int status, int short_invalid) {
    (void)feed; (void)err; ++notes; status_seen = status; short_seen = short_invalid;
    return 0;
}
static int complete(Feed *feed) {
    struct usb_fs_complete done;
    do {
BLOCK
        return 1; /* Valid IN input may now reach decode/publication. */
    } while (0);
    return 0;
}
int main(void) {
    Feed feed = {.busy=1, .lenp=27, .ep={{.aFrames=1}}};
    complete_errno = EBUSY;
    assert(!complete(&feed) && feed.busy && notes == 1);
    complete_errno = 0; completed_index = 1; notes = 0;
    /* OUT completion must not clear an outstanding IN transfer. */
    assert(!complete(&feed) && feed.busy && notes == 0);
    feed.ep[0].status = 99;
    assert(!complete(&feed) && feed.busy && notes == 0);
    completed_index = 0;
    assert(!complete(&feed) && !feed.busy && status_seen == 99);
    feed.ep[0].status = 0; feed.busy = 1; feed.lenp = 0;
    assert(!complete(&feed) && !feed.busy && short_seen == 1);
    feed.busy = 1; feed.lenp = 65;
    assert(!complete(&feed) && !feed.busy);
    feed.busy = 1; feed.lenp = 27; feed.ep[0].aFrames = 0;
    assert(!complete(&feed) && !feed.busy);
    feed.busy = 1; feed.ep[0].aFrames = 1;
    assert(complete(&feed) && !feed.busy);
    return 0;
}
'''.replace('BLOCK', block)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-completion-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(work / 'test.c'),
        '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: completion ownership, EBUSY retention, status and frame/length validation')

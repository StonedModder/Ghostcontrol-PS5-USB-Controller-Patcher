#!/usr/bin/env python3
"""Exercise expired-search cancellation without flooding an asynchronous radio."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/bt/host.c').read_text()
block = source[source.index('    if (now >= h->pair_until && h->inquiring) {'):source.index('\nint host_pair(')]
block = block[:block.rfind('}')]
complete = source[source.index('    case 0x0E:  /* Command Complete:'):source.index('    case 0x0F:  /* Command Status:')]
cancel_status_start = source.find('        if (len >= 6 && le16(ev + 4) == OP_INQUIRY_CANCEL')
cancel_status = '' if cancel_status_start < 0 else source[cancel_status_start:source.index('        if (len >= 6 && (le16(ev + 4) == 0x200D', cancel_status_start)]
helper = ''
if 'static void cancel_inquiry(host_t *h)\n' in source:
    helper = source[source.index('static void cancel_inquiry(host_t *h)\n'):source.index('static void start_inquiry(')]
harness = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define OP_INQUIRY_CANCEL 0x0402
#define T_INQUIRY_RETRY 500
static long clock_ms;
static long now_ms(void) { return clock_ms; }
typedef struct { long pair_until; int inquiring; unsigned cc_op; int cc_len, le_scan_pending, closing; unsigned char cc[8]; } host_t;
typedef struct { host_t host; long inquiry_cancel_at; } host_storage;
static unsigned cancels;
static int send_ok = 1;
static int send_cmd(host_t *h, unsigned op, const void *data, int length) {
    (void)h; (void)data; assert(op == OP_INQUIRY_CANCEL && !length);
    ++cancels; return send_ok;
}
HELPER
static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static void le_on_cmd_complete(host_t *h, unsigned op, const unsigned char *p, int n) {
    (void)h; (void)op; (void)p; (void)n;
}
static void command_complete(host_t *h, const unsigned char *ev, int len) {
    switch (ev[0]) {
COMPLETE
    default: break;
    }
}
static void command_status(host_t *h, const unsigned char *ev, int len) {
    (void)h; (void)ev; (void)len;
CANCEL_STATUS
}
static void expire(host_t *h) {
    long now = now_ms();
BLOCK
}
int main(void) {
    host_storage state = {.host={.pair_until=1000, .inquiring=1}};
    clock_ms = 999; expire(&state.host); assert(!cancels);
    for (clock_ms=1000; clock_ms<1500; ++clock_ms) expire(&state.host);
    assert(cancels == 1 && state.host.inquiring);
    clock_ms=1500; expire(&state.host); assert(cancels == 2);
    /* Missing answers and failed sends both retain bounded retries. */
    send_ok = 0;
    for (clock_ms=1501; clock_ms<=2500; ++clock_ms) expire(&state.host);
    assert(cancels == 4 && state.host.inquiring);
    /* An error does not establish that the active procedure ended. */
    unsigned char event[] = {0x0e, 4, 1, 0x02, 0x04, 0x0c};
    command_complete(&state.host, event, sizeof event);
    assert(state.host.inquiring);
    unsigned char status[] = {0x0f, 4, 0x0c, 1, 0x02, 0x04};
    command_status(&state.host, status, sizeof status);
    assert(state.host.inquiring);
    /* Natural Inquiry Complete may precede the cancel's refusal. */
    state.host.inquiring=0;
    command_complete(&state.host,event,sizeof event);
    assert(!state.host.inquiring);
    clock_ms=3000; expire(&state.host); assert(cancels == 4);
    /* A delayed old cancel cannot stop a fresh inquiry whose deadline reset. */
    state.host.inquiring=1; state.inquiry_cancel_at=0;
    command_complete(&state.host,event,sizeof event);
    assert(state.host.inquiring);
    event[5]=0; command_complete(&state.host,event,sizeof event);
    assert(state.host.inquiring);
    /* Unrelated failures keep the inquiry active and preserve retry timing. */
    state.inquiry_cancel_at=4000; event[5]=3;
    command_complete(&state.host,event,sizeof event);
    assert(state.host.inquiring && state.inquiry_cancel_at==4000);
    event[5]=0; command_complete(&state.host,event,sizeof event);
    assert(!state.host.inquiring && !state.inquiry_cancel_at);
    /* Synchronous shutdown is an explicit cancel even without a retry timer. */
    state.host.closing=state.host.inquiring=1;
    command_complete(&state.host,event,sizeof event);
    assert(!state.host.inquiring);
    return 0;
}
'''.replace('HELPER', helper).replace('BLOCK', block).replace('COMPLETE', complete).replace('CANCEL_STATUS', cancel_status)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-inquiry-cancel-') as directory:
    work=Path(directory); (work/'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
        '-std=c11','-Wall','-Wextra','-Werror',str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: expired inquiry cancellation is bounded across delayed replies and failed sends')

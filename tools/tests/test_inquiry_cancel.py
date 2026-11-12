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
helper = ''
if 'static void cancel_inquiry(host_t *h)\n' in source:
    helper = source[source.index('static void cancel_inquiry(host_t *h)\n'):source.index('static void start_inquiry(')]
harness = r'''
#include <assert.h>
#include <stddef.h>
#define OP_INQUIRY_CANCEL 0x0402
#define T_INQUIRY_RETRY 500
static long clock_ms;
static long now_ms(void) { return clock_ms; }
typedef struct { long pair_until; int inquiring; } host_t;
typedef struct { host_t host; long inquiry_cancel_at; } host_storage;
static unsigned cancels;
static int send_ok = 1;
static int send_cmd(host_t *h, unsigned op, const void *data, int length) {
    (void)h; (void)data; assert(op == OP_INQUIRY_CANCEL && !length);
    ++cancels; return send_ok;
}
HELPER
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
    state.host.inquiring=0; clock_ms=3000; expire(&state.host);
    assert(cancels == 4);
    return 0;
}
'''.replace('HELPER', helper).replace('BLOCK', block)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-inquiry-cancel-') as directory:
    work=Path(directory); (work/'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
        '-std=c11','-Wall','-Wextra','-Werror',str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: expired inquiry cancellation is bounded across delayed replies and failed sends')

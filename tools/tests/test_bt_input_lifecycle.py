#!/usr/bin/env python3
"""Run the production Bluetooth publication path across disconnect boundaries."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/bt/host.c').read_text()
block = source[source.index('void link_input('):source.index('/* Ready callbacks and snapshots')]
harness = r'''
#include "host_int.h"
#include <assert.h>
#include <string.h>
typedef struct { host_t host; int qualified; } host_storage;
static unsigned parses, states, reports;
static int disconnect_on_state;
int slot_of(host_t *h, const link_t *l) { return (int)(l - h->links); }
static host_origin captured_origin(host_t *h, const link_t *l) {
    (void)h; (void)l; host_origin o = {0}; o.occurrence = 1; return o;
}
static int parse(void *ctx, const uint8_t *data, int n, pad_state *st) {
    (void)ctx; (void)st; assert(n == 1 && data[0] == 1); ++parses; return PARSE_OK;
}
static void state(void *ud, int slot, const pad_state *st) {
    host_t *h = ud; (void)st; ++states;
    if (disconnect_on_state) h->links[slot].disconnecting = 1;
}
static void report(void *ud, int slot, const uint8_t *data, int n) {
    (void)ud; (void)slot; (void)data; (void)n; ++reports;
}
static void qstate(void *ud, int slot, const host_origin *origin, const pad_state *st) {
    assert(origin->occurrence == 1); state(ud, slot, st);
}
static void qreport(void *ud, int slot, const host_origin *origin, const uint8_t *data, int n) {
    assert(origin->occurrence == 1); report(ud, slot, data, n);
}
BLOCK
int main(void) {
    const pad_profile profile = {.parse=parse};
    const uint8_t data[] = {1};
    for (int qualified=0; qualified<2; ++qualified) {
        host_storage storage = {0}; host_t *h = &storage.host;
        storage.qualified=qualified;
        h->ev.ud=h; h->ev.on_state=state; h->ev.on_report=report;
        h->ev.on_state_qualified=qstate; h->ev.on_report_qualified=qreport;
        link_t *l = &h->links[0]; l->prof=&profile; l->connected=l->ready=1;
        parses=states=reports=0;
        link_input(h,l,data,1); assert(parses==1 && states==1 && reports==1);
        parses=states=reports=0; l->disconnecting=1;
        link_input(h,l,data,1); assert(!parses && !states && !reports);
        l->disconnecting=0; l->connected=0;
        link_input(h,l,data,1); assert(!parses && !states && !reports);
        l->connected=1; disconnect_on_state=1;
        link_input(h,l,data,1); assert(parses==1 && states==1 && !reports);
        disconnect_on_state=0; l->disconnecting=0; h->closing=1;
        parses=states=reports=0;
        link_input(h,l,data,1); assert(!parses && !states && !reports);
        h->closing=0;
        link_input(h,l,data,0); assert(!parses && !states && !reports);
    }
    return 0;
}
'''.replace('BLOCK',block)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-bt-input-') as directory:
    work=Path(directory); (work/'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
        '-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'core/src/bt'),
        str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: Bluetooth input publication stops across disconnected, closing and callback teardown states')

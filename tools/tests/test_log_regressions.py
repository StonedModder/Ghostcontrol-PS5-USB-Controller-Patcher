#!/usr/bin/env python3
"""Run the production store and inquiry routines without PS5 hardware."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/bt/host.c').read_text()
store = source[source.index('static int db_make_parent('):source.index('static void db_load(')]
find = source[source.index('db_rec *db_find('):source.index('/* The store file:')]
load = source[source.index('static void db_load('):source.index('db_rec *db_put(')]
forget_start = source.rfind('\n', 0, source.index('db_forget(host_t')) + 1
forget = source[forget_start:source.index('/* ---- GATT cache:')]
identity = source[source.index('static const unsigned char *link_identity('):source.index('int host_paired_get(')]
host_forget = source[source.index('int host_forget('):source.index('/* ---- controller setup')]
start = source[source.index('static void start_inquiry('):source.index('void host_poll(')]
status = source.split('if (len >= 6 && le16(ev + 4) == OP_INQUIRY) {', 1)[1].split('\n        /* A refused Inquiry Cancel', 1)[0]
harness = r'''
#include "host_int.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char last_log[512];
void log_line(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, args); va_end(args);
}
const char *addr_str(const unsigned char *a) { (void)a; return "test-pad"; }
static int disconnects;
void link_disconnect(host_t *h, link_t *l, const char *why) {
    (void)h; (void)why; l->disconnecting = 1; ++disconnects;
}
static long clock_ms;
long now_ms(void) { return clock_ms; }
typedef struct { host_t host; long t_inquiry, inquiry_at, inquiry_cancel_at; int inquiry_backoff; } host_storage;
static int send_ok = 1;
int send_cmd(host_t *h, unsigned op, const void *data, int size) {
    (void)h; (void)op; (void)data; (void)size; return send_ok;
}
STORE
FIND
LOAD
FORGET
IDENTITY
HOST_FORGET
START
static void inquiry_status(host_t *h, unsigned char code) {
    unsigned char ev[6] = {0}; ev[2] = code;
    { STATUS
}
int main(void) {
    char work[] = "/tmp/ghostcontrol-store-XXXXXX";
    assert(mkdtemp(work));
    host_t h = {0};
    snprintf(h.db_path, sizeof h.db_path, "%s/service/bt/controllers.db", work);
    h.ndb = 1; h.db[0].addr[0] = 0x42; h.db[0].key[0] = 0xab;
    assert(db_save(&h));
    FILE *f = fopen(h.db_path, "rb"); assert(f);
    unsigned char bytes[8 + DB_REC];
    assert(fread(bytes, 1, sizeof bytes, f) == sizeof bytes);
    assert(fclose(f) == 0);
    assert(!memcmp(bytes, DB_MAGIC, 8)); assert(bytes[8] == 0x42 && bytes[14] == 0xab);
    assert(db_save(&h)); /* Existing directories and replacement. */

    /* Remove a middle record, preserving the others on disk and in memory. */
    h.ndb = 3; h.db[1].addr[0] = 0x43; h.db[2].addr[0] = 0x44;
    assert(db_save(&h));
    const unsigned char target[6] = {0x43}, absent[6] = {0x99};
    h.links[0].used = 1; h.links[0].le = 1;
    memcpy(h.links[0].le_s.id_addr, target, 6);
    h.links[1].used = 1; h.links[1].addr[0] = 0x44;
    assert(host_forget(&h, target));
    assert(disconnects == 1 && !h.links[1].disconnecting);
    assert(strstr(last_log, "pad test-pad: forgotten"));
    assert(h.ndb == 2 && h.db[0].addr[0] == 0x42 && h.db[1].addr[0] == 0x44);
    db_load(&h);
    assert(h.ndb == 2 && !db_find(&h, target));
    assert(!host_forget(&h, absent) && h.ndb == 2 && disconnects == 1);
    h.closing = 1;
    assert(!host_forget(&h, h.db[0].addr) && h.ndb == 2);
    h.closing = 0;

    /* A blocked rewrite leaves the old disk store: do not report success. */
    char pending[300]; snprintf(pending, sizeof pending, "%s.new", h.db_path);
    assert(mkdir(pending, 0700) == 0);
    const unsigned char kept[6] = {0x42};
    assert(!host_forget(&h, kept));
    assert(strstr(last_log, "forgotten in memory; store write failed"));
    assert(h.ndb == 1 && !db_find(&h, kept));
    db_load(&h);
    assert(h.ndb == 2 && db_find(&h, kept));
    assert(rmdir(pending) == 0);
    assert(host_forget(&h, kept)); /* Retry after storage is writable again. */
    db_load(&h);
    assert(h.ndb == 1 && h.db[0].addr[0] == 0x44);
    assert(unlink(h.db_path) == 0);
    char folder[300]; snprintf(folder, sizeof folder, "%s/service/bt", work); assert(rmdir(folder) == 0);
    snprintf(folder, sizeof folder, "%s/service", work); assert(rmdir(folder) == 0);
    assert(rmdir(work) == 0);
    strcpy(h.db_path, "/dev/null/controllers.db");
    assert(!db_save(&h)); /* Parent is a file: do not claim success. */
    host_storage state = {0};
    for (int i = 0; i < 8; ++i) {
        start_inquiry(&state.host);
        inquiry_status(&state.host, 0x0c);
        int expected = T_INQUIRY_RETRY << (i < 4 ? i : 4);
        assert(state.inquiry_backoff == expected);
        assert(!state.host.inquiring);
        assert(state.inquiry_at == clock_ms + expected);
        clock_ms = state.inquiry_at;
    }
    start_inquiry(&state.host);
    inquiry_status(&state.host, 0);
    assert(state.inquiry_backoff == 0 && state.host.inquiring);
    state = (host_storage){0};
    send_ok = 0;
    for (int i = 0; i < 8; ++i) {
        start_inquiry(&state.host);
        int expected = T_INQUIRY_RETRY << (i < 4 ? i : 4);
        assert(!state.host.inquiring);
        assert(state.inquiry_backoff == expected);
        assert(state.inquiry_at == clock_ms + expected);
        clock_ms = state.inquiry_at;
    }
    send_ok = 1;
    start_inquiry(&state.host);
    assert(state.host.inquiring && state.inquiry_backoff == T_INQUIRY_RETRY_MAX);
    inquiry_status(&state.host, 0);
    assert(state.inquiry_backoff == 0);
    return 0;
}
'''.replace('STORE', store).replace('FIND', find).replace('LOAD', load).replace('HOST_FORGET', host_forget).replace('FORGET', forget).replace('IDENTITY', identity).replace('START', start).replace('STATUS', status)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-tests-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-D_DEFAULT_SOURCE', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(root / 'core/src/bt'), str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: bond persistence; forget success/failure, restart and retry; inquiry retry growth, cap and reset')

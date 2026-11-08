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
start = source[source.index('static void start_inquiry('):source.index('void host_poll(')]
status = source.split('if (len >= 6 && le16(ev + 4) == OP_INQUIRY) {', 1)[1].split('\n        if (len >= 6 && le16(ev + 4) == OP_INQUIRY_CANCEL', 1)[0]
harness = r'''
#include "host_int.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
void log_line(const char *fmt, ...) { (void)fmt; }
static long clock_ms;
long now_ms(void) { return clock_ms; }
typedef struct { host_t host; long t_inquiry, inquiry_at; int inquiry_backoff; } host_storage;
int send_cmd(host_t *h, unsigned op, const void *data, int size) {
    (void)h; (void)op; (void)data; (void)size; return 1;
}
STORE
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
    return 0;
}
'''.replace('STORE', store).replace('START', start).replace('STATUS', status)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-tests-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-D_DEFAULT_SOURCE', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(root / 'core/src/bt'), str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: bond-store directory creation, replacement and failure; inquiry retry growth, cap and reset')

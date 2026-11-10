#!/usr/bin/env python3
"""Exercise the production kernel-log reader with bounded host I/O fakes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'core/src/bt/ps5_vpad.c').read_text()
globals_ = source[source.index('static int g_log_fd'):source.index('/* Raw sensor counts')]
reader = source[source.index('static int log_source_open('):source.index('/* ---- users and privileges')]
harness = r'''
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define LINE_MAX_LEN 1024
static int g_pending = 0, g_matches;
static uint64_t g_match_dev;
static long g_t_match;
static int fail_get, fail_set, fail_close, reads, closes, flags;
static const char *input;
static int outcome = EAGAIN;
static char logs[8192];
static long now_ms(void) { return 123; }
static void log_line(const char *format, ...) {
    va_list args; va_start(args, format);
    size_t used = strlen(logs);
    vsnprintf(logs + used, sizeof logs - used, format, args);
    va_end(args);
}
static int fake_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol; return 10;
}
static int fake_connect(int fd, const struct sockaddr *addr, socklen_t length) {
    (void)fd; (void)addr; (void)length; return 0;
}
static int fake_fcntl(int fd, int command, ...) {
    (void)fd;
    if (command == F_GETFL) { if (fail_get) { errno = EIO; return -1; } return 0; }
    assert(command == F_SETFL);
    if (fail_set) { errno = EIO; return -1; }
    va_list args; va_start(args, command); flags = va_arg(args, int); va_end(args);
    return 0;
}
static int fake_open(const char *path, int mode) {
    (void)path; (void)mode; errno = ENOENT; return -1;
}
static int fake_close(int fd) {
    assert(fd == 10); ++closes;
    if (fail_close) { errno = EIO; return -1; } return 0;
}
static ssize_t fake_read(int fd, void *buffer, size_t length) {
    assert(fd == 10); ++reads;
    if (input && *input) {
        size_t size = strlen(input); if (size > length) size = length;
        memcpy(buffer, input, size); input += size; return (ssize_t)size;
    }
    if (!outcome) return 0;
    errno = outcome; return -1;
}
#define socket fake_socket
#define connect fake_connect
#define fcntl fake_fcntl
#define open fake_open
#define close fake_close
#define read fake_read
GLOBALS
READER
int main(void) {
    fail_get = 1;
    assert(log_source_open() == -1); /* Must not return a blocking TCP socket. */
    assert(closes == 1);
    fail_get = 0; fail_set = 1;
    assert(log_source_open() == -1 && closes == 2);
    fail_set = 0;
    g_log_fd = log_source_open();
    assert(g_log_fd == 10 && (flags & O_NONBLOCK));
    input = "DEVICE_ADDED deviceId=0x1234 deviceType=22\n";
    log_source_read();
    assert(g_matches == 1 && g_match_dev == 0x1234 && g_t_match == 123);
    assert(reads == 2); /* One packet and EAGAIN: no busy loop. */
    assert(g_log_lines == 1 && g_log_events == 1 && g_log_bytes > 0);
    assert(!g_log_eof && !g_log_read_errno);
    log_identification_result(0);
    g_matches = 0; g_log_bytes = g_log_lines = g_log_events = 0;
    logs[0] = '\0';
    log_identification_result(0);
    assert(strstr(logs, "supplied no bytes"));
    outcome = EINTR; log_source_read();
    assert(!g_log_read_errno && !g_log_eof);
    outcome = EIO; log_source_read();
    assert(g_log_read_errno == EIO);
    logs[0] = '\0'; log_identification_result(0);
    assert(strstr(logs, "kernel log read failed"));
    g_log_read_errno = 0; outcome = 0;
    log_source_read(); assert(g_log_eof);
    logs[0] = '\0'; log_identification_result(0);
    assert(strstr(logs, "stream ended"));
    g_log_eof = 0; outcome = EAGAIN;
    input = "unrelated kernel event\n"; log_source_read();
    logs[0] = '\0'; log_identification_result(0);
    assert(strstr(logs, "no DEVICE_ADDED event"));
    input = "DEVICE_ADDED deviceId=0x1234 deviceType=34\n"; log_source_read();
    logs[0] = '\0'; log_identification_result(0);
    assert(strstr(logs, "no virtual DualSense matched"));
    /* Every outcome retains the descriptor; no fallback or guessing an owner. */
    assert(g_log_fd == 10 && closes == 2);
    log_source_close(); assert(g_log_fd == -1 && closes == 3);
    fail_get = fail_close = 1;
    assert(log_source_open() == -1 && g_log_close_uncertain);
    assert(log_source_open() == -1 && closes == 4); /* Never retry ambiguous close. */
    return 0;
}
'''.replace('GLOBALS', globals_).replace('READER', reader)
with tempfile.TemporaryDirectory(prefix='ghostcontrol-vpad-log-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(work / 'test.c'),
        '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: kernel-log nonblocking setup, bounded reads, EOF/error diagnostics and close ownership')

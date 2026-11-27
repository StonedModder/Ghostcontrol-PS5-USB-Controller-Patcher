#!/usr/bin/env python3
"""Exercise real DS4 discovery, hotplug and rumble relay code with host fakes.

The kernel header below declares only the two SDK calls used by source.c.
This checks host behavior, not the console ABI or transport value on hardware.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source_test = r'''
#include <assert.h>
#include "gc_ds4_source.c"
static int transport, info_result, short_info, ticks;
static GcDs4ReaderArgs *running;
void ghostpad_status_log(const char *format, ...) { (void)format; }
int gc_remote_write(pid_t pid, intptr_t addr, const void *in, size_t size) {
    (void)pid; memcpy((void *)addr, in, size); return 0;
}
int gc_remote_read(pid_t pid, intptr_t addr, void *out, size_t size) {
    (void)pid; memcpy(out, (void *)addr, size); return 0;
}
static int32_t info(int32_t handle, void *out) {
    (void)handle;
    if (!short_info) ((uint8_t *)out)[10] = (uint8_t)transport;
    return info_result;
}
static int32_t read_pad(int32_t handle, ScePadData *out) {
    (void)handle; memset(out, 0, sizeof(*out)); out->connected = 1; return 0;
}
static int32_t get_handle(int32_t user, int32_t port, int32_t index) {
    (void)user; (void)port; return index == 0 ? 42 : -1;
}
static int32_t is_ds4(int32_t handle) { (void)handle; return 1; }
static int sleep_tick(uint32_t delay) {
    (void)delay; if (++ticks == 8) running->stop = 1; return 0;
}
int gc_remote_session_call(GcRemoteSession *session, intptr_t fn,
                          const uint64_t a[6], int64_t *result) {
    (void)session;
    if (fn == 1) *result = get_handle((int32_t)a[0], (int32_t)a[1], (int32_t)a[2]);
    else if (fn == 2) *result = read_pad((int32_t)a[0], (void *)(uintptr_t)a[1]);
    else if (fn == 3) *result = 1;
    else if (fn == 4) *result = info((int32_t)a[0], (void *)(uintptr_t)a[1]);
    else assert(0);
    return 0;
}
static void check_selection(int want) {
    GcRemoteSession session = {0};
    GcDs4SourceSymbols symbols = {.get_handle=1, .read_state=2, .is_ds4=3,
                                .controller_info=4};
    GcDs4ReaderArgs args = {0};
    uint8_t scratch[256]; int32_t user = 100;
    int result = discover_sources(&session, &symbols, (intptr_t)scratch,
                                  &user, 1, &args, NULL);
    assert((result == 0) == want);
    assert(args.source_count == (unsigned)want);
    memset(&args, 0, sizeof(args));
    args.fp_read_state = (intptr_t)read_pad;
    args.fp_get_handle = (intptr_t)get_handle;
    args.fp_is_ds4 = (intptr_t)is_ds4;
    args.fp_controller_info = (intptr_t)info;
    args.fp_usleep = (intptr_t)sleep_tick;
    args.user_count = 1; args.user_ids[0] = user;
    args.owner_check_interval = 100;
    ticks = 0; running = &args;
    gc_ds4_reader_stub(&args);
    assert(args.source_count == (unsigned)want);
}
int main(void) {
    transport = 2; check_selection(1);
    transport = 0; check_selection(0);
    transport = 1; check_selection(0);
    transport = 255; check_selection(0);
    transport = 2; info_result = -1; check_selection(0);
    info_result = 0; short_info = 1; check_selection(0);
    return 0;
}
'''
bridge_test = r'''
#include <assert.h>
#define clock_gettime test_clock_gettime
#include "gc_ds4_bridge.c"
static uint64_t test_now = 1000000000;
static unsigned attempts, stops, removals;
static int fail_delivery = 1;
static uint32_t requested_sequence = 2;
int test_clock_gettime(clockid_t clock, struct timespec *out) {
    (void)clock; out->tv_sec = test_now / 1000000000;
    out->tv_nsec = test_now % 1000000000; return 0;
}
void ghostpad_status_log(const char *format, ...) { (void)format; }
int gc_ds4_game_rumble(const GcDs4GameBridge *bridge, unsigned route,
                      uint32_t *sequence, uint8_t *large, uint8_t *small) {
    (void)bridge; (void)route;
    *sequence = requested_sequence; *large = 0; *small = 0; return 0;
}
int gc_ds4_source_set_vibration(const GcDs4SourceReader *reader,
                              unsigned slot, uint8_t large, uint8_t small) {
    (void)reader; assert(slot < 2); assert(!large && !small);
    ++attempts; if (!fail_delivery) ++stops; return fail_delivery ? -1 : 0;
}
int gc_ds4_game_remove(GcDs4GameBridge *bridge) {
    (void)bridge; ++removals; return 0;
}
int main(void) {
    game.state = GC_DS4_GAME_LIVE; game.route_count = 1;
    game.routes[0].source_slot = 1;
    forward_game_rumble();
    assert(attempts == 1 && game.rumble_sequences[0] == 0);
    forward_game_rumble(); assert(attempts == 1);
    test_now += 100000000; fail_delivery = 0;
    forward_game_rumble(); assert(attempts == 2 && stops == 1);
    assert(game.rumble_sequences[0] == requested_sequence);
    forward_game_rumble(); assert(attempts == 2);
    requested_sequence = 4; forward_game_rumble(); assert(attempts == 3);
    game.state = GC_DS4_GAME_EMPTY;
    forward_game_rumble(); assert(attempts == 3);
    source.source_count = 2;
    assert(remove_game() == 0 && removals == 1);
    assert(attempts == 5 && stops == 4);
    return 0;
}
'''
game_test = r'''
#include <assert.h>
#include <string.h>
#include "gc_ds4_game_stub.h"
static int native_result, reads;
static int live_identity(const GcDs4GameBridge *bridge) { (void)bridge; return 1; }
int gc_remote_read(pid_t pid, intptr_t address, void *out, size_t size) {
    (void)pid; (void)address;
    GcDs4GameRumble value = {.sequence=2, .large=10, .small=20};
    /* Model a writer starting after the second copy read its old sequence. */
    if (++reads == 2) { value.large = 99; value.small = 99; }
    assert(size == sizeof(value)); memcpy(out, &value, size); return 0;
}
READER
static int32_t native(int32_t handle, const void *parameter) {
    (void)handle; (void)parameter; return native_result;
}
int main(void) {
    GcDs4GameArgs args = {.magic=GC_DS4_GAME_MAGIC, .layout=GC_DS4_GAME_LAYOUT,
                         .active=1, .route_count=1};
    args.originals[6] = (intptr_t)native;
    args.routes[0].pad_handle = 42;
    uint8_t motors[2] = {10, 20};
    assert(gc_ds4_game_set_vibration_stub(42, motors, &args) == 0);
    assert(args.rumble[0].sequence == 2 && args.rumble[0].large == 10);
    gc_ds4_game_set_vibration_stub(42, motors, &args);
    assert(args.rumble[0].sequence == 2);
    motors[0] = 0; motors[1] = 0;
    gc_ds4_game_set_vibration_stub(42, motors, &args);
    assert(args.rumble[0].sequence == 4 && args.rumble[0].large == 0);
    motors[0] = 90; native_result = -1;
    assert(gc_ds4_game_set_vibration_stub(42, motors, &args) == -1);
    assert(args.rumble[0].sequence == 4);
    native_result = 0;
    gc_ds4_game_set_vibration_stub(99, motors, &args);
    assert(args.rumble[0].sequence == 4);
    args.active = 0;
    gc_ds4_game_set_vibration_stub(42, motors, &args);
    assert(args.rumble[0].sequence == 4);
    GcDs4GameBridge bridge = {.state=GC_DS4_GAME_LIVE, .route_count=1};
    uint32_t sequence; uint8_t large, small;
    assert(gc_ds4_game_rumble(&bridge, 0, &sequence, &large, &small) == 0);
    assert(sequence == 2 && large == 10 && small == 20);
    return 0;
}
'''.replace('READER', 'int gc_ds4_game_rumble(' +
    (root / 'core/src/gc_ds4_game.c').read_text().split('int gc_ds4_game_rumble(', 1)[1])

with tempfile.TemporaryDirectory(prefix='ghostcontrol-ds4-') as directory:
    work = Path(directory)
    (work / 'ps5').mkdir()
    (work / 'ps5/kernel.h').write_text(
        '#include <stdint.h>\n#include <sys/types.h>\n'
        'uint32_t kernel_get_fw_version(void);\nintptr_t kernel_get_proc(pid_t);\n')
    for name, test in [('source', source_test), ('bridge', bridge_test), ('game', game_test)]:
        harness = work / f'{name}.c'
        harness.write_text(test)
        executable = work / name
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-Wall', '-Wextra', '-Wshadow', '-Werror', '-O2',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-I', str(work), '-I', str(root / 'core/src'), str(harness),
            str(root / 'core/src/gc_ds4_policy.c'),
            str(root / 'core/src/gc_ds4_game_stub.c'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
print('PASS: DS4 startup/hotplug transport; rumble capture, retry, teardown and snapshot consistency')

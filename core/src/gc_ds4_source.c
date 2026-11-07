/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_ds4_source.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#include <ps5/kernel.h>

#include "gc_remote.h"

#define GC_DS4_TARGET_PAGE_SIZE 0x4000u
#define GC_DS4_SOURCE_MAPPING_SIZE (GC_DS4_TARGET_PAGE_SIZE * 2u)
#define GC_DS4_SOURCE_INTERVAL_US 8333u
#define GC_DS4_OWNER_CHECK_LOOPS 120u

extern void ghostpad_status_log(const char *format, ...);
#define source_log(...) ghostpad_status_log("[GC DS4 source] " __VA_ARGS__)

typedef int32_t (*GcRemoteReadState)(int32_t, ScePadData *);
typedef int (*GcRemoteUsleep)(uint32_t);
typedef int (*GcRemoteKill)(int32_t, int32_t);
typedef int32_t (*GcRemoteClose)(int32_t);
typedef int32_t (*GcRemoteHandle)(int32_t, int32_t, int32_t);
typedef int32_t (*GcRemoteOpen)(int32_t, int32_t, int32_t, void *);

typedef struct {
    GcDs4Source source;
    int32_t close_on_exit;
} GcDs4ReaderChannel;

typedef struct {
    uint32_t source_count;
    GcDs4SourceFrame frames[GC_DS4_MAX_ROUTES];
} GcDs4ReaderBank;

typedef struct {
    intptr_t fp_read_state;
    intptr_t fp_usleep;
    intptr_t fp_kill;
    intptr_t fp_close;
    intptr_t fp_get_handle, fp_open, fp_is_ds4;
    intptr_t fp_clock_gettime;
    volatile uint64_t owner_deadline_ns;
    uint64_t thread_id;
    int32_t owner_pid;
    uint32_t interval_us;
    uint32_t owner_check_interval;
    uint32_t source_count;
    uint32_t user_count;
    int32_t user_ids[6];
    volatile uint32_t ready;
    volatile uint32_t stop;
    volatile uint32_t sequence;
    volatile uint32_t owner_misses;
    GcDs4ReaderChannel channels[GC_DS4_MAX_ROUTES];
    GcDs4ReaderBank banks[2];
} GcDs4ReaderArgs;

_Static_assert(sizeof(ScePadData) == 120, "ScePadData ABI changed");
_Static_assert(sizeof(GcDs4ReaderArgs) < GC_DS4_TARGET_PAGE_SIZE,
               "reader arguments exceed their RW page");

extern void *gc_ds4_reader_stub(void *argument);
extern void gc_ds4_reader_stub_end(void);

__attribute__((noinline, used, section(".text.gc_ds4_reader.1")))
void *
gc_ds4_reader_stub(void *argument)
{
    GcDs4ReaderArgs *args = argument;
    GcRemoteReadState read_state =
        (GcRemoteReadState)(uintptr_t)args->fp_read_state;
    GcRemoteUsleep sleep_us = (GcRemoteUsleep)(uintptr_t)args->fp_usleep;
    GcRemoteKill kill_process = (GcRemoteKill)(uintptr_t)args->fp_kill;
    GcRemoteClose close_pad = (GcRemoteClose)(uintptr_t)args->fp_close;
    GcRemoteHandle get_handle = (GcRemoteHandle)(uintptr_t)args->fp_get_handle;
    GcRemoteClose is_ds4 = (GcRemoteClose)(uintptr_t)args->fp_is_ds4;
    int (*get_time)(clockid_t, struct timespec *) = (void *)(uintptr_t)args->fp_clock_gettime;
    uint32_t owner_countdown = args->owner_check_interval;
    uint32_t scan_cursor = 0;

    args->ready = 1;
    while (!args->stop) {
        /* One user/index per frame bounds discovery work and handles hotplug
         * without repeatedly stopping RemotePlay with ptrace. */
        uint32_t user_slot = scan_cursor / 8u;
        int32_t pad_index = (int32_t)(scan_cursor & 7u);
        if (++scan_cursor >= args->user_count * 8u) scan_cursor = 0;
        if (args->source_count < GC_DS4_MAX_ROUTES && args->user_ids[user_slot] >= 0) {
            int32_t user = args->user_ids[user_slot];
            int already = 0;
            for (unsigned slot = 0; slot < args->source_count; ++slot)
                already |= args->channels[slot].source.user_id == user &&
                    args->channels[slot].source.pad_index == pad_index;
            if (!already) {
                int32_t handle = get_handle(user, 0, pad_index);
                ScePadData probe;
                if (handle > 0 && is_ds4(handle) > 0 &&
                    !read_state(handle, &probe) && probe.connected) {
                    int duplicate = 0;
                    for (unsigned slot = 0; slot < args->source_count; ++slot)
                        duplicate |= args->channels[slot].source.pad_handle == handle;
                    if (!duplicate) {
                        GcDs4ReaderChannel *channel = &args->channels[args->source_count++];
                        channel->source.user_id = user;
                        channel->source.pad_index = pad_index;
                        channel->source.pad_handle = handle;
                        channel->source.vendor_id = 0;
                        channel->source.product_id = 0;
                        channel->source.connected = 1;
                        channel->source.identity_verified = 1;
                        channel->close_on_exit = 0;
                    }
                }
            }
        }
        uint32_t next = args->sequence == UINT32_MAX
            ? 2u : args->sequence + 1u;
        GcDs4ReaderBank *bank = &args->banks[next & 1u];
        bank->source_count = 0;
        for (uint32_t slot = 0; slot < args->source_count;) {
            GcDs4SourceFrame *frame = &bank->frames[bank->source_count];
            frame->source = args->channels[slot].source;
            frame->read_result = read_state(
                frame->source.pad_handle, &frame->data);
            if (frame->read_result || !frame->data.connected) {
                if (args->channels[slot].close_on_exit)
                    close_pad(args->channels[slot].source.pad_handle);
                --args->source_count;
                if (slot != args->source_count)
                    args->channels[slot] = args->channels[args->source_count];
                continue;
            }
            ++bank->source_count;
            ++slot;
        }
        __asm__ volatile("sfence" ::: "memory");
        args->sequence = next;

        if (--owner_countdown == 0) {
            owner_countdown = args->owner_check_interval;
            struct timespec now;
            if (kill_process(args->owner_pid, 0) != 0 || get_time(CLOCK_MONOTONIC, &now) ||
                (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec >=
                    __atomic_load_n(&args->owner_deadline_ns, __ATOMIC_ACQUIRE)) {
                ++args->owner_misses;
                break;
            }
        }
        sleep_us(args->interval_us);
    }

    for (uint32_t slot = 0; slot < args->source_count; ++slot) {
        if (args->channels[slot].close_on_exit)
            close_pad(args->channels[slot].source.pad_handle);
    }
    __asm__ volatile("sfence" ::: "memory");
    args->ready = 2;
    return NULL;
}

__attribute__((noinline, used, section(".text.gc_ds4_reader.2")))
void
gc_ds4_reader_stub_end(void)
{
}

typedef struct {
    intptr_t get_handle;
    intptr_t open;
    intptr_t close;
    intptr_t is_ds4;
    intptr_t get_device_id;
    intptr_t get_device_info;
    intptr_t read_state;
    intptr_t read_state_ext;
    intptr_t read;
    intptr_t read_ext;
    intptr_t data_internal;
    intptr_t controller_info;
    intptr_t set_vibration;
    intptr_t usleep;
    intptr_t kill;
    intptr_t clock_gettime;
    intptr_t pthread_create;
    intptr_t pthread_join;
} GcDs4SourceSymbols;

static int
remote_call(GcRemoteSession *session, intptr_t function,
            uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
            int64_t *result)
{
    uint64_t arguments[6] = {a1, a2, a3, a4, 0, 0};
    return gc_remote_session_call(session, function, arguments, result);
}

static uint64_t
fnv1a64(const uint8_t *bytes, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void
collect_evidence(pid_t pid, const GcDs4SourceSymbols *symbols,
                 GcDs4LibpadEvidence *evidence)
{
    const intptr_t functions[GC_DS4_SOURCE_LIBPAD_SYMBOLS] = {
        symbols->read_state, symbols->read_state_ext, symbols->read,
        symbols->read_ext, symbols->data_internal, symbols->controller_info,
        symbols->set_vibration
    };
    memset(evidence, 0, sizeof(*evidence));
    evidence->firmware = kernel_get_fw_version();
    for (size_t slot = 0; slot < GC_DS4_SOURCE_LIBPAD_SYMBOLS; ++slot) {
        uint8_t code[256];
        if (functions[slot] > 0 &&
            gc_remote_read(pid, functions[slot], code, sizeof(code)) == 0) {
            evidence->fingerprints[slot] = fnv1a64(code, sizeof(code));
            evidence->fingerprint_mask |= UINT32_C(1) << slot;
        }
    }
}

static int
resolve_symbols(pid_t pid, GcDs4SourceSymbols *symbols)
{
    uint32_t pad = 0;
    uint32_t kernel = 0;
    uint32_t pthread = 0;
    if (gc_remote_get_library(pid, "libScePad", &pad) != 0 ||
        (gc_remote_get_library(pid, "libkernel_sys", &kernel) != 0 &&
         gc_remote_get_library(pid, "libkernel", &kernel) != 0))
        return -1;
    (void)gc_remote_get_library(pid, "libpthread", &pthread);
    memset(symbols, 0, sizeof(*symbols));

#define RESOLVE_PAD(field, name) \
    symbols->field = gc_remote_resolve(pid, pad, name)
    RESOLVE_PAD(get_handle, "scePadGetHandle");
    RESOLVE_PAD(open, "scePadOpen");
    RESOLVE_PAD(close, "scePadClose");
    RESOLVE_PAD(is_ds4, "scePadIsDS4Connected");
    RESOLVE_PAD(get_device_id, "scePadGetDeviceId");
    RESOLVE_PAD(get_device_info, "scePadGetDeviceInfo");
    RESOLVE_PAD(read_state, "scePadReadState");
    RESOLVE_PAD(read_state_ext, "scePadReadStateExt");
    RESOLVE_PAD(read, "scePadRead");
    RESOLVE_PAD(read_ext, "scePadReadExt");
    RESOLVE_PAD(data_internal, "scePadGetDataInternal");
    RESOLVE_PAD(controller_info, "scePadGetControllerInformation");
    RESOLVE_PAD(set_vibration, "scePadSetVibration");
#undef RESOLVE_PAD

    symbols->usleep = gc_remote_resolve(pid, kernel, "usleep");
    symbols->kill = gc_remote_resolve(pid, kernel, "kill");
    symbols->clock_gettime = gc_remote_resolve(pid, kernel, "clock_gettime");
    symbols->pthread_create =
        gc_remote_resolve(pid, kernel, "pthread_create");
    symbols->pthread_join = gc_remote_resolve(pid, kernel, "pthread_join");
    if (!symbols->pthread_create)
        symbols->pthread_create =
            gc_remote_resolve(pid, kernel, "scePthreadCreate");
    if (!symbols->pthread_join)
        symbols->pthread_join =
            gc_remote_resolve(pid, kernel, "scePthreadJoin");
    if (pthread) {
        if (!symbols->usleep)
            symbols->usleep = gc_remote_resolve(pid, pthread, "usleep");
        if (!symbols->pthread_create)
            symbols->pthread_create =
                gc_remote_resolve(pid, pthread, "pthread_create");
        if (!symbols->pthread_create)
            symbols->pthread_create =
                gc_remote_resolve(pid, pthread, "scePthreadCreate");
        if (!symbols->pthread_join)
            symbols->pthread_join =
                gc_remote_resolve(pid, pthread, "pthread_join");
        if (!symbols->pthread_join)
            symbols->pthread_join =
                gc_remote_resolve(pid, pthread, "scePthreadJoin");
    }
    return symbols->get_handle && symbols->close && symbols->is_ds4 &&
        symbols->read_state &&
        symbols->usleep && symbols->kill && symbols->clock_gettime && symbols->pthread_create &&
        symbols->pthread_join ? 0 : -1;
}

static int
probe_device_identity(GcRemoteSession *session,
                      const GcDs4SourceSymbols *symbols,
                      intptr_t scratch, int32_t handle,
                      uint16_t *vendor, uint16_t *product)
{
    int32_t device_id = -1;
    uint8_t info[256];
    int64_t call_result = -1;
    memset(info, 0, sizeof(info));
    if (!symbols->get_device_id || !symbols->get_device_info ||
        gc_remote_write(session->pid, scratch, &device_id,
                        sizeof(device_id)) != 0 ||
        remote_call(session, symbols->get_device_id,
                    (uint32_t)handle, (uint64_t)scratch, 0, 0,
                    &call_result) != 0 || call_result != 0 ||
        gc_remote_read(session->pid, scratch, &device_id,
                       sizeof(device_id)) != 0 || device_id < 0 ||
        gc_remote_write(session->pid, scratch, info, sizeof(info)) != 0 ||
        remote_call(session, symbols->get_device_info,
                    (uint32_t)device_id, (uint64_t)scratch, 0, 0,
                    &call_result) != 0 || call_result != 0 ||
        gc_remote_read(session->pid, scratch, info, sizeof(info)) != 0)
        return -1;
    memcpy(vendor, info + 16, sizeof(*vendor));
    memcpy(product, info + 18, sizeof(*product));
    return 0;
}

static int
discover_sources(GcRemoteSession *session,
                 const GcDs4SourceSymbols *symbols,
                 intptr_t scratch, const int32_t *user_ids,
                 size_t user_count, GcDs4ReaderArgs *args, GcDs4SourceReader *reader)
{
    (void)reader;
    for (size_t user_slot = 0; user_slot < user_count; ++user_slot) {
        if (user_ids[user_slot] < 0)
            continue;
        for (int32_t pad_index = 0; pad_index <= GC_DS4_MAX_PAD_INDEX;
             ++pad_index) {
            int64_t value = -1;
            if (remote_call(session, symbols->get_handle,
                            (uint32_t)user_ids[user_slot], 0,
                            (uint32_t)pad_index, 0, &value) != 0)
                return -1;
            int32_t handle = (int32_t)value;
            if (handle < 0)
                continue;

            ScePadData sample;
            memset(&sample, 0, sizeof(sample));
            if (gc_remote_write(session->pid, scratch, &sample,
                                sizeof(sample)) != 0 ||
                remote_call(session, symbols->read_state,
                            (uint32_t)handle, (uint64_t)scratch,
                            0, 0, &value) != 0)
                return -1;
            int read_ok = (int32_t)value == 0 &&
                gc_remote_read(session->pid, scratch, &sample,
                               sizeof(sample)) == 0 && sample.connected;

            int64_t is_ds4 = 0;
            if (remote_call(session, symbols->is_ds4,
                            (uint32_t)handle, 0, 0, 0, &is_ds4) != 0)
                return -1;
            uint16_t vendor = 0;
            uint16_t product = 0;
            int public_identity = probe_device_identity(
                session, symbols, scratch, handle, &vendor, &product) == 0 &&
                gc_ds4_is_known(1, vendor, product);
            int verified = read_ok && (public_identity || is_ds4 > 0);

            int duplicate = 0;
            for (uint32_t slot = 0; slot < args->source_count; ++slot) {
                duplicate |= args->channels[slot].source.pad_handle == handle;
                duplicate |=
                    args->channels[slot].source.user_id == user_ids[user_slot] &&
                    args->channels[slot].source.pad_index == pad_index;
            }
            if (verified && !duplicate &&
                args->source_count < GC_DS4_MAX_ROUTES) {
                GcDs4ReaderChannel *channel =
                    &args->channels[args->source_count++];
                channel->source.user_id = user_ids[user_slot];
                channel->source.pad_index = pad_index;
                channel->source.pad_handle = handle;
                channel->source.vendor_id = vendor;
                channel->source.product_id = product;
                channel->source.connected = 1;
                channel->source.identity_verified = 1;
                channel->close_on_exit = 0;
                source_log(
                    "selected user=0x%08x index=%d handle=0x%08x "
                    "vid=0x%04x pid=0x%04x method=%s\n",
                    (uint32_t)user_ids[user_slot], pad_index,
                    (uint32_t)handle, vendor, product,
                    public_identity ? "device-info" : "scePadIsDS4");
            }
        }
    }
    return args->source_count != 0 ? 0 : -1;
}

static int
close_owned_sources(GcRemoteSession *session,
                    const GcDs4SourceSymbols *symbols,
                    GcDs4SourceReader *reader)
{
    if (reader->pending_handle >= 0) {
        int64_t result;
        if (remote_call(session, symbols->close, (uint32_t)reader->pending_handle,
                        0, 0, 0, &result) || result) return -1;
        reader->pending_handle = -1;
    }
    while (reader->owned_count) {
        int64_t result;
        if (remote_call(session, symbols->close,
                        (uint32_t)reader->owned_handles[reader->owned_count - 1],
                        0, 0, 0, &result) || result) return -1;
        --reader->owned_count;
    }
    return 0;
}

static void
probe_controller_info_abi(GcRemoteSession *session,
                          const GcDs4SourceSymbols *symbols,
                          intptr_t scratch, int32_t handle,
                          GcDs4LibpadEvidence *evidence)
{
    uint8_t info[32];
    int64_t result = -1;
    memset(info, 0xa5, sizeof(info));
    if (!symbols->controller_info ||
        gc_remote_write(session->pid, scratch, info, sizeof(info)) != 0 ||
        remote_call(session, symbols->controller_info,
                    (uint32_t)handle, (uint64_t)scratch, 0, 0,
                    &result) != 0 || result != 0 ||
        gc_remote_read(session->pid, scratch, info, sizeof(info)) != 0)
        return;

    uint16_t touch_x = 0;
    uint16_t touch_y = 0;
    int32_t connected = 0;
    int32_t device_class = -1;
    memcpy(&touch_x, info + 4, sizeof(touch_x));
    memcpy(&touch_y, info + 6, sizeof(touch_y));
    memcpy(&connected, info + 12, sizeof(connected));
    memcpy(&device_class, info + 16, sizeof(device_class));
    evidence->controller_info_abi =
        touch_x > 0 && touch_x <= 8192 &&
        touch_y > 0 && touch_y <= 8192 &&
        info[8] <= 127 && info[9] <= 127 && info[10] <= 8 &&
        connected > 0 && device_class >= 0 && device_class <= 32;
}

static int
start_in_process(pid_t pid, const int32_t *user_ids, size_t user_count,
                 GcDs4SourceReader *reader)
{
    GcDs4SourceSymbols symbols;
    GcDs4ReaderArgs args = {0};
    GcRemoteSession *session = &reader->trace;
    if (resolve_symbols(pid, &symbols)) return -1;
    reader->pid = pid;
    reader->process_identity = kernel_get_proc(pid);
    reader->pending_handle = -1;
    reader->mapping_size = GC_DS4_SOURCE_MAPPING_SIZE;
    if (!reader->process_identity ||
        gc_remote_map(pid, reader->mapping_size, &reader->mapping)) goto done;
    reader->args_address = reader->mapping + GC_DS4_TARGET_PAGE_SIZE;
    if (gc_remote_session_open(session, pid)) goto done;
    /* RemotePlay owns this pad service. Do not change its global privilege. */
    int64_t owner_alive = -1;
    if (remote_call(session, symbols.kill, (uint32_t)getpid(), 0,
                    0, 0, &owner_alive) || owner_alive) goto done;
    collect_evidence(pid, &symbols, &reader->evidence);
    if (discover_sources(session, &symbols, reader->args_address,
                          user_ids, user_count, &args, reader)) goto done;
    probe_controller_info_abi(session, &symbols, reader->args_address,
        args.channels[0].source.pad_handle, &reader->evidence);
    size_t code_size = (uintptr_t)gc_ds4_reader_stub_end - (uintptr_t)gc_ds4_reader_stub;
    if (!code_size || code_size > GC_DS4_TARGET_PAGE_SIZE) goto done;
    args.fp_read_state = symbols.read_state;
    args.fp_usleep = symbols.usleep;
    args.fp_kill = symbols.kill;
    args.fp_close = symbols.close;
    args.fp_get_handle = symbols.get_handle;
    /* Observe only existing pad handles. scePadOpen on absent indexes can
     * exhaust the console's Bluetooth-device limit during user assignment. */
    args.fp_open = 0;
    args.fp_is_ds4 = symbols.is_ds4;
    args.fp_clock_gettime = symbols.clock_gettime;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) goto done;
    args.owner_deadline_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec + UINT64_C(3000000000);
    args.user_count = (uint32_t)user_count;
    for (size_t i = 0; i < user_count; ++i) args.user_ids[i] = user_ids[i];
    args.owner_pid = getpid();
    args.interval_us = GC_DS4_SOURCE_INTERVAL_US;
    args.owner_check_interval = GC_DS4_OWNER_CHECK_LOOPS;
    reader->source_count = args.source_count;
    for (unsigned slot = 0; slot < args.source_count; ++slot)
        reader->sources[slot] = args.channels[slot].source;
    uint8_t verify[GC_DS4_TARGET_PAGE_SIZE];
    if (gc_remote_write(pid, reader->mapping, gc_ds4_reader_stub, code_size) ||
        gc_remote_write(pid, reader->args_address, &args, sizeof(args)) ||
        gc_remote_read(pid, reader->mapping, verify, code_size) ||
        memcmp(verify, gc_ds4_reader_stub, code_size) ||
        gc_remote_protect(pid, reader->mapping, GC_DS4_TARGET_PAGE_SIZE,
                          PROT_READ | PROT_EXEC)) goto done;
    if (gc_remote_record_save(GC_REMOTE_RECORD_SOURCE, pid, reader->process_identity,
                              reader->mapping, reader->mapping_size)) goto done;
    int64_t create_result = -1;
    reader->launch_attempted = 1; /* uncertain calls can still have created a thread */
    if (remote_call(session, symbols.pthread_create,
        reader->args_address + offsetof(GcDs4ReaderArgs, thread_id),
        0, (uint64_t)reader->mapping, (uint64_t)reader->args_address,
        &create_result)) goto done;
    if (create_result) { reader->launch_attempted = 0; goto done; }
    if (gc_remote_read(pid, reader->args_address + offsetof(GcDs4ReaderArgs, thread_id),
                        &reader->thread_id, sizeof(reader->thread_id)) ||
        !reader->thread_id || gc_remote_session_close(session)) goto done;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        GcDs4SourceSnapshot snapshot;
        if (!gc_ds4_source_snapshot(reader, &snapshot)) {
            source_log("reader ready pid=%d sources=%u\n", pid, reader->source_count);
            return 0;
        }
        usleep(10000);
    }
done:
    if (gc_ds4_source_stop(reader))
        source_log("cleanup pending pid=%d; retaining reader ownership\n", pid);
    return -1;
}

int
gc_ds4_source_start(const int32_t *user_ids, size_t user_count,
                    GcDs4SourceReader *reader)
{
    if (!user_ids || user_count == 0 || user_count > 6 || !reader ||
        !gc_remote_available())
        return -1;
    if (reader->pid && gc_ds4_source_stop(reader)) return -1;
    GcRemoteRecord record;
    if (!gc_remote_record_load(GC_REMOTE_RECORD_SOURCE, &record)) {
        if (kernel_get_proc(record.pid) != record.process_identity)
            gc_remote_record_clear(GC_REMOTE_RECORD_SOURCE);
        else {
            GcDs4SourceSymbols symbols;
            GcDs4ReaderArgs args;
            uint8_t code[GC_DS4_TARGET_PAGE_SIZE];
            size_t size = (uintptr_t)gc_ds4_reader_stub_end - (uintptr_t)gc_ds4_reader_stub;
            if (!size || size > sizeof(code) || resolve_symbols(record.pid, &symbols) ||
                gc_remote_read(record.pid, record.mapping, code, size) ||
                memcmp(code, gc_ds4_reader_stub, size) ||
                gc_remote_read(record.pid, record.mapping + GC_DS4_TARGET_PAGE_SIZE, &args, sizeof(args)) ||
                args.source_count > GC_DS4_MAX_ROUTES || !args.user_count || args.user_count > 6 ||
                args.fp_read_state != symbols.read_state || args.fp_close != symbols.close ||
                args.fp_get_handle != symbols.get_handle || args.fp_open != 0 ||
                args.fp_is_ds4 != symbols.is_ds4 || args.fp_clock_gettime != symbols.clock_gettime ||
                args.fp_kill != symbols.kill || args.fp_usleep != symbols.usleep ||
                !args.thread_id) return -1;
            /* The same verified reader can be joined and reclaimed after owner death. */
            reader->pid = record.pid; reader->process_identity = record.process_identity;
            reader->mapping = record.mapping; reader->mapping_size = record.mapping_size;
            reader->args_address = record.mapping + GC_DS4_TARGET_PAGE_SIZE;
            reader->pending_handle = -1; reader->thread_id = args.thread_id;
            reader->launch_attempted = 1;
            if (gc_ds4_source_stop(reader)) return -1;
        }
    }
    pid_t pids[8];
    size_t count = gc_remote_find_pids("SceRemotePlay", pids, 8);
    for (size_t slot = 0; slot < count; ++slot) {
        if (start_in_process(pids[slot], user_ids, user_count, reader) == 0)
            return 0;
        if (reader->pid) return -1;
    }
    return -1;
}

int
gc_ds4_source_snapshot(const GcDs4SourceReader *reader,
                       GcDs4SourceSnapshot *snapshot)
{
    if (!reader || reader->pid <= 0 || reader->args_address <= 0 ||
        reader->source_count > GC_DS4_MAX_ROUTES || !snapshot ||
        kernel_get_proc(reader->pid) != reader->process_identity)
        return -1;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        GcDs4ReaderArgs remote;
        uint32_t sequence_after = 0;
        if (gc_remote_read(reader->pid, reader->args_address,
                           &remote, sizeof(remote)) != 0 ||
            gc_remote_read(
                reader->pid,
                reader->args_address + offsetof(GcDs4ReaderArgs, sequence),
                &sequence_after, sizeof(sequence_after)) != 0)
            return -1;
        if (remote.ready != 1 || remote.sequence == 0 ||
            remote.sequence != sequence_after)
            continue;
        const GcDs4ReaderBank *bank = &remote.banks[remote.sequence & 1u];
        if (bank->source_count > GC_DS4_MAX_ROUTES) return -1;
        snapshot->sequence = remote.sequence;
        snapshot->source_count = bank->source_count;
        memset(snapshot->frames, 0, sizeof(snapshot->frames));
        memcpy(snapshot->frames, bank->frames,
               bank->source_count * sizeof(snapshot->frames[0]));
        return 0;
    }
    return -1;
}

int
gc_ds4_source_set_vibration(const GcDs4SourceReader *reader,
                            unsigned source_slot, uint8_t large, uint8_t small)
{
    typedef struct { uint8_t large_motor, small_motor; } GcDs4Vibration;
    GcDs4SourceSymbols symbols;
    GcRemoteSession session = {0};
    uint32_t libpad = 0;
    intptr_t set_vibration_mode;
    int64_t mode_result = -1, vibration_result = -1;
    if (!reader || source_slot >= reader->source_count || reader->pid <= 0 ||
        kernel_get_proc(reader->pid) != reader->process_identity ||
        resolve_symbols(reader->pid, &symbols) || !symbols.set_vibration ||
        gc_remote_get_library(reader->pid, "libScePad", &libpad) ||
        gc_remote_session_open(&session, reader->pid)) return -1;
    set_vibration_mode = gc_remote_resolve(reader->pid, libpad,
                                            "scePadSetVibrationMode");
    if (!set_vibration_mode) goto failed;
    GcDs4Source source = reader->sources[source_slot];
    if (!source.identity_verified || !source.connected || source.pad_handle <= 0)
        goto failed;
    GcDs4Vibration vibration = {large, small};
    const intptr_t scratch = reader->args_address + 0x3000;
    if (gc_remote_write(reader->pid, scratch, &vibration, sizeof(vibration)) ||
        remote_call(&session, set_vibration_mode,
            (uint32_t)source.pad_handle, 2, 0, 0,
            &mode_result) ||
        remote_call(&session, symbols.set_vibration, (uint32_t)source.pad_handle,
            (uint64_t)scratch, 0, 0, &vibration_result)) goto failed;
    if (gc_remote_session_close(&session)) return -1;
    source_log("rumble handle=0x%08x large=%u small=%u mode_rc=%lld set_rc=%lld\n",
        (uint32_t)source.pad_handle, large, small,
        (long long)mode_result, (long long)vibration_result);
    return vibration_result == 0 ? 0 : -1;
failed:
    (void)gc_remote_session_close(&session);
    return -1;
}

int gc_ds4_source_heartbeat(const GcDs4SourceReader *reader)
{
    struct timespec now;
    if (!reader || !reader->args_address || clock_gettime(CLOCK_MONOTONIC, &now) ||
        kernel_get_proc(reader->pid) != reader->process_identity) return -1;
    uint64_t deadline = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec + UINT64_C(3000000000);
    return gc_remote_write(reader->pid,
        reader->args_address + offsetof(GcDs4ReaderArgs, owner_deadline_ns), &deadline, sizeof(deadline));
}

int gc_ds4_source_request_stop(const GcDs4SourceReader *reader)
{
    if (!reader || !reader->pid) return 0;
    if (kernel_get_proc(reader->pid) != reader->process_identity) return 0;
    uint32_t stop = 1;
    return gc_remote_write(reader->pid,
        reader->args_address + offsetof(GcDs4ReaderArgs, stop), &stop, sizeof(stop));
}

int
gc_ds4_source_stop(GcDs4SourceReader *reader)
{
    if (!reader) return -1;
    if (!reader->pid) return 0;
    if (kernel_get_proc(reader->pid) != reader->process_identity) {
        memset(reader, 0, sizeof(*reader));
        gc_remote_record_clear(GC_REMOTE_RECORD_SOURCE); return 0;
    }
    GcDs4SourceSymbols symbols;
    GcRemoteSession *session = &reader->trace;
    if (session->needs_restore || session->state == GC_REMOTE_RUNNING_UNKNOWN)
        if (gc_remote_session_close(session)) return -1;
    if (reader->launch_attempted) {
        uint32_t stop = 1, ready = 0;
        if (gc_remote_write(reader->pid,
            reader->args_address + offsetof(GcDs4ReaderArgs, stop), &stop, sizeof(stop)) ||
            gc_remote_session_close(session)) return -1;
        for (unsigned attempt = 0; attempt < 200; ++attempt) {
            if (!gc_remote_read(reader->pid,
                reader->args_address + offsetof(GcDs4ReaderArgs, ready), &ready, sizeof(ready)) &&
                ready == 2) break;
            usleep(10000);
        }
        if (ready != 2) return -1;
        if (!reader->thread_id && gc_remote_read(reader->pid,
            reader->args_address + offsetof(GcDs4ReaderArgs, thread_id),
            &reader->thread_id, sizeof(reader->thread_id))) return -1;
        if (!reader->thread_id) return -1;
    }
    if (reader->launch_attempted && !reader->joined) {
        if (resolve_symbols(reader->pid, &symbols) ||
            gc_remote_session_open(session, reader->pid)) return -1;
        int64_t result = -1;
        if (remote_call(session, symbols.pthread_join, reader->thread_id, 0, 0, 0, &result) ||
            result) { (void)gc_remote_session_close(session); return -1; }
        reader->joined = 1;
        reader->owned_count = 0; /* reader closed them before its stopped marker */
    } else if (!reader->launch_attempted &&
               (reader->owned_count || reader->pending_handle >= 0)) {
        if (resolve_symbols(reader->pid, &symbols)) return -1;
        if (session->state != GC_REMOTE_STOPPED &&
            gc_remote_session_open(session, reader->pid)) return -1;
        if (close_owned_sources(session, &symbols, reader)) {
            (void)gc_remote_session_close(session); return -1;
        }
    }
    if (gc_remote_session_close(session)) return -1;
    if (reader->mapping && gc_remote_unmap(reader->pid, reader->mapping, reader->mapping_size))
        return -1;
    memset(reader, 0, sizeof(*reader));
    gc_remote_record_clear(GC_REMOTE_RECORD_SOURCE);
    return 0;
}

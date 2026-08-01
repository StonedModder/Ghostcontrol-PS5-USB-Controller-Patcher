/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional wireless DualShock 4 bridge for native PS5 games.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <machine/reg.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#include <ps5/nid.h>

#include "wireless_ds4.h"
#include "gc_types.h"

extern void ghostpad_status_log(const char *format, ...);
#define klog_printf ghostpad_status_log
#define DS4TOD5_REMOTE_PAD_CAPACITY 256u

static void report_printf(int fd, const char *format, ...);

static uint64_t
ghostpad_fnv1a64(const uint8_t *buf, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    if (!buf) return 0;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)buf[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* sys_ptrace - elevate credentials for ptrace, then restore */
static int
sys_ptrace(int request, pid_t pid, caddr_t addr, int data)
{
    uint8_t privcaps[16] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };
    pid_t   mypid = getpid();
    uint8_t caps[16];
    uint64_t authid;
    int ret;

    if (!(authid = kernel_get_ucred_authid(mypid))) return -1;
    if (kernel_get_ucred_caps(mypid, caps))          return -1;
    if (kernel_set_ucred_authid(mypid, 0x4800000000010003l)) return -1;
    if (kernel_set_ucred_caps(mypid, privcaps)) {
        (void)kernel_set_ucred_authid(mypid, authid);
        return -1;
    }

    ret = (int)__syscall(SYS_ptrace, request, pid, addr, data);

    (void)kernel_set_ucred_caps(mypid, caps);
    (void)kernel_set_ucred_authid(mypid, authid);
    return ret;
}

/* find_pids - locate processes by thread name via sysctl (ki_pid@72, ki_tdname@447) */
static size_t
find_pids(const char *name, pid_t *pids, size_t max_pids)
{
    int mib[4] = {1, 14, 8, 0};
    pid_t mypid = getpid();
    size_t buf_size;
    uint8_t *buf;
    size_t count = 0;

    if (!pids || max_pids == 0) return 0;
    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0)) return 0;
    if (!(buf = malloc(buf_size)))                 return 0;
    if (sysctl(mib, 4, buf, &buf_size, NULL, 0)) { free(buf); return 0; }

    const uint8_t *end = buf + buf_size;
    for (uint8_t *ptr = buf; ptr < end;) {
        if ((size_t)(end - ptr) < sizeof(int))
            break;
        int ki_structsize = 0;
        memcpy(&ki_structsize, ptr, sizeof(ki_structsize));
        if (ki_structsize < 448 ||
            (size_t)ki_structsize > (size_t)(end - ptr))
            break;
        pid_t ki_pid = 0;
        memcpy(&ki_pid, ptr + 72, sizeof(ki_pid));
        const char *ki_tdname = (const char *)&ptr[447];
        size_t tdname_limit = (size_t)ki_structsize - 447u;
        size_t tdname_length = 0;
        while (tdname_length < tdname_limit &&
               ki_tdname[tdname_length] != '\0')
            tdname_length++;
        size_t pi;
        int seen = 0;

        ptr += (size_t)ki_structsize;
        if (tdname_length == tdname_limit ||
            strcmp(name, ki_tdname) || ki_pid == mypid) {
            continue;
        }
        for (pi = 0; pi < count; pi++) {
            if (pids[pi] == ki_pid) {
                seen = 1;
                break;
            }
        }
        if (seen || count >= max_pids) {
            continue;
        }
        pids[count++] = ki_pid;
    }

    for (size_t i = 1; i < count; i++) {
        pid_t pid = pids[i];
        size_t j = i;
        while (j > 0 && pids[j - 1] > pid) {
            pids[j] = pids[j - 1];
            j--;
        }
        pids[j] = pid;
    }

    free(buf);
    return count;
}

/* resolve_sym - look up a symbol in a remote process library */
static intptr_t
resolve_sym(pid_t pid, uint32_t lib_handle, const char *sym)
{
    intptr_t addr = kernel_dynlib_dlsym(pid, lib_handle, sym);
    if (addr) return addr;

#ifdef __PROSPERO__
    char nid[12];
    nid_encode(sym, nid);
    addr = kernel_dynlib_resolve(pid, lib_handle, nid);
    return addr;
#else
    return 0;
#endif
}

/* get_lib - wrapper around kernel_dynlib_handle with logging */
static int
get_lib(pid_t pid, const char *name, uint32_t *handle)
{
    *handle = 0;
    int ret = kernel_dynlib_handle(pid, name, handle);
    if (ret != 0 || *handle == 0) {
        char sprx[64];
        snprintf(sprx, sizeof(sprx), "%s.sprx", name);
        ret = kernel_dynlib_handle(pid, sprx, handle);
    }
    klog_printf("[WirelessDS4] dynlib_handle(%s) -> ret=%d handle=0x%x\n",
                name, ret, *handle);
    return (*handle != 0) ? 0 : -1;
}

/* pt_io_write - write process memory via PT_IO (process must be stopped) */
static int
pt_io_write(pid_t pid, intptr_t dst, const void *src, size_t len)
{
    struct ptrace_io_desc iod;
    iod.piod_op    = PIOD_WRITE_D;
    iod.piod_offs  = (void *)dst;
    iod.piod_addr  = (void *)src;
    iod.piod_len   = len;
    return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}

static int64_t
pt_call(pid_t pid, intptr_t fn, intptr_t trap_rip,
        uint64_t a1, uint64_t a2, uint64_t a3,
        uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct reg regs = {0};
    struct reg saved = {0};
    int status;

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) return -1;
    memcpy(&saved, &regs, sizeof(regs));

    /*
     * Skip the x86-64 red zone, then build the stack exactly as a real CALL
     * would. The AMD64 ABI requires 16-byte alignment before CALL, so a
     * callee observes RSP % 16 == 8 after the return address is pushed.
     * Entering with RSP % 16 == 0 can fault on aligned SIMD stack accesses.
     */
    intptr_t new_rsp = ((regs.r_rsp - 256) & ~(intptr_t)0xf) - 8;

    if (pt_io_write(pid, new_rsp, &trap_rip, 8)) return -1;

    regs.r_rsp = new_rsp;
    regs.r_rip = fn;
    regs.r_rdi = a1;
    regs.r_rsi = a2;
    regs.r_rdx = a3;
    regs.r_rcx = a4;
    regs.r_r8  = a5;
    regs.r_r9  = a6;

    if (sys_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0)) return -1;
    if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0)) {
        (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }

    /* Wait for our SIGTRAP; forward other signals so the process stays healthy.
     * Use WNOHANG + 1 ms sleep so we never block forever if the INT3 doesn't fire
     * (e.g. if the write failed silently or the process runs past trap_rip).
     * SIGCHLD (17) is suppressed rather than forwarded: forwarding it while the
     * main thread is executing injected code (pthread_create, pad IPC calls) has
     * been observed to cause a kernel panic when SceShellUI's SIGCHLD handler
     * runs concurrently with thread-creation internals. */
    int got_trap = 0;
    for (int total_ms = 0; total_ms < 5000; ) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r < 0) {
            klog_printf("[WirelessDS4] pt_call: waitpid error errno=%d\n", errno);
            break;
        }
        if (r == 0) {
            usleep(1000);   /* 1 ms - process has not stopped yet */
            total_ms++;
            continue;
        }
        /* Process stopped */
        if (!WIFSTOPPED(status)) {
            klog_printf("[WirelessDS4] pt_call: process exited status=0x%x\n", status);
            sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -1;
        }
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) { got_trap = 1; break; }
        if (sig == SIGBUS || sig == SIGSEGV) {
            klog_printf("[WirelessDS4] pt_call: suppressing fatal target sig=%d\n",
                        sig);
            (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -2;
        }
        /* Suppress SIGCHLD; forwarding it during injected execution causes panics. */
        int fwd = (sig == 17) ? 0 : sig;
        if (fwd != sig)
            klog_printf("[WirelessDS4] pt_call: suppressing SIGCHLD\n");
        else
            klog_printf("[WirelessDS4] pt_call: forwarding sig=%d\n", sig);
        if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, fwd) != 0) {
            (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -1;
        }
    }
    if (!got_trap) {
        klog_printf("[WirelessDS4] pt_call: timed out waiting for SIGTRAP fn=0x%lx\n", fn);
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) {
        (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    int64_t retval = (int64_t)regs.r_rax;
    klog_printf("[WirelessDS4] pt_call: fn=0x%lx rip=0x%lx rax=0x%lx\n",
                fn, (uint64_t)regs.r_rip, (uint64_t)retval);

    sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return retval;
}


typedef struct {
    intptr_t fp_readstate;
    intptr_t fp_usleep;
    int32_t  pad_handle;
    uint32_t interval_us;
    volatile int32_t  ready;       /* 0=starting, 1=running, 2=stopped */
    volatile int32_t  stop;
    volatile int32_t  last_result;
    volatile uint32_t seq;
    uint8_t pad_data[DS4TOD5_REMOTE_PAD_CAPACITY];
} RemotePadReaderArgs;

extern void *remote_pad_reader_stub(void *arg);
extern void remote_pad_reader_stub_end(void);

/*
 * Position-independent target thread. Do not call local symbols or touch
 * globals: every target-side call is made through a resolved function pointer
 * stored in RemotePadReaderArgs.
 */
__attribute__((noinline, used, section(".text.ds4reader")))
void *
remote_pad_reader_stub(void *arg)
{
    RemotePadReaderArgs *a = (RemotePadReaderArgs *)arg;
    typedef int32_t (*read_fn_t)(int32_t, void *);
    typedef void (*usleep_fn_t)(unsigned int);
    read_fn_t readstate = (read_fn_t)(uintptr_t)a->fp_readstate;
    usleep_fn_t sleep_us = (usleep_fn_t)(uintptr_t)a->fp_usleep;

    __atomic_store_n(&a->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&a->stop, __ATOMIC_ACQUIRE)) {
        uint32_t odd =
            (__atomic_load_n(&a->seq, __ATOMIC_RELAXED) + 1u) | 1u;
        __atomic_store_n(&a->seq, odd, __ATOMIC_RELEASE);
        int32_t result = readstate(a->pad_handle, a->pad_data);
        __atomic_store_n(&a->last_result, result, __ATOMIC_RELAXED);
        __atomic_store_n(&a->seq, odd + 1u, __ATOMIC_RELEASE);
        sleep_us(a->interval_us);
    }
    __atomic_store_n(&a->ready, 2, __ATOMIC_RELEASE);
    return (void *)0;
}

__attribute__((noinline, used, section(".text.ds4reader")))
void
remote_pad_reader_stub_end(void)
{
}

/*
 * Native-game pad-read bridge. These functions are copied into the target
 * game and must remain position independent: no local calls, global accesses,
 * or libc helpers are permitted in the copied range.
 */
#define DS4TOD5_GAME_BRIDGE_MAGIC UINT32_C(0x34424744) /* "DGB4" */
#define DS4TOD5_GAME_BRIDGE_PAD_SIZE 120u
#define DS4TOD5_GAME_BRIDGE_PORT 5906u
#define DS4TOD5_GAME_BRIDGE_STOP_MAGIC UINT32_C(0x504f5453) /* "STOP" */

typedef struct {
    uint32_t magic;
    volatile uint32_t active;
    volatile uint32_t seq;
    uint32_t pad_size;
    int32_t pad_handle;
    uint32_t reserved0;
    intptr_t fp_state_internal;
    intptr_t fp_read_internal;
    intptr_t fp_data_internal;
    intptr_t fp_get_controller_info_trampoline;
    intptr_t fp_socket;
    intptr_t fp_bind;
    intptr_t fp_recvfrom;
    intptr_t fp_close;
    intptr_t remote_block;
    uint32_t remote_block_size;
    uint32_t original_protection;
    uint32_t original_data_protection;
    uint32_t original_info_function_protection;
    uint32_t reserved1;
    intptr_t read_state_address;
    intptr_t read_state_ext_address;
    intptr_t read_address;
    intptr_t read_ext_address;
    intptr_t data_internal_address;
    intptr_t controller_info_address;
    intptr_t controller_info_trampoline;
    intptr_t controller_info_gateway;
    uint8_t original_read_state[16];
    uint8_t original_read_state_ext[16];
    uint8_t original_read[16];
    uint8_t original_read_ext[16];
    uint8_t original_data_internal[16];
    uint8_t original_controller_info[32];
    volatile uint64_t read_state_calls;
    volatile uint64_t read_state_ext_calls;
    volatile uint64_t read_calls;
    volatile uint64_t read_ext_calls;
    volatile uint64_t data_internal_calls;
    volatile uint64_t controller_info_calls;
    volatile uint64_t controller_info_spoofs;
    volatile int32_t receiver_ready;
    volatile int32_t receiver_stop;
    volatile int32_t receiver_last_result;
    uint32_t receiver_port;
    volatile uint64_t receiver_packets;
    uint8_t pad_data[DS4TOD5_GAME_BRIDGE_PAD_SIZE];
} GamePadBridgeArgs;

static int g_game_bridge_sender_fd = -1;

static int game_bridge_send_stop(void);

extern int32_t game_pad_read_state_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_read_state_ext_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_read_stub(
    int32_t handle, void *out, int32_t num, GamePadBridgeArgs *args);
extern int32_t game_pad_read_ext_stub(
    int32_t handle, void *out, int32_t num, GamePadBridgeArgs *args);
extern int32_t game_pad_get_data_internal_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_get_controller_info_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern void *game_pad_bridge_receiver_stub(void *arg);
extern void game_pad_bridge_stub_end(void);

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_state_stub(int32_t handle, void *out,
                         GamePadBridgeArgs *args)
{
    typedef int32_t (*state_internal_fn)(int32_t, void *, int32_t);
    if (args && args->magic == DS4TOD5_GAME_BRIDGE_MAGIC &&
        __atomic_load_n(&args->active, __ATOMIC_ACQUIRE) &&
        handle == args->pad_handle && out &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before =
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE);
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            if (before ==
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE)) {
                (void)__atomic_fetch_add(
                    &args->read_state_calls, 1, __ATOMIC_RELAXED);
                return 0;
            }
        }
    }
    state_internal_fn original =
        (state_internal_fn)(uintptr_t)(args ? args->fp_state_internal : 0);
    return original ? original(handle, out, 0) : (int32_t)0x80920001u;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_state_ext_stub(int32_t handle, void *out,
                             GamePadBridgeArgs *args)
{
    typedef int32_t (*state_internal_fn)(int32_t, void *, int32_t);
    if (args && args->magic == DS4TOD5_GAME_BRIDGE_MAGIC &&
        __atomic_load_n(&args->active, __ATOMIC_ACQUIRE) &&
        handle == args->pad_handle && out &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before =
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE);
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            if (before ==
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE)) {
                (void)__atomic_fetch_add(
                    &args->read_state_ext_calls, 1, __ATOMIC_RELAXED);
                return 0;
            }
        }
    }
    state_internal_fn original =
        (state_internal_fn)(uintptr_t)(args ? args->fp_state_internal : 0);
    return original ? original(handle, out, 1) : (int32_t)0x80920001u;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_stub(int32_t handle, void *out, int32_t num,
                   GamePadBridgeArgs *args)
{
    typedef int32_t (*read_internal_fn)(int32_t, void *, int32_t, int32_t);
    if (args && args->magic == DS4TOD5_GAME_BRIDGE_MAGIC &&
        __atomic_load_n(&args->active, __ATOMIC_ACQUIRE) &&
        handle == args->pad_handle && out && num > 0 &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before =
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE);
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            if (before ==
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE)) {
                (void)__atomic_fetch_add(
                    &args->read_calls, 1, __ATOMIC_RELAXED);
                return 1;
            }
        }
    }
    read_internal_fn original =
        (read_internal_fn)(uintptr_t)(args ? args->fp_read_internal : 0);
    return original ? original(handle, out, num, 0) :
                      (int32_t)0x80920001u;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_ext_stub(int32_t handle, void *out, int32_t num,
                       GamePadBridgeArgs *args)
{
    typedef int32_t (*read_internal_fn)(int32_t, void *, int32_t, int32_t);
    if (args && args->magic == DS4TOD5_GAME_BRIDGE_MAGIC &&
        __atomic_load_n(&args->active, __ATOMIC_ACQUIRE) &&
        handle == args->pad_handle && out && num > 0 &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before =
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE);
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            if (before ==
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE)) {
                (void)__atomic_fetch_add(
                    &args->read_ext_calls, 1, __ATOMIC_RELAXED);
                return 1;
            }
        }
    }
    read_internal_fn original =
        (read_internal_fn)(uintptr_t)(args ? args->fp_read_internal : 0);
    return original ? original(handle, out, num, 1) :
                      (int32_t)0x80920001u;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_get_data_internal_stub(int32_t handle, void *out,
                                GamePadBridgeArgs *args)
{
    typedef int32_t (*data_internal_fn)(int32_t, void *, int32_t);
    if (args && args->magic == DS4TOD5_GAME_BRIDGE_MAGIC &&
        __atomic_load_n(&args->active, __ATOMIC_ACQUIRE) &&
        handle == args->pad_handle && out &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before =
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE);
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            if (before ==
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE)) {
                (void)__atomic_fetch_add(
                    &args->data_internal_calls, 1, __ATOMIC_RELAXED);
                return 0;
            }
        }
    }
    data_internal_fn original =
        (data_internal_fn)(uintptr_t)(args ? args->fp_data_internal : 0);
    return original ? original(handle, out, 1) :
                      (int32_t)0x80920001u;
}

/*
 * ScePadControllerInformation (firmware 11.60, standard controller):
 *   +0x00 float touchpadDensity
 *   +0x04 uint16_t touchResolutionX
 *   +0x06 uint16_t touchResolutionY
 *   +0x08 uint8_t stickDeadzoneL
 *   +0x09 uint8_t stickDeadzoneR
 *   +0x0a uint8_t connectionType
 *   +0x0b uint8_t connectedCount
 *   +0x0c int32_t connected
 *   +0x10 int32_t deviceClass
 *   +0x14 uint8_t reserved[8]
 *
 * The game-local import hook calls Sony first and changes only the standard
 * metadata prefix.  It is activated only after the loopback receiver has a
 * fresh wireless ScePadData snapshot, so admission and data availability move
 * together.
 */
__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_get_controller_info_stub(int32_t handle, void *out,
                                  GamePadBridgeArgs *args)
{
    typedef int32_t (*get_info_fn)(int32_t, void *);
    get_info_fn original = (get_info_fn)(uintptr_t)(
        args ? args->fp_get_controller_info_trampoline : 0);
    int32_t result = original
        ? original(handle, out) : (int32_t)0x80920001u;
    int have_fresh_pad = 0;
    if (args) {
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before =
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE);
            if (before & 1u)
                continue;
            uint8_t connected =
                args->pad_data[offsetof(ScePadData, connected)];
            if (before ==
                __atomic_load_n(&args->seq, __ATOMIC_ACQUIRE)) {
                have_fresh_pad = connected != 0;
                break;
            }
        }
    }
    if (args)
        (void)__atomic_fetch_add(
            &args->controller_info_calls, 1, __ATOMIC_RELAXED);
    if (result == 0 && args &&
        args->magic == DS4TOD5_GAME_BRIDGE_MAGIC &&
        __atomic_load_n(&args->active, __ATOMIC_ACQUIRE) &&
        handle == args->pad_handle && out && have_fresh_pad) {
        volatile uint8_t *info = (volatile uint8_t *)out;
        /* 1.0f pixels/mm-ish density; DS4 native 1920x943 touch range. */
        info[0] = 0x00;
        info[1] = 0x00;
        info[2] = 0x80;
        info[3] = 0x3f;
        info[4] = 0x80;
        info[5] = 0x07;
        info[6] = 0xaf;
        info[7] = 0x03;
        info[8] = 2;
        info[9] = 2;
        info[10] = 0; /* local controller */
        info[11] = 1;
        info[12] = 1;
        info[13] = 0;
        info[14] = 0;
        info[15] = 0;
        info[16] = 0; /* standard controller class */
        info[17] = 0;
        info[18] = 0;
        info[19] = 0;
        (void)__atomic_fetch_add(
            &args->controller_info_spoofs, 1, __ATOMIC_RELAXED);
    }
    return result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
void *
game_pad_bridge_receiver_stub(void *arg)
{
    GamePadBridgeArgs *args = (GamePadBridgeArgs *)arg;
    typedef int32_t (*socket_fn)(int32_t, int32_t, int32_t);
    typedef int32_t (*bind_fn)(int32_t, const void *, uint32_t);
    typedef int64_t (*recvfrom_fn)(
        int32_t, void *, uint64_t, int32_t, void *, void *);
    typedef int32_t (*close_fn)(int32_t);
    socket_fn socket_call =
        (socket_fn)(uintptr_t)args->fp_socket;
    bind_fn bind_call = (bind_fn)(uintptr_t)args->fp_bind;
    recvfrom_fn recvfrom_call =
        (recvfrom_fn)(uintptr_t)args->fp_recvfrom;
    close_fn close_call = (close_fn)(uintptr_t)args->fp_close;
    struct {
        uint8_t length;
        uint8_t family;
        uint16_t port;
        uint32_t address;
        uint8_t zero[8];
    } loopback;
    uint8_t packet[DS4TOD5_GAME_BRIDGE_PAD_SIZE];
    for (unsigned byte = 0; byte < sizeof(loopback); ++byte)
        ((uint8_t *)(void *)&loopback)[byte] = 0;
    loopback.length = 16;
    loopback.family = 2; /* AF_INET */
    loopback.port = (uint16_t)(
        ((args->receiver_port & 0xffu) << 8) |
        ((args->receiver_port >> 8) & 0xffu));
    loopback.address = UINT32_C(0x0100007f); /* 127.0.0.1 */

    int32_t fd = socket_call(2, 2, 0); /* AF_INET, SOCK_DGRAM */
    __atomic_store_n(
        &args->receiver_last_result, fd, __ATOMIC_RELAXED);
    if (fd < 0) {
        __atomic_store_n(&args->receiver_ready, -1, __ATOMIC_RELEASE);
        return (void *)0;
    }
    int32_t bind_result = bind_call(fd, &loopback, sizeof(loopback));
    __atomic_store_n(
        &args->receiver_last_result, bind_result, __ATOMIC_RELAXED);
    if (bind_result != 0) {
        __atomic_store_n(&args->receiver_ready, -2, __ATOMIC_RELEASE);
        close_call(fd);
        return (void *)0;
    }
    __atomic_store_n(&args->receiver_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&args->receiver_stop, __ATOMIC_ACQUIRE)) {
        int64_t received = recvfrom_call(
            fd, packet, sizeof(packet), 0, (void *)0, (void *)0);
        __atomic_store_n(
            &args->receiver_last_result, (int32_t)received,
            __ATOMIC_RELAXED);
        if (received == (int64_t)sizeof(uint32_t)) {
            uint32_t magic = 0;
            for (unsigned byte = 0; byte < sizeof(magic); ++byte)
                ((uint8_t *)(void *)&magic)[byte] = packet[byte];
            if (magic == DS4TOD5_GAME_BRIDGE_STOP_MAGIC)
                break;
        }
        if (received != (int64_t)DS4TOD5_GAME_BRIDGE_PAD_SIZE)
            continue;
        uint32_t odd =
            (__atomic_load_n(&args->seq, __ATOMIC_RELAXED) + 1u) | 1u;
        __atomic_store_n(&args->seq, odd, __ATOMIC_RELEASE);
        for (unsigned byte = 0;
             byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
             ++byte)
            args->pad_data[byte] = packet[byte];
        __atomic_store_n(&args->seq, odd + 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&args->active, 1, __ATOMIC_RELEASE);
        (void)__atomic_fetch_add(
            &args->receiver_packets, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&args->active, 0, __ATOMIC_RELEASE);
    close_call(fd);
    __atomic_store_n(&args->receiver_ready, 2, __ATOMIC_RELEASE);
    return (void *)0;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
void
game_pad_bridge_stub_end(void)
{
}

static int
remote_reader_copyout(pid_t pid, intptr_t addr, void *buf, size_t len)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)addr; (void)buf; (void)len;
    return -1;
#else
    pid_t self = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (saved_authid &&
        kernel_set_ucred_authid(self, 0x4800000000010003l) != 0)
        return -1;
    int result = mdbg_copyout(pid, addr, buf, len);
    if (saved_authid)
        kernel_set_ucred_authid(self, saved_authid);
    return result;
#endif
}

static int
remote_reader_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)buf; (void)addr; (void)len;
    return -1;
#else
    pid_t self = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (saved_authid &&
        kernel_set_ucred_authid(self, 0x4800000000010003l) != 0)
        return -1;
    int result = mdbg_copyin(pid, buf, addr, len);
    if (saved_authid)
        kernel_set_ucred_authid(self, saved_authid);
    return result;
#endif
}

int
wireless_ds4_remote_reader_start(int32_t userId, pid_t *out_pid,
                                intptr_t *out_args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)userId; (void)out_pid; (void)out_args_kaddr;
    return -1;
#else
    pid_t pids[8];
    size_t count = find_pids("SceRemotePlay", pids, 8);
    pid_t target;
    int attached = 0;
    int launched = 0;
    int trap_saved = 0;
    int trap_writable = 0;
    int trap_protection = PROT_READ | PROT_EXEC;
    uint8_t original_trap_byte = 0;
    intptr_t trap_mem = 0;
    intptr_t remote_block = 0;
    intptr_t args_addr = 0;
    intptr_t stub_addr = 0;
    intptr_t thread_addr = 0;
    intptr_t fn_free = 0;

    if (out_pid) *out_pid = -1;
    if (out_args_kaddr) *out_args_kaddr = 0;
    if (count == 0) {
        klog_printf("[DS4toDS5] reader start: SceRemotePlay not found\n");
        return -1;
    }
    target = pids[0];

    klog_printf("[DS4toDS5] reader start: PT_ATTACH pid=%d user=0x%08x\n",
                target, (uint32_t)userId);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[DS4toDS5] reader start: attach failed errno=%d\n",
                    errno);
        return -1;
    }
    attached = 1;
    if (waitpid(target, NULL, 0) < 0)
        goto cleanup;

    uint32_t libpad_h = 0, libkernel_h = 0, libpthread_h = 0;
    uint32_t liblibc_h = 0;
    get_lib(target, "libScePad", &libpad_h);
    if (get_lib(target, "libkernel_sys", &libkernel_h) != 0)
        (void)get_lib(target, "libkernel", &libkernel_h);
    get_lib(target, "libpthread", &libpthread_h);
    get_lib(target, "libSceLibcInternal", &liblibc_h);

    intptr_t fn_gethandle = libpad_h
        ? resolve_sym(target, libpad_h, "scePadGetHandle") : 0;
    intptr_t fn_open = libpad_h
        ? resolve_sym(target, libpad_h, "scePadOpen") : 0;
    intptr_t fn_readstate = libpad_h
        ? resolve_sym(target, libpad_h, "scePadReadState") : 0;
    intptr_t fn_setpriv = libpad_h
        ? resolve_sym(target, libpad_h, "scePadSetProcessPrivilege") : 0;
    intptr_t fn_usleep = libkernel_h
        ? resolve_sym(target, libkernel_h, "usleep") : 0;
    intptr_t fn_pthread_create = libkernel_h
        ? resolve_sym(target, libkernel_h, "pthread_create") : 0;
    intptr_t fn_malloc = liblibc_h
        ? resolve_sym(target, liblibc_h, "malloc") : 0;
    fn_free = liblibc_h
        ? resolve_sym(target, liblibc_h, "free") : 0;
    if (!fn_usleep && libpthread_h)
        fn_usleep = resolve_sym(target, libpthread_h, "usleep");
    if (!fn_pthread_create && libkernel_h)
        fn_pthread_create =
            resolve_sym(target, libkernel_h, "scePthreadCreate");
    if (!fn_pthread_create && libpthread_h)
        fn_pthread_create =
            resolve_sym(target, libpthread_h, "pthread_create");
    if (!fn_pthread_create && libpthread_h)
        fn_pthread_create =
            resolve_sym(target, libpthread_h, "scePthreadCreate");

    trap_mem = libpad_h ? kernel_dynlib_fini_addr(target, libpad_h) : 0;
    if (!trap_mem && libpad_h)
        trap_mem = kernel_dynlib_init_addr(target, libpad_h);

    klog_printf("[DS4toDS5] reader symbols: get=0x%lx open=0x%lx "
                "read=0x%lx sleep=0x%lx pthread=0x%lx malloc=0x%lx "
                "trap=0x%lx\n",
                fn_gethandle, fn_open, fn_readstate, fn_usleep,
                fn_pthread_create, fn_malloc, trap_mem);
    if ((!fn_gethandle && !fn_open) || !fn_readstate || !fn_usleep ||
        !fn_pthread_create || !fn_malloc || !trap_mem)
        goto cleanup;

    if (mdbg_copyout(target, trap_mem, &original_trap_byte, 1) != 0)
        goto cleanup;
    trap_saved = 1;
    {
        int protection = kernel_get_vmem_protection(target, trap_mem, 1);
        if (protection >= 0)
            trap_protection = protection;
    }
    if (kernel_set_vmem_protection(target, trap_mem, 16,
                                   PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto cleanup;
    trap_writable = 1;
    {
        uint8_t int3 = 0xcc;
        if (pt_io_write(target, trap_mem, &int3, 1) != 0)
            goto cleanup;
    }

    if (fn_setpriv) {
        int64_t result = pt_call(target, fn_setpriv, trap_mem,
                                 1, 0, 0, 0, 0, 0);
        klog_printf("[DS4toDS5] reader setpriv=0x%llx\n",
                    (unsigned long long)(uint64_t)result);
    }

    int32_t handle = -1;
    if (fn_gethandle) {
        int64_t result = pt_call(target, fn_gethandle, trap_mem,
                                 (uint32_t)userId, 0, 0, 0, 0, 0);
        if ((int32_t)result >= 0)
            handle = (int32_t)result;
        klog_printf("[DS4toDS5] reader GetHandle=0x%llx\n",
                    (unsigned long long)(uint64_t)result);
    }
    if (handle < 0 && fn_open) {
        int64_t result = pt_call(target, fn_open, trap_mem,
                                 (uint32_t)userId, 0, 0, 0, 0, 0);
        if ((int32_t)result >= 0)
            handle = (int32_t)result;
        klog_printf("[DS4toDS5] reader Open=0x%llx\n",
                    (unsigned long long)(uint64_t)result);
    }
    if (handle < 0)
        goto cleanup;

    size_t stub_len =
        (size_t)((uintptr_t)remote_pad_reader_stub_end -
                 (uintptr_t)remote_pad_reader_stub);
    size_t stub_off = 16;
    size_t args_off = (stub_off + stub_len + 15) & ~(size_t)15;
    size_t thread_off =
        (args_off + sizeof(RemotePadReaderArgs) + 15) & ~(size_t)15;
    size_t alloc_size = thread_off + 16;

    remote_block = (intptr_t)pt_call(target, fn_malloc, trap_mem,
                                     alloc_size, 0, 0, 0, 0, 0);
    klog_printf("[DS4toDS5] reader malloc(%zu)=0x%lx stub_len=%zu\n",
                alloc_size, remote_block, stub_len);
    if (remote_block <= 0)
        goto cleanup;
    if (kernel_set_vmem_protection(target, remote_block, alloc_size,
                                   PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto cleanup;

    stub_addr = remote_block + (intptr_t)stub_off;
    args_addr = remote_block + (intptr_t)args_off;
    thread_addr = remote_block + (intptr_t)thread_off;
    if (pt_io_write(target, stub_addr, remote_pad_reader_stub, stub_len) != 0)
        goto cleanup;

    RemotePadReaderArgs args;
    memset(&args, 0, sizeof(args));
    args.fp_readstate = fn_readstate;
    args.fp_usleep = fn_usleep;
    args.pad_handle = handle;
    args.interval_us = 8333;
    args.last_result = -1;
    if (pt_io_write(target, args_addr, &args, sizeof(args)) != 0)
        goto cleanup;

    {
        int64_t result = pt_call(target, fn_pthread_create, trap_mem,
                                 (uint64_t)thread_addr, 0,
                                 (uint64_t)stub_addr, (uint64_t)args_addr,
                                 0, 0);
        klog_printf("[DS4toDS5] reader pthread_create=0x%llx\n",
                    (unsigned long long)(uint64_t)result);
        if (result != 0)
            goto cleanup;
    }
    launched = 1;

cleanup:
    if (!launched && remote_block > 0 && fn_free && trap_mem)
        (void)pt_call(target, fn_free, trap_mem,
                      (uint64_t)remote_block, 0, 0, 0, 0, 0);
    if (trap_saved && trap_writable)
        (void)pt_io_write(target, trap_mem, &original_trap_byte, 1);
    if (trap_writable)
        (void)kernel_set_vmem_protection(target, trap_mem, 16,
                                         trap_protection);
    if (attached)
        (void)sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);

    if (!launched)
        return -1;

    for (unsigned attempt = 0; attempt < 100; attempt++) {
        int32_t ready = 0;
        if (remote_reader_copyout(
                target,
                args_addr + (intptr_t)offsetof(RemotePadReaderArgs, ready),
                &ready, sizeof(ready)) == 0 && ready == 1) {
            if (out_pid) *out_pid = target;
            if (out_args_kaddr) *out_args_kaddr = args_addr;
            klog_printf("[DS4toDS5] reader ready pid=%d args=0x%lx\n",
                        target, args_addr);
            return 0;
        }
        usleep(10000);
    }

    klog_printf("[DS4toDS5] reader ready timeout\n");
    (void)wireless_ds4_remote_reader_stop(target, args_addr);
    return -1;
#endif
}

int
wireless_ds4_remote_reader_read(pid_t pid, intptr_t args_kaddr,
                               void *pad_data, uint32_t pad_data_len,
                               uint32_t *out_seq)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)args_kaddr; (void)pad_data;
    (void)pad_data_len; (void)out_seq;
    return -1;
#else
    if (!pad_data || !args_kaddr)
        return -1;
    if (pad_data_len > DS4TOD5_REMOTE_PAD_CAPACITY)
        pad_data_len = DS4TOD5_REMOTE_PAD_CAPACITY;

    for (unsigned attempt = 0; attempt < 3; attempt++) {
        RemotePadReaderArgs snapshot;
        uint32_t seq_after = 0;
        if (remote_reader_copyout(pid, args_kaddr, &snapshot,
                                  sizeof(snapshot)) != 0)
            return -1;
        if (remote_reader_copyout(
                pid,
                args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, seq),
                &seq_after, sizeof(seq_after)) != 0)
            return -1;
        if ((snapshot.seq & 1u) == 0 && snapshot.seq == seq_after &&
            snapshot.ready == 1 &&
            snapshot.last_result == 0 && snapshot.seq != 0) {
            memcpy(pad_data, snapshot.pad_data, pad_data_len);
            if (out_seq) *out_seq = snapshot.seq;
            return 0;
        }
    }
    return -1;
#endif
}

int
wireless_ds4_remote_reader_stop(pid_t pid, intptr_t args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)args_kaddr;
    return -1;
#else
    int32_t stop = 1;
    intptr_t stop_addr =
        args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, stop);
    intptr_t ready_addr =
        args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, ready);
    int write_result =
        remote_reader_copyin(pid, &stop, stop_addr, sizeof(stop));
    int32_t observed_stop = 0;
    int read_result =
        remote_reader_copyout(pid, stop_addr, &observed_stop,
                              sizeof(observed_stop));
    klog_printf("[DS4toDS5] reader stop: mdbg write=%d read=%d value=%d\n",
                write_result, read_result, observed_stop);

    if (write_result != 0 || read_result != 0 || observed_stop != 1) {
        klog_printf("[DS4toDS5] reader stop: using one-time ptrace write\n");
        if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0)
            return -1;
        if (waitpid(pid, NULL, 0) < 0) {
            (void)sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
            return -1;
        }
        int ptrace_result =
            pt_io_write(pid, stop_addr, &stop, sizeof(stop));
        (void)sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        if (ptrace_result != 0)
            return -1;
    }
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        int32_t ready = 0;
        if (remote_reader_copyout(pid, ready_addr, &ready,
                                  sizeof(ready)) == 0 && ready == 2)
            return 0;
        usleep(10000);
    }
    {
        int32_t ready = 0;
        int32_t final_stop = 0;
        (void)remote_reader_copyout(pid, ready_addr, &ready, sizeof(ready));
        (void)remote_reader_copyout(pid, stop_addr, &final_stop,
                                    sizeof(final_stop));
        klog_printf("[DS4toDS5] reader stop timeout: ready=%d stop=%d\n",
                    ready, final_stop);
    }
    return -1;
#endif
}

#define DS4TOD5_GAME_BRIDGE_FW_1160 UINT32_C(0x11600005)
#define DS4TOD5_READ_STATE_OFFSET_1160 UINT32_C(0x00002a80)
#define DS4TOD5_READ_STATE_FNV256_1160 UINT64_C(0xa4fea18d88eb7cc9)
#define DS4TOD5_READ_OFFSET_1160 UINT32_C(0x00002a90)
#define DS4TOD5_READ_FNV256_1160 UINT64_C(0x996f9a2e6fd4b22b)
#define DS4TOD5_READ_STATE_EXT_OFFSET_1160 UINT32_C(0x00002aa0)
#define DS4TOD5_READ_STATE_EXT_FNV256_1160 UINT64_C(0x54f5d565c9144b3e)
#define DS4TOD5_READ_EXT_OFFSET_1160 UINT32_C(0x00002ab0)
#define DS4TOD5_READ_EXT_FNV256_1160 UINT64_C(0x4d640c3e64e9029a)
#define DS4TOD5_DATA_INTERNAL_OFFSET_1160 UINT32_C(0x00000e10)
#define DS4TOD5_DATA_INTERNAL_FNV256_1160 \
    UINT64_C(0x644d37d059c8de86)
#define DS4TOD5_CONTROLLER_INFO_OFFSET_1160 UINT32_C(0x00004960)
#define DS4TOD5_CONTROLLER_INFO_FNV256_1160 \
    UINT64_C(0xb011e3f87d55e253)

static int
game_bridge_make_gateway(uint8_t gateway[16], intptr_t gateway_address,
                         intptr_t args_address, intptr_t stub_address,
                         int args_in_rcx)
{
    memset(gateway, 0x90, 16);
    gateway[0] = 0x48;
    gateway[1] = 0x8d;
    gateway[2] = args_in_rcx ? 0x0d : 0x15; /* lea rcx/rdx,[rip+disp32] */
    int64_t args_delta =
        (int64_t)args_address - (int64_t)(gateway_address + 7);
    int64_t stub_delta =
        (int64_t)stub_address - (int64_t)(gateway_address + 12);
    if (args_delta < INT32_MIN || args_delta > INT32_MAX ||
        stub_delta < INT32_MIN || stub_delta > INT32_MAX)
        return -1;
    int32_t args_rel = (int32_t)args_delta;
    int32_t stub_rel = (int32_t)stub_delta;
    memcpy(gateway + 3, &args_rel, sizeof(args_rel));
    gateway[7] = 0xe9;
    memcpy(gateway + 8, &stub_rel, sizeof(stub_rel));
    return 0;
}

static void
game_bridge_make_absolute_jump(uint8_t patch[16], intptr_t destination)
{
    memset(patch, 0x90, 16);
    patch[0] = 0x49;
    patch[1] = 0xbb; /* movabs r11, destination */
    uint64_t target = (uint64_t)destination;
    memcpy(patch + 2, &target, sizeof(target));
    patch[10] = 0x41;
    patch[11] = 0xff;
    patch[12] = 0xe3; /* jmp r11 */
}

/* Diagnostic-only wrapper-shape parser. Unknown firmware is never accepted
 * from these short patterns; they are recorded solely to help review a future
 * exact manifest without requiring an unrestricted memory dump. */
static int
game_bridge_nop_padding(const uint8_t *code, size_t length,
                        size_t *out_size)
{
    if (!code || !out_size || length == 0)
        return 0;
    if (code[0] == 0x90 || code[0] == 0xcc) {
        *out_size = 1;
        return 1;
    }
    static const uint8_t nops[][8] = {
        {0x66,0x90},
        {0x0f,0x1f,0x00},
        {0x0f,0x1f,0x40,0x00},
        {0x0f,0x1f,0x44,0x00,0x00},
        {0x66,0x0f,0x1f,0x44,0x00,0x00},
        {0x0f,0x1f,0x80,0x00,0x00,0x00,0x00},
        {0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00}
    };
    static const uint8_t sizes[] = {2,3,4,5,6,7,8};
    for (size_t index = 0; index < sizeof(sizes); ++index) {
        size_t size = sizes[index];
        if (length >= size && memcmp(code, nops[index], size) == 0) {
            *out_size = size;
            return 1;
        }
    }
    return 0;
}

/* Recognize the small ABI wrappers by their meaning, not only by the one
 * compiler spelling observed on 11.60.  kind 0/1 are the zeroing wrappers
 * for edx/ecx; kind 2/3/4 require the corresponding constant-one move.  The
 * accepted alternatives are deliberately finite and the tail must still be
 * only a direct/PLT jump plus padding. */
static int
game_bridge_report_wrapper_shape(const uint8_t *code, unsigned kind,
                                 size_t *out_jump_offset)
{
    if (!code || !out_jump_offset)
        return 0;
    size_t jump_offset = 0;
    if (kind == 0 || kind == 1) {
        uint8_t reg = kind == 0 ? 0xd2 : 0xc9;
        if (code[0] == 0x31 && code[1] == reg) {
            jump_offset = 2;
        } else if (code[0] == 0x29 && code[1] == reg) {
            /* sub edx,edx / sub ecx,ecx */
            jump_offset = 2;
        } else if (code[0] == (kind == 0 ? 0xba : 0xb9) &&
                   code[1] == 0x00 && code[2] == 0x00 &&
                   code[3] == 0x00 && code[4] == 0x00) {
            /* mov edx,0 / mov ecx,0 */
            jump_offset = 5;
        } else {
            return 0;
        }
    } else {
        uint8_t opcode = (kind == 2 || kind == 4) ? 0xba : 0xb9;
        if (code[0] != opcode || code[1] != 0x01 || code[2] != 0x00 ||
            code[3] != 0x00 || code[4] != 0x00)
            return 0;
        jump_offset = 5;
    }
    if (jump_offset + 5u > 16u)
        return 0;
    size_t jump_length = 0;
    if (code[jump_offset] == 0xe9) {
        jump_length = 5;
    } else if (jump_offset + 6u <= 16u &&
               code[jump_offset] == 0xff &&
               code[jump_offset + 1] == 0x25) {
        jump_length = 6;
    } else {
        return 0;
    }
    size_t padding = jump_offset + jump_length;
    while (padding < 16u) {
        size_t nop_size = 0;
        if (!game_bridge_nop_padding(code + padding, 16u - padding,
                                     &nop_size))
            return 0;
        padding += nop_size;
    }
    *out_jump_offset = jump_offset;
    return 1;
}

static int
game_bridge_target_within_sane_range(intptr_t base, intptr_t target)
{
    if (base <= 0 || target < base)
        return 0;
    uintptr_t delta = (uintptr_t)(target - base);
    /* Keep a malformed rel32 from escaping into another mapping. */
    return delta < (uintptr_t)0x01000000u;
}

static int
game_bridge_report_controller_info_shape(const uint8_t *code)
{
    static const uint8_t prefix[16] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec};
    return code && memcmp(code, prefix, sizeof(prefix)) == 0;
}

/* Keep rejected-firmware reports useful to the person adding the next
 * manifest.  Ghostpad's portability work is driven by short signatures, not
 * by assuming that a symbol name implies an unchanged ABI.  Capture a
 * bounded prefix before any target write; this is diagnostic-only and is
 * intentionally not used as an acceptance rule. */
static void
game_bridge_report_prefix(int fd, const char *name,
                          const uint8_t *code, size_t length)
{
    if (fd < 0 || !name || !code)
        return;
    size_t count = length < 64u ? length : 64u;
    report_printf(fd, "%s=", name);
    for (size_t index = 0; index < count; ++index)
        report_printf(fd, "%02x", code[index]);
    report_printf(fd, "\n");
}

/* Resolve either a direct rel32 wrapper or a six-byte RIP-relative PLT jump.
 * The latter is resolved while the target is stopped, so a stale or malformed
 * GOT entry cannot race the structural checks below. */
static int
game_bridge_wrapper_target(pid_t target, intptr_t function,
                           const uint8_t *code, size_t jump_offset,
                           intptr_t *out_target)
{
#if !defined(__PROSPERO__)
    (void)target; (void)function; (void)code; (void)jump_offset;
    (void)out_target;
    return -1;
#else
    if (!code || !out_target || jump_offset + 5u > 256u)
        return -1;
    if (code[jump_offset] == 0xe9) {
        int32_t displacement = 0;
        memcpy(&displacement, code + jump_offset + 1,
               sizeof(displacement));
        *out_target = function + (intptr_t)jump_offset + 5 + displacement;
        return *out_target > 0 ? 0 : -1;
    }
    if (jump_offset + 6u <= 256u &&
        code[jump_offset] == 0xff &&
        code[jump_offset + 1] == 0x25) {
        int32_t displacement = 0;
        memcpy(&displacement, code + jump_offset + 2,
               sizeof(displacement));
        intptr_t slot = function + (intptr_t)jump_offset + 6 + displacement;
        uint64_t pointer = 0;
        if (slot <= 0 || mdbg_copyout(
                target, slot, &pointer, sizeof(pointer)) != 0 ||
            pointer == 0 || pointer > (uint64_t)INTPTR_MAX)
            return -1;
        *out_target = (intptr_t)pointer;
        return 0;
    }
    return -1;
#endif
}

int
wireless_ds4_game_bridge_install(int32_t user_id, pid_t *out_game_pid,
                                intptr_t *out_args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)user_id; (void)out_game_pid; (void)out_args_kaddr;
    return -1;
#else
    pid_t pids[8];
    int report_fd = -1;
    int attached = 0;
    int trap_saved = 0;
    int trap_writable = 0;
    int wrappers_writable = 0;
    int data_wrapper_writable = 0;
    int info_function_writable = 0;
    int info_function_patched = 0;
    int patched_count = 0;
    int receiver_launched = 0;
    int wake_receiver = 0;
    uint8_t original_trap = 0;
    int trap_protection = PROT_READ | PROT_EXEC;
    int wrapper_protection = PROT_READ | PROT_EXEC;
    int data_wrapper_protection = PROT_READ | PROT_EXEC;
    int info_function_protection = PROT_READ | PROT_EXEC;
    intptr_t trap = 0;
    intptr_t remote_block = 0;
    intptr_t args_address = 0;
    intptr_t fn_free = 0;
    size_t alloc_size = 0;
    int result = -1;
    uint32_t firmware = kernel_get_fw_version();
    int exact_manifest = 0;

    if (out_game_pid) *out_game_pid = -1;
    if (out_args_kaddr) *out_args_kaddr = 0;
    mkdir("/data/ds4tod5", 0755);
    report_fd = open(
        "/data/ds4tod5/game-pad-bridge-last.txt",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    report_printf(
        report_fd, "firmware=0x%08x\nuser_id=0x%08x\n",
        firmware, (uint32_t)user_id);

    size_t process_count = find_pids("eboot.bin", pids, 8);
    report_printf(report_fd, "process_count=%zu\n", process_count);
    if (process_count == 0) {
        report_printf(report_fd, "state=waiting_for_game\n");
        result = -2;
        goto done;
    }
    if (process_count != 1) {
        report_printf(report_fd, "error=process_count\n");
        result = -3;
        goto done;
    }
    pid_t target = pids[0];
    if (out_game_pid) *out_game_pid = target;
    report_printf(report_fd, "pid=%d\n", target);

    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        report_printf(report_fd, "error=attach errno=%d\n", errno);
        goto done;
    }
    attached = 1;
    if (waitpid(target, NULL, 0) < 0) {
        report_printf(report_fd, "error=wait_attach errno=%d\n", errno);
        goto cleanup;
    }

    uint32_t libpad_handle = 0;
    uint32_t libc_handle = 0;
    uint32_t libkernel_handle = 0;
    const char *libkernel_name = "libkernel_sys";
    if (get_lib(target, "libScePad", &libpad_handle) != 0 ||
        get_lib(target, "libSceLibcInternal", &libc_handle) != 0) {
        report_printf(report_fd, "error=required_module\n");
        goto cleanup;
    }
    if (get_lib(target, "libkernel_sys", &libkernel_handle) != 0) {
        libkernel_name = "libkernel";
        if (get_lib(target, libkernel_name, &libkernel_handle) != 0) {
            report_printf(report_fd, "error=required_kernel_module\n");
            goto cleanup;
        }
    }
    intptr_t base = kernel_dynlib_mapbase_addr(target, libpad_handle);
    intptr_t read_state =
        resolve_sym(target, libpad_handle, "scePadReadState");
    intptr_t read_state_ext =
        resolve_sym(target, libpad_handle, "scePadReadStateExt");
    intptr_t read_fn = resolve_sym(target, libpad_handle, "scePadRead");
    intptr_t read_ext = resolve_sym(target, libpad_handle, "scePadReadExt");
    intptr_t data_internal =
        resolve_sym(target, libpad_handle, "scePadGetDataInternal");
    intptr_t controller_info = resolve_sym(
        target, libpad_handle, "scePadGetControllerInformation");
    intptr_t get_handle =
        resolve_sym(target, libpad_handle, "scePadGetHandle");
    intptr_t fn_malloc = resolve_sym(target, libc_handle, "malloc");
    fn_free = resolve_sym(target, libc_handle, "free");
    intptr_t fn_socket = resolve_sym(target, libkernel_handle, "socket");
    intptr_t fn_bind = resolve_sym(target, libkernel_handle, "bind");
    intptr_t fn_recvfrom =
        resolve_sym(target, libkernel_handle, "recvfrom");
    intptr_t fn_close = resolve_sym(target, libkernel_handle, "close");
    intptr_t fn_pthread_create =
        resolve_sym(target, libkernel_handle, "pthread_create");
    if (!fn_pthread_create)
        fn_pthread_create =
            resolve_sym(target, libkernel_handle, "scePthreadCreate");
    uint8_t read_state_code[256];
    uint8_t read_state_ext_code[256];
    uint8_t read_code[256];
    uint8_t read_ext_code[256];
    uint8_t data_internal_code[256];
    uint8_t controller_info_code[256];
    if (!base || !read_state || !read_state_ext || !read_fn || !read_ext ||
        !data_internal || !controller_info ||
        !get_handle || !fn_malloc || !fn_free || !fn_socket || !fn_bind ||
        !fn_recvfrom || !fn_close || !fn_pthread_create ||
        mdbg_copyout(target, read_state, read_state_code,
                     sizeof(read_state_code)) != 0 ||
        mdbg_copyout(target, read_state_ext, read_state_ext_code,
                     sizeof(read_state_ext_code)) != 0 ||
        mdbg_copyout(target, read_fn, read_code, sizeof(read_code)) != 0 ||
        mdbg_copyout(target, read_ext, read_ext_code,
                     sizeof(read_ext_code)) != 0 ||
        mdbg_copyout(target, data_internal, data_internal_code,
                     sizeof(data_internal_code)) != 0 ||
        mdbg_copyout(target, controller_info, controller_info_code,
                     sizeof(controller_info_code)) != 0) {
        report_printf(report_fd, "error=resolve_or_copy errno=%d\n", errno);
        goto cleanup;
    }
    report_printf(
        report_fd,
        "kernel_module=%s handle=0x%x\n"
        "socket=0x%lx bind=0x%lx recvfrom=0x%lx close=0x%lx "
        "pthread_create=0x%lx\n",
        libkernel_name, libkernel_handle,
        (unsigned long)fn_socket, (unsigned long)fn_bind,
        (unsigned long)fn_recvfrom, (unsigned long)fn_close,
        (unsigned long)fn_pthread_create);

    uint64_t read_state_hash =
        ghostpad_fnv1a64(read_state_code, sizeof(read_state_code));
    uint64_t read_state_ext_hash =
        ghostpad_fnv1a64(read_state_ext_code, sizeof(read_state_ext_code));
    uint64_t read_hash = ghostpad_fnv1a64(read_code, sizeof(read_code));
    uint64_t read_ext_hash =
        ghostpad_fnv1a64(read_ext_code, sizeof(read_ext_code));
    uint64_t data_internal_hash =
        ghostpad_fnv1a64(data_internal_code, sizeof(data_internal_code));
    uint64_t controller_info_hash =
        ghostpad_fnv1a64(controller_info_code,
                         sizeof(controller_info_code));
    report_printf(
        report_fd,
        "libpad_base=0x%lx\n"
        "read_state_offset=0x%lx read_state_fnv256=0x%016llx\n"
        "read_state_ext_offset=0x%lx read_state_ext_fnv256=0x%016llx\n"
        "read_offset=0x%lx read_fnv256=0x%016llx\n"
        "read_ext_offset=0x%lx read_ext_fnv256=0x%016llx\n"
        "data_internal_offset=0x%lx data_internal_fnv256=0x%016llx\n"
        "controller_info_offset=0x%lx "
        "controller_info_fnv256=0x%016llx\n",
        (unsigned long)base,
        (unsigned long)(read_state - base),
        (unsigned long long)read_state_hash,
        (unsigned long)(read_state_ext - base),
        (unsigned long long)read_state_ext_hash,
        (unsigned long)(read_fn - base),
        (unsigned long long)read_hash,
        (unsigned long)(read_ext - base),
        (unsigned long long)read_ext_hash,
        (unsigned long)(data_internal - base),
        (unsigned long long)data_internal_hash,
        (unsigned long)(controller_info - base),
        (unsigned long long)controller_info_hash);
    game_bridge_report_prefix(report_fd, "read_state_prefix",
                              read_state_code, sizeof(read_state_code));
    game_bridge_report_prefix(report_fd, "read_state_ext_prefix",
                              read_state_ext_code,
                              sizeof(read_state_ext_code));
    game_bridge_report_prefix(report_fd, "read_prefix",
                              read_code, sizeof(read_code));
    game_bridge_report_prefix(report_fd, "read_ext_prefix",
                              read_ext_code, sizeof(read_ext_code));
    game_bridge_report_prefix(report_fd, "data_internal_prefix",
                              data_internal_code,
                              sizeof(data_internal_code));
    game_bridge_report_prefix(report_fd, "controller_info_prefix64",
                              controller_info_code,
                              sizeof(controller_info_code));
    static const uint8_t expected_read_state[16] = {
        0x31,0xd2,0xe9,0x89,0xeb,0xff,0xff,0xcc,
        0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
    static const uint8_t expected_read[16] = {
        0x31,0xc9,0xe9,0xd9,0xed,0xff,0xff,0xcc,
        0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
    static const uint8_t expected_read_state_ext[16] = {
        0xba,0x01,0x00,0x00,0x00,0xe9,0x66,0xeb,
        0xff,0xff,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
    static const uint8_t expected_read_ext[16] = {
        0xb9,0x01,0x00,0x00,0x00,0xe9,0xb6,0xed,
        0xff,0xff,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
    static const uint8_t expected_data_internal[16] = {
        0xba,0x01,0x00,0x00,0x00,0xe9,0x96,0xfe,
        0xff,0xff,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
    static const uint8_t expected_controller_info_prologue[20] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,
        0xd8,0x00,0x00,0x00};
    exact_manifest = firmware == DS4TOD5_GAME_BRIDGE_FW_1160 &&
        (uint32_t)(read_state - base) ==
            DS4TOD5_READ_STATE_OFFSET_1160 &&
        read_state_hash == DS4TOD5_READ_STATE_FNV256_1160 &&
        (uint32_t)(read_state_ext - base) ==
            DS4TOD5_READ_STATE_EXT_OFFSET_1160 &&
        read_state_ext_hash == DS4TOD5_READ_STATE_EXT_FNV256_1160 &&
        (uint32_t)(read_fn - base) == DS4TOD5_READ_OFFSET_1160 &&
        read_hash == DS4TOD5_READ_FNV256_1160 &&
        (uint32_t)(read_ext - base) == DS4TOD5_READ_EXT_OFFSET_1160 &&
        read_ext_hash == DS4TOD5_READ_EXT_FNV256_1160 &&
        (uint32_t)(data_internal - base) ==
            DS4TOD5_DATA_INTERNAL_OFFSET_1160 &&
        data_internal_hash == DS4TOD5_DATA_INTERNAL_FNV256_1160 &&
        (uint32_t)(controller_info - base) ==
            DS4TOD5_CONTROLLER_INFO_OFFSET_1160 &&
        controller_info_hash == DS4TOD5_CONTROLLER_INFO_FNV256_1160;

    int exact_wrapper_bytes =
        memcmp(read_state_code, expected_read_state, 16) == 0 &&
        memcmp(read_code, expected_read, 16) == 0 &&
        memcmp(read_state_ext_code, expected_read_state_ext, 16) == 0 &&
        memcmp(read_ext_code, expected_read_ext, 16) == 0 &&
        memcmp(data_internal_code, expected_data_internal, 16) == 0 &&
        memcmp(controller_info_code,
               expected_controller_info_prologue,
               sizeof(expected_controller_info_prologue)) == 0;
    size_t read_state_jump_offset = 2u;
    size_t read_state_ext_jump_offset = 5u;
    size_t read_jump_offset = 2u;
    size_t read_ext_jump_offset = 5u;
    size_t data_internal_jump_offset = 5u;
    /* Keep an actionable read-only fingerprint when a new firmware is
     * rejected.  These bytes are captured before any target write and let a
     * future manifest be added without guessing from a bare failure. */
    report_printf(
        report_fd,
        "wrapper_shapes=state:%d state_ext:%d read:%d read_ext:%d "
        "data:%d info:%d\n"
        "info_prefix=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        game_bridge_report_wrapper_shape(
            read_state_code, 0, &read_state_jump_offset),
        game_bridge_report_wrapper_shape(
            read_state_ext_code, 2, &read_state_ext_jump_offset),
        game_bridge_report_wrapper_shape(
            read_code, 1, &read_jump_offset),
        game_bridge_report_wrapper_shape(
            read_ext_code, 3, &read_ext_jump_offset),
        game_bridge_report_wrapper_shape(
            data_internal_code, 4, &data_internal_jump_offset),
        game_bridge_report_controller_info_shape(controller_info_code),
        controller_info_code[0], controller_info_code[1],
        controller_info_code[2], controller_info_code[3],
        controller_info_code[4], controller_info_code[5],
        controller_info_code[6], controller_info_code[7],
        controller_info_code[8], controller_info_code[9],
        controller_info_code[10], controller_info_code[11],
        controller_info_code[12], controller_info_code[13],
        controller_info_code[14], controller_info_code[15]);
    report_printf(
        report_fd,
        "wrapper_jump_offsets=state:%zu state_ext:%zu read:%zu "
        "read_ext:%zu data:%zu\n",
        read_state_jump_offset, read_state_ext_jump_offset,
        read_jump_offset, read_ext_jump_offset,
        data_internal_jump_offset);
    if (!exact_manifest) {
        report_printf(report_fd,
                      "error=unsupported_firmware_manifest\n");
        result = 0;
        goto cleanup;
    }
    if (!exact_wrapper_bytes) {
        report_printf(report_fd, "error=wrapper_bytes_mismatch\n");
        result = 0;
        goto cleanup;
    }
    report_printf(report_fd, "manifest_mode=11.60-exact\n");

    intptr_t state_internal = 0;
    intptr_t state_ext_internal = 0;
    intptr_t read_internal = 0;
    intptr_t read_ext_internal = 0;
    intptr_t data_internal_target = 0;
    int wrapper_targets_ok =
        game_bridge_wrapper_target(
            target, read_state, read_state_code, read_state_jump_offset,
            &state_internal) == 0 &&
        game_bridge_wrapper_target(
            target, read_state_ext, read_state_ext_code,
            read_state_ext_jump_offset,
            &state_ext_internal) == 0 &&
        game_bridge_wrapper_target(
            target, read_fn, read_code, read_jump_offset,
            &read_internal) == 0 &&
        game_bridge_wrapper_target(
            target, read_ext, read_ext_code, read_ext_jump_offset,
            &read_ext_internal) == 0 &&
        game_bridge_wrapper_target(
            target, data_internal, data_internal_code,
            data_internal_jump_offset,
            &data_internal_target) == 0;
    report_printf(
        report_fd,
        "state_internal=0x%lx state_ext_internal=0x%lx\n"
        "read_internal=0x%lx read_ext_internal=0x%lx\n"
        "data_internal_target=0x%lx\n",
        (unsigned long)state_internal,
        (unsigned long)state_ext_internal,
        (unsigned long)read_internal,
        (unsigned long)read_ext_internal,
        (unsigned long)data_internal_target);
    if (!wrapper_targets_ok ||
        state_internal != state_ext_internal ||
        read_internal != read_ext_internal ||
        !game_bridge_target_within_sane_range(base, state_internal) ||
        !game_bridge_target_within_sane_range(base, read_internal) ||
        !game_bridge_target_within_sane_range(base, data_internal_target) ||
        (uint32_t)(state_internal - base) != 0x1610u ||
        (uint32_t)(read_internal - base) != 0x1870u ||
        (uint32_t)(data_internal_target - base) != 0x0cb0u) {
        report_printf(report_fd, "error=internal_target_mismatch\n");
        result = 0;
        goto cleanup;
    }

    trap = kernel_dynlib_fini_addr(target, libpad_handle);
    if (!trap)
        trap = kernel_dynlib_init_addr(target, libpad_handle);
    if (!trap || mdbg_copyout(target, trap, &original_trap, 1) != 0) {
        report_printf(report_fd, "error=trap_discovery errno=%d\n", errno);
        goto cleanup;
    }
    trap_saved = 1;
    int protection = kernel_get_vmem_protection(target, trap, 1);
    if (protection >= 0)
        trap_protection = protection;
    if (kernel_set_vmem_protection(
            target, trap, 16,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        report_printf(report_fd, "error=trap_protection errno=%d\n", errno);
        goto cleanup;
    }
    trap_writable = 1;
    uint8_t int3 = 0xcc;
    if (pt_io_write(target, trap, &int3, 1) != 0) {
        report_printf(report_fd, "error=trap_write errno=%d\n", errno);
        goto cleanup;
    }

    int32_t pad_handle = (int32_t)pt_call(
        target, get_handle, trap,
        (uint32_t)user_id, 0, 0, 0, 0, 0);
    report_printf(report_fd, "type0_handle=0x%08x\n", (uint32_t)pad_handle);
    if (pad_handle < 0) {
        report_printf(report_fd, "state=waiting_for_type0_handle\n");
        result = -4;
        goto cleanup;
    }
    int table_matches = 0;
    for (unsigned slot = 0; slot < 16; ++slot) {
        uint32_t candidate = 0;
        intptr_t address =
            base + UINT32_C(0x20018) + UINT32_C(0x1c) +
            (intptr_t)slot * UINT32_C(0x5c8);
        if (mdbg_copyout(target, address, &candidate,
                         sizeof(candidate)) != 0) {
            report_printf(report_fd, "error=table_copy slot=%u\n", slot);
            goto cleanup;
        }
        if (candidate == (uint32_t)pad_handle)
            table_matches++;
    }
    report_printf(report_fd, "type0_table_matches=%d\n", table_matches);
    if (table_matches != 1) {
        report_printf(report_fd, "error=ambiguous_type0_handle\n");
        result = 0;
        goto cleanup;
    }

    uintptr_t local_stub_base = (uintptr_t)game_pad_read_state_stub;
    uintptr_t local_stub_end = (uintptr_t)game_pad_bridge_stub_end;
    const uintptr_t local_stub_addresses[6] = {
        (uintptr_t)game_pad_read_state_stub,
        (uintptr_t)game_pad_read_state_ext_stub,
        (uintptr_t)game_pad_read_stub,
        (uintptr_t)game_pad_read_ext_stub,
        (uintptr_t)game_pad_get_data_internal_stub,
        (uintptr_t)game_pad_get_controller_info_stub};
    uintptr_t local_receiver_address =
        (uintptr_t)game_pad_bridge_receiver_stub;
    if (local_stub_end <= local_stub_base) {
        report_printf(report_fd, "error=stub_layout\n");
        goto cleanup;
    }
    for (unsigned index = 0; index < 6; ++index) {
        if (local_stub_addresses[index] < local_stub_base ||
            local_stub_addresses[index] >= local_stub_end) {
            report_printf(report_fd, "error=stub_order index=%u\n", index);
            goto cleanup;
        }
    }
    if (local_receiver_address < local_stub_base ||
        local_receiver_address >= local_stub_end) {
        report_printf(report_fd, "error=receiver_stub_order\n");
        goto cleanup;
    }
    size_t stub_size = (size_t)(local_stub_end - local_stub_base);
    size_t gateway_offset = (stub_size + 15u) & ~(size_t)15u;
    size_t info_trampoline_offset =
        (gateway_offset + 6u * 16u + 15u) & ~(size_t)15u;
    size_t args_offset =
        (info_trampoline_offset + 48u + 15u) & ~(size_t)15u;
    size_t thread_offset =
        (args_offset + sizeof(GamePadBridgeArgs) + 15u) & ~(size_t)15u;
    alloc_size = thread_offset + 16u;
    remote_block = (intptr_t)pt_call(
        target, fn_malloc, trap, alloc_size, 0, 0, 0, 0, 0);
    report_printf(
        report_fd, "stub_size=%zu alloc_size=%zu remote_block=0x%lx\n",
        stub_size, alloc_size, (unsigned long)remote_block);
    if (remote_block <= 0 ||
        kernel_set_vmem_protection(
            target, remote_block, alloc_size,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        report_printf(report_fd, "error=remote_alloc_or_protection errno=%d\n",
                      errno);
        goto cleanup;
    }
    if (pt_io_write(target, remote_block,
                    (const void *)local_stub_base, stub_size) != 0) {
        report_printf(report_fd, "error=stub_write errno=%d\n", errno);
        goto cleanup;
    }
    args_address = remote_block + (intptr_t)args_offset;
    intptr_t remote_stub_addresses[6];
    for (unsigned index = 0; index < 6; ++index) {
        remote_stub_addresses[index] =
            remote_block +
            (intptr_t)(local_stub_addresses[index] - local_stub_base);
    }
    static const uint8_t args_in_rcx[6] = {0, 0, 1, 1, 0, 0};
    for (unsigned index = 0; index < 6; ++index) {
        intptr_t gateway_address =
            remote_block + (intptr_t)gateway_offset +
            (intptr_t)index * 16;
        uint8_t gateway[16];
        if (game_bridge_make_gateway(
                gateway, gateway_address, args_address,
                remote_stub_addresses[index], args_in_rcx[index]) != 0 ||
            pt_io_write(target, gateway_address, gateway,
                        sizeof(gateway)) != 0) {
            report_printf(report_fd, "error=gateway index=%u errno=%d\n",
                          index, errno);
            goto cleanup;
        }
    }
    intptr_t info_trampoline_address =
        remote_block + (intptr_t)info_trampoline_offset;
    uint8_t info_trampoline[48];
    memset(info_trampoline, 0x90, sizeof(info_trampoline));
    memcpy(info_trampoline, controller_info_code,
           sizeof(expected_controller_info_prologue));
    game_bridge_make_absolute_jump(
        info_trampoline + sizeof(expected_controller_info_prologue),
        controller_info +
            (intptr_t)sizeof(expected_controller_info_prologue));
    if (pt_io_write(
            target, info_trampoline_address,
            info_trampoline, sizeof(info_trampoline)) != 0) {
        report_printf(report_fd, "error=info_trampoline_write errno=%d\n",
                      errno);
        goto cleanup;
    }

    GamePadBridgeArgs args;
    memset(&args, 0, sizeof(args));
    args.magic = DS4TOD5_GAME_BRIDGE_MAGIC;
    args.pad_size = DS4TOD5_GAME_BRIDGE_PAD_SIZE;
    args.pad_handle = pad_handle;
    args.fp_state_internal = state_internal;
    args.fp_read_internal = read_internal;
    args.fp_data_internal = data_internal_target;
    args.fp_get_controller_info_trampoline =
        info_trampoline_address;
    args.fp_socket = fn_socket;
    args.fp_bind = fn_bind;
    args.fp_recvfrom = fn_recvfrom;
    args.fp_close = fn_close;
    args.remote_block = remote_block;
    args.remote_block_size = (uint32_t)alloc_size;
    args.read_state_address = read_state;
    args.read_state_ext_address = read_state_ext;
    args.read_address = read_fn;
    args.read_ext_address = read_ext;
    args.data_internal_address = data_internal;
    args.controller_info_address = controller_info;
    args.controller_info_trampoline = info_trampoline_address;
    args.controller_info_gateway =
        remote_block + (intptr_t)gateway_offset + 5 * 16;
    args.receiver_port = DS4TOD5_GAME_BRIDGE_PORT;
    memcpy(args.original_read_state, read_state_code, 16);
    memcpy(args.original_read_state_ext, read_state_ext_code, 16);
    memcpy(args.original_read, read_code, 16);
    memcpy(args.original_read_ext, read_ext_code, 16);
    memcpy(args.original_data_internal, data_internal_code, 16);
    memcpy(args.original_controller_info, controller_info_code,
           sizeof(args.original_controller_info));
    wrapper_protection = kernel_get_vmem_protection(target, read_state, 64);
    if (wrapper_protection < 0)
        wrapper_protection = PROT_READ | PROT_EXEC;
    data_wrapper_protection =
        kernel_get_vmem_protection(target, data_internal, 16);
    if (data_wrapper_protection < 0)
        data_wrapper_protection = PROT_READ | PROT_EXEC;
    args.original_protection = (uint32_t)wrapper_protection;
    args.original_data_protection =
        (uint32_t)data_wrapper_protection;
    info_function_protection = kernel_get_vmem_protection(
        target, controller_info,
        sizeof(expected_controller_info_prologue));
    if (info_function_protection < 0)
        info_function_protection = PROT_READ | PROT_EXEC;
    args.original_info_function_protection =
        (uint32_t)info_function_protection;
    if (pt_io_write(target, args_address, &args, sizeof(args)) != 0) {
        report_printf(report_fd, "error=args_write errno=%d\n", errno);
        goto cleanup;
    }
    intptr_t receiver_address =
        remote_block +
        (intptr_t)(local_receiver_address - local_stub_base);
    intptr_t thread_address = remote_block + (intptr_t)thread_offset;
    int64_t pthread_result = pt_call(
        target, fn_pthread_create, trap,
        (uint64_t)thread_address, 0,
        (uint64_t)receiver_address, (uint64_t)args_address,
        0, 0);
    report_printf(
        report_fd,
        "receiver_address=0x%lx thread_address=0x%lx "
        "pthread_create=0x%llx\n",
        (unsigned long)receiver_address,
        (unsigned long)thread_address,
        (unsigned long long)(uint64_t)pthread_result);
    if (pthread_result != 0) {
        report_printf(report_fd, "error=receiver_thread_create\n");
        goto cleanup;
    }
    receiver_launched = 1;

    if (kernel_set_vmem_protection(
            target, read_state, 64,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        report_printf(report_fd, "error=wrapper_protection errno=%d\n", errno);
        goto cleanup;
    }
    wrappers_writable = 1;
    if (kernel_set_vmem_protection(
            target, data_internal, 16,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        report_printf(
            report_fd, "error=data_wrapper_protection errno=%d\n",
            errno);
        goto cleanup;
    }
    data_wrapper_writable = 1;
    const intptr_t wrapper_addresses[5] = {
        read_state, read_state_ext, read_fn, read_ext, data_internal};
    for (unsigned index = 0; index < 5; ++index) {
        uint8_t patch[16];
        intptr_t gateway_address =
            remote_block + (intptr_t)gateway_offset +
            (intptr_t)index * 16;
        game_bridge_make_absolute_jump(patch, gateway_address);
        if (pt_io_write(target, wrapper_addresses[index], patch,
                        sizeof(patch)) != 0) {
            report_printf(report_fd, "error=patch index=%u errno=%d\n",
                          index, errno);
            goto cleanup;
        }
        patched_count++;
    }
    if (kernel_set_vmem_protection(
            target, controller_info,
            sizeof(expected_controller_info_prologue),
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        report_printf(
            report_fd, "error=info_function_protection errno=%d\n", errno);
        goto cleanup;
    }
    info_function_writable = 1;
    uint8_t info_patch[20];
    memset(info_patch, 0x90, sizeof(info_patch));
    game_bridge_make_absolute_jump(
        info_patch, args.controller_info_gateway);
    if (pt_io_write(
            target, controller_info, info_patch,
            sizeof(info_patch)) != 0) {
        report_printf(report_fd, "error=info_function_patch errno=%d\n",
                      errno);
        goto cleanup;
    }
    info_function_patched = 1;
    if (kernel_set_vmem_protection(
            target, controller_info,
            sizeof(expected_controller_info_prologue),
            info_function_protection) != 0) {
        report_printf(
            report_fd, "error=info_function_reprotect errno=%d\n", errno);
        goto cleanup;
    }
    info_function_writable = 0;
    result = 1;
    report_printf(
        report_fd,
        "args_address=0x%lx\npad_handle=0x%08x\n"
        "patched_read_wrappers=5\n"
        "controller_info_function_patched=1\n"
        "game_import_layout_dependency=none\nresult=1\n",
        (unsigned long)args_address, (uint32_t)pad_handle);
    if (out_game_pid) *out_game_pid = target;
    if (out_args_kaddr) *out_args_kaddr = args_address;

cleanup:
    if (result != 1 && receiver_launched && args_address > 0) {
        int32_t stop_receiver = 1;
        (void)pt_io_write(
            target,
            args_address +
                (intptr_t)offsetof(GamePadBridgeArgs, receiver_stop),
            &stop_receiver, sizeof(stop_receiver));
        wake_receiver = 1;
    }
    if (result != 1 && patched_count > 0) {
        const intptr_t cleanup_addresses[5] = {
            read_state, read_state_ext, read_fn, read_ext, data_internal};
        const uint8_t *originals[5] = {
            read_state_code, read_state_ext_code, read_code, read_ext_code,
            data_internal_code};
        for (int index = patched_count - 1; index >= 0; --index)
            (void)pt_io_write(target, cleanup_addresses[index],
                              originals[index], 16);
    }
    if (result != 1 && info_function_patched) {
        if (!info_function_writable &&
            kernel_set_vmem_protection(
                target, controller_info,
                sizeof(expected_controller_info_prologue),
                PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
            info_function_writable = 1;
        if (info_function_writable)
            (void)pt_io_write(
                target, controller_info, controller_info_code,
                sizeof(expected_controller_info_prologue));
    }
    if (info_function_writable)
        (void)kernel_set_vmem_protection(
            target, controller_info,
            sizeof(expected_controller_info_prologue),
            info_function_protection);
    if (wrappers_writable)
        (void)kernel_set_vmem_protection(
            target, read_state, 64, wrapper_protection);
    if (data_wrapper_writable)
        (void)kernel_set_vmem_protection(
            target, data_internal, 16, data_wrapper_protection);
    if (result != 1 && !receiver_launched && remote_block > 0 &&
        fn_free && trap)
        (void)pt_call(target, fn_free, trap,
                      (uint64_t)remote_block, 0, 0, 0, 0, 0);
    if (trap_saved && trap_writable)
        (void)pt_io_write(target, trap, &original_trap, 1);
    if (trap_writable)
        (void)kernel_set_vmem_protection(
            target, trap, 16, trap_protection);
    if (attached)
        (void)sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
    if (wake_receiver)
        (void)game_bridge_send_stop();
done:
    if (result != 1)
        report_printf(report_fd, "result=%d\n", result);
    if (report_fd >= 0)
        close(report_fd);
    return result;
#endif
}

static int
game_bridge_send_stop(void)
{
    if (g_game_bridge_sender_fd < 0)
        g_game_bridge_sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_game_bridge_sender_fd < 0)
        return -1;

    struct {
        uint8_t length;
        uint8_t family;
        uint16_t port;
        uint32_t address;
        uint8_t zero[8];
    } loopback;
    memset(&loopback, 0, sizeof(loopback));
    loopback.length = sizeof(loopback);
    loopback.family = AF_INET;
    loopback.port = (uint16_t)(
        ((DS4TOD5_GAME_BRIDGE_PORT & 0xffu) << 8) |
        ((DS4TOD5_GAME_BRIDGE_PORT >> 8) & 0xffu));
    loopback.address = UINT32_C(0x0100007f);
    uint32_t stop_magic = DS4TOD5_GAME_BRIDGE_STOP_MAGIC;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        ssize_t sent = sendto(
            g_game_bridge_sender_fd, &stop_magic, sizeof(stop_magic), 0,
            (const struct sockaddr *)(const void *)&loopback,
            sizeof(loopback));
        if (sent == (ssize_t)sizeof(stop_magic))
            return 0;
        usleep(10000);
    }
    return -1;
}

int
wireless_ds4_game_bridge_update(pid_t game_pid, intptr_t args_kaddr,
                               const void *pad_data,
                               uint32_t pad_data_len)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr; (void)pad_data; (void)pad_data_len;
    return -1;
#else
    if (!args_kaddr || !pad_data ||
        pad_data_len < DS4TOD5_GAME_BRIDGE_PAD_SIZE)
        return -1;
    (void)game_pid;
    if (g_game_bridge_sender_fd < 0) {
        g_game_bridge_sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_game_bridge_sender_fd < 0)
            return -1;
    }
    struct {
        uint8_t length;
        uint8_t family;
        uint16_t port;
        uint32_t address;
        uint8_t zero[8];
    } loopback;
    memset(&loopback, 0, sizeof(loopback));
    loopback.length = sizeof(loopback);
    loopback.family = AF_INET;
    loopback.port = (uint16_t)(
        ((DS4TOD5_GAME_BRIDGE_PORT & 0xffu) << 8) |
        ((DS4TOD5_GAME_BRIDGE_PORT >> 8) & 0xffu));
    loopback.address = UINT32_C(0x0100007f);
    ssize_t sent = sendto(
        g_game_bridge_sender_fd, pad_data,
        DS4TOD5_GAME_BRIDGE_PAD_SIZE, 0,
        (const struct sockaddr *)(const void *)&loopback,
        sizeof(loopback));
    if (sent != (ssize_t)DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        close(g_game_bridge_sender_fd);
        g_game_bridge_sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_game_bridge_sender_fd < 0)
            return -1;
        sent = sendto(
            g_game_bridge_sender_fd, pad_data,
            DS4TOD5_GAME_BRIDGE_PAD_SIZE, 0,
            (const struct sockaddr *)(const void *)&loopback,
            sizeof(loopback));
    }
    if (sent != (ssize_t)DS4TOD5_GAME_BRIDGE_PAD_SIZE)
        return -1;
    return 0;
#endif
}

int
wireless_ds4_game_bridge_status(pid_t game_pid, intptr_t args_kaddr,
                               Ds4tod5GameBridgeStatus *out_status)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr; (void)out_status;
    return -1;
#else
    if (game_pid <= 0 || !args_kaddr || !out_status)
        return -1;
    GamePadBridgeArgs args;
    memset(&args, 0, sizeof(args));
    int stable_snapshot = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t seq_after = 0;
        if (remote_reader_copyout(
                game_pid, args_kaddr, &args, sizeof(args)) != 0 ||
            remote_reader_copyout(
                game_pid,
                args_kaddr + (intptr_t)offsetof(GamePadBridgeArgs, seq),
                &seq_after, sizeof(seq_after)) != 0)
            return -1;
        if ((args.seq & 1u) == 0 && args.seq == seq_after) {
            stable_snapshot = 1;
            break;
        }
    }
    if (!stable_snapshot ||
        args.magic != DS4TOD5_GAME_BRIDGE_MAGIC ||
        args.pad_size != DS4TOD5_GAME_BRIDGE_PAD_SIZE ||
        args.remote_block <= 0 || args.remote_block_size == 0)
        return -1;
    ScePadData pad;
    memcpy(&pad, args.pad_data, sizeof(pad));
    memset(out_status, 0, sizeof(*out_status));
    out_status->active = (uint32_t)args.active;
    out_status->seq = args.seq;
    out_status->pad_handle = args.pad_handle;
    out_status->receiver_ready = args.receiver_ready;
    out_status->receiver_last_result = args.receiver_last_result;
    out_status->receiver_port = args.receiver_port;
    out_status->receiver_packets = args.receiver_packets;
    out_status->read_state_calls = args.read_state_calls;
    out_status->read_state_ext_calls = args.read_state_ext_calls;
    out_status->read_calls = args.read_calls;
    out_status->read_ext_calls = args.read_ext_calls;
    out_status->data_internal_calls = args.data_internal_calls;
    out_status->controller_info_calls = args.controller_info_calls;
    out_status->controller_info_spoofs = args.controller_info_spoofs;
    out_status->buttons = pad.buttons;
    out_status->connected = pad.connected;
    return 0;
#endif
}

int
wireless_ds4_game_bridge_remove(pid_t game_pid, intptr_t args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr;
    return -1;
#else
    if (!args_kaddr)
        return -1;
    int32_t stop_receiver = 1;
    if (remote_reader_copyin(
            game_pid, &stop_receiver,
            args_kaddr + (intptr_t)offsetof(
                GamePadBridgeArgs, receiver_stop),
            sizeof(stop_receiver)) != 0)
        return -1;
    (void)game_bridge_send_stop();
    int receiver_stopped = 0;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int32_t ready = 0;
        if (remote_reader_copyout(
                game_pid,
                args_kaddr + (intptr_t)offsetof(
                    GamePadBridgeArgs, receiver_ready),
                &ready, sizeof(ready)) == 0 &&
            (ready == 2 || ready < 0)) {
            receiver_stopped = 1;
            break;
        }
        if ((attempt % 20u) == 19u)
            (void)game_bridge_send_stop();
        usleep(10000);
    }
    if (g_game_bridge_sender_fd >= 0) {
        close(g_game_bridge_sender_fd);
        g_game_bridge_sender_fd = -1;
    }
    if (!receiver_stopped) {
        klog_printf(
            "[DS4toDS5] refusing restore while receiver is still running\n");
        return -1;
    }
    if (sys_ptrace(PT_ATTACH, game_pid, 0, 0) != 0)
        return -1;
    if (waitpid(game_pid, NULL, 0) < 0) {
        (void)sys_ptrace(PT_DETACH, game_pid, (caddr_t)1, 0);
        return -1;
    }
    GamePadBridgeArgs args;
    int result = -1;
    if (mdbg_copyout(game_pid, args_kaddr, &args, sizeof(args)) != 0 ||
        args.magic != DS4TOD5_GAME_BRIDGE_MAGIC ||
        args.remote_block <= 0 || args.remote_block_size == 0)
        goto remove_done;
    intptr_t addresses[5] = {
        args.read_state_address,
        args.read_state_ext_address,
        args.read_address,
        args.read_ext_address,
        args.data_internal_address};
    const uint8_t *originals[5] = {
        args.original_read_state,
        args.original_read_state_ext,
        args.original_read,
        args.original_read_ext,
        args.original_data_internal};
    for (unsigned index = 0; index < 5; ++index) {
        uint8_t current[16];
        if (mdbg_copyout(game_pid, addresses[index], current,
                         sizeof(current)) != 0 ||
            current[0] != 0x49 || current[1] != 0xbb ||
            current[10] != 0x41 || current[11] != 0xff ||
            current[12] != 0xe3)
            goto remove_done;
        uint64_t gateway = 0;
        memcpy(&gateway, current + 2, sizeof(gateway));
        if (gateway < (uint64_t)args.remote_block ||
            gateway >= (uint64_t)args.remote_block +
                       args.remote_block_size)
            goto remove_done;
    }
    uint8_t current_info_function[20];
    if (args.controller_info_address <= 0 ||
        args.controller_info_trampoline < args.remote_block ||
        args.controller_info_trampoline >=
            args.remote_block + (intptr_t)args.remote_block_size ||
        args.controller_info_gateway < args.remote_block ||
        args.controller_info_gateway >=
            args.remote_block + (intptr_t)args.remote_block_size ||
        mdbg_copyout(
            game_pid, args.controller_info_address,
            current_info_function, sizeof(current_info_function)) != 0 ||
        current_info_function[0] != 0x49 ||
        current_info_function[1] != 0xbb ||
        current_info_function[10] != 0x41 ||
        current_info_function[11] != 0xff ||
        current_info_function[12] != 0xe3)
        goto remove_done;
    uint64_t current_info_gateway = 0;
    memcpy(&current_info_gateway, current_info_function + 2,
           sizeof(current_info_gateway));
    if (current_info_gateway != (uint64_t)args.controller_info_gateway)
        goto remove_done;
    int read_writable = 0;
    int data_writable = 0;
    int info_writable = 0;
    if (kernel_set_vmem_protection(
            game_pid, args.read_state_address, 64,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto remove_done;
    read_writable = 1;
    if (kernel_set_vmem_protection(
            game_pid, args.data_internal_address, 16,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto restore_bridge_protections;
    data_writable = 1;
    if (kernel_set_vmem_protection(
            game_pid, args.controller_info_address, 20,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto restore_bridge_protections;
    info_writable = 1;
    for (unsigned index = 0; index < 5; ++index) {
        if (pt_io_write(game_pid, addresses[index], originals[index], 16) != 0)
            goto restore_bridge_protections;
    }
    if (pt_io_write(
            game_pid, args.controller_info_address,
            args.original_controller_info, 20) != 0)
        goto restore_bridge_protections;
    result = 0;
restore_bridge_protections:
    if (info_writable)
        (void)kernel_set_vmem_protection(
            game_pid, args.controller_info_address, 20,
            (int)args.original_info_function_protection);
    if (data_writable)
        (void)kernel_set_vmem_protection(
            game_pid, args.data_internal_address, 16,
            (int)args.original_data_protection);
    if (read_writable)
        (void)kernel_set_vmem_protection(
            game_pid, args.read_state_address, 64,
            (int)args.original_protection);
    if (result != 0)
        goto remove_done;
    klog_printf(
        "[DS4toDS5] game bridge removed calls state=%llu state_ext=%llu "
        "read=%llu read_ext=%llu data_internal=%llu "
        "controller_info=%llu spoofed=%llu\n",
        (unsigned long long)args.read_state_calls,
        (unsigned long long)args.read_state_ext_calls,
        (unsigned long long)args.read_calls,
        (unsigned long long)args.read_ext_calls,
        (unsigned long long)args.data_internal_calls,
        (unsigned long long)args.controller_info_calls,
        (unsigned long long)args.controller_info_spoofs);
remove_done:
    (void)sys_ptrace(PT_DETACH, game_pid, (caddr_t)1, 0);
    return result;
#endif
}


static void
report_printf(int fd, const char *format, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    size_t write_length = (size_t)length;
    if (write_length >= sizeof(buffer))
        write_length = sizeof(buffer) - 1;
    (void)write(fd, buffer, write_length);
}

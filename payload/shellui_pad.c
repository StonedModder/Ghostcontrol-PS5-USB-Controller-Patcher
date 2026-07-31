/* SPDX-License-Identifier: GPL-3.0-or-later
 * shellui_pad.c — Inject a pad-forwarding thread into a system process
 *
 * Injects a stub thread into the first attachable system process that has
 * libScePad loaded and can create/bind a virtual controller through the
 * SceShellCore/SceShellUI path. Once running, 60 Hz pad updates arrive via
 * mdbg_copyin when a valid write handle exists.
 *
 * == Phase ordering (fixes PS5 freeze) ==
 *   OLD (broken): write code cave → PT_ATTACH (fails) → INT3 left in live code
 *   NEW (fixed):  PT_ATTACH first → only write if attach succeeds → PT_DETACH
 *
 * == Code cave (Phase 5) ==
 *   Use the library init/fini section directly as the conservative injection
 *   path. Target-side mmap experimentation proved less stable on retail.
 *
 * == Injection targets (Phase 4) ==
 *   SceShellUI cannot be ptraced (EINVAL, authid protected).  Candidates are
 *   tried in order; the first that accepts PT_ATTACH is used.
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
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <machine/reg.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#include <ps5/nid.h>
#endif

#ifdef __ORBIS__
#include <ps4/kernel.h>
#include <ps4/klog.h>
#include <ps4/mdbg.h>
#include <dlfcn.h>
#endif

#include "shellui_pad.h"
#include "gc_types.h"

/* Route shellui_pad diagnostics through the persistent status logger as well
 * as klog. main.c owns ghostpad_status_log(), which keeps klog_printf behavior
 * and appends the same line to /data/ghostpad/ghostpad_status.log. */
#define klog_printf ghostpad_status_log

#ifdef __ORBIS__
extern int kernel_set_vmem_protection(pid_t pid, intptr_t addr, size_t size, int prot);
#endif

#define GHOSTPAD_ASSIGNMENT_SCREEN_RET ((int32_t)0x803B0006u)
#define GHOSTPAD_AUTO_DISMISS_ACTIVE   ((int32_t)0x44534D31u) /* "DSM1" */
#define GHOSTPAD_AUTO_DISMISS_DONE     ((int32_t)0x44534D32u) /* "DSM2" */

static void report_printf(int fd, const char *format, ...);

#ifndef GHOSTPAD_ALLOW_UNSAFE_VDA_PATCH
#define GHOSTPAD_ALLOW_UNSAFE_VDA_PATCH 0
#endif

#ifndef GHOSTPAD_ALLOW_UNSAFE_SETPRIV_HOOK
#define GHOSTPAD_ALLOW_UNSAFE_SETPRIV_HOOK 0
#endif

#ifndef GHOSTPAD_ENABLE_KNOWN_VDA_PATCH
#define GHOSTPAD_ENABLE_KNOWN_VDA_PATCH 1
#endif

#define GHOSTPAD_VDA_PS4_LIBSCEPAD_VDA_OFF     0x5b40u
#define GHOSTPAD_VDA_PS4_HASH256               0xbb22d8acd843d81eull
#define GHOSTPAD_VDA_PS4_HASH4K                0x346f2b8071895f89ull
#define GHOSTPAD_VDA_PS4_CALL_OFF              0x0c0u
#define GHOSTPAD_VDA_PS4_AFTER_CALL_OFF        0x0c5u
#define GHOSTPAD_VDA_PS4_BRANCH_OFF            0x0cdu
#define GHOSTPAD_VDA_PS4_CAVE_OFF              0x0dd2u
#define GHOSTPAD_VDA_PS4_CAVE_LEN              14u

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

static int
ghostpad_all_byte(const uint8_t *buf, size_t len, uint8_t value)
{
    if (!buf || len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != value) return 0;
    }
    return 1;
}

/* PT_IO fallback for shellui_pad_update — always enabled */

/* sys_ptrace — elevate credentials for ptrace, then restore */
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
    if (kernel_set_ucred_caps(mypid, privcaps))      return -1;

    ret = (int)__syscall(SYS_ptrace, request, pid, addr, data);

    kernel_set_ucred_authid(mypid, authid);
    kernel_set_ucred_caps(mypid, caps);
    return ret;
}

/* find_pids — locate processes by thread name via sysctl (ki_pid@72, ki_tdname@447) */
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

    for (uint8_t *ptr = buf; ptr < (buf + buf_size);) {
        int   ki_structsize = *(int   *)ptr;
        pid_t ki_pid        = *(pid_t *)&ptr[72];
        char *ki_tdname     = (char   *)&ptr[447];
        size_t pi;
        int seen = 0;

        ptr += ki_structsize;
        if (strcmp(name, ki_tdname) || ki_pid == mypid) {
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

/* resolve_sym — look up a symbol in a remote process library */
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

/* get_lib — wrapper around kernel_dynlib_handle with logging */
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
    klog_printf("[Ghostpad] dynlib_handle(%s) -> ret=%d handle=0x%x\n",
                name, ret, *handle);
    return (*handle != 0) ? 0 : -1;
}

/* pt_io_write — write process memory via PT_IO (process must be stopped) */
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

size_t
shellui_pad_process_count(const char *process_name)
{
    pid_t pids[8];
    if (!process_name || !process_name[0])
        return 0;
    return find_pids(process_name, pids, sizeof(pids) / sizeof(pids[0]));
}

typedef struct {
    int      valid;
    int      attached;
    pid_t    pid;
    intptr_t args_kaddr;
    intptr_t trap_rip;
    intptr_t fn_setpriv;
    intptr_t fn_setloginuser;
    intptr_t fn_setusernumber;
    intptr_t fn_setfocus;
    intptr_t fn_usleep;
    intptr_t fn_gethandle;
    intptr_t fn_gethandle_ext;
    intptr_t fn_open;
    intptr_t fn_open_ext;
    intptr_t fn_open_ext2;
    intptr_t fn_insert;
    intptr_t fn_vdi;
    int32_t  pad_handle;
    int32_t  use_insert;
} ShellUiDirectState;

static ShellUiDirectState g_shellui_direct_state = {0};
static int32_t g_shellui_direct_last_stage = 0;
static int64_t g_shellui_direct_last_value = 0;

/* ── Original bytes saved for unpatching ── */
static uint8_t  g_orig_gethandle[5];
static int      g_gethandle_hooked        = 0;
static uint8_t  g_orig_setpriv[5];
static int      g_setpriv_hooked          = 0;
static uint8_t  g_orig_vdi_128[128];
static int      g_vdi_hooked              = 0;
static uint8_t  g_orig_vda_call[5];
static uint8_t  g_orig_vda_cave[8];
static int      g_vda_patched             = 0;
static pid_t    g_vda_patched_pid         = -1;
static uint8_t  g_orig_self_vda_call[5];
static uint8_t  g_orig_self_vda_cave[8];
static int      g_self_vda_patched        = 0;

/* Saved injection state for stub relaunch — populated by shellui_pad_inject */
static pid_t    g_relaunch_pid            = -1;
static intptr_t g_relaunch_args_kaddr     = 0;

static intptr_t g_relaunch_stub_fn        = 0;   /* stub function addr in target */
static intptr_t g_relaunch_thread_storage = 0;   /* pthread_t storage in target  */
static intptr_t g_relaunch_pthread_fn     = 0;   /* pthread_create addr in target*/
static intptr_t g_relaunch_trap_rip       = 0;   /* INT3 addr for pt_call        */
static intptr_t g_relaunch_malloc_fn      = 0;   /* malloc addr in target — for new stub alloc */

static void
shellui_pad_direct_set_last_status(int32_t stage, int64_t value)
{
    g_shellui_direct_last_stage = stage;
    g_shellui_direct_last_value = value;
}

void
shellui_pad_direct_get_last_status(int32_t *stage, int64_t *value)
{
    if (stage) {
        *stage = g_shellui_direct_last_stage;
    }
    if (value) {
        *value = g_shellui_direct_last_value;
    }
}

static int
shellui_pad_direct_context_usable(pid_t shellui_pid, intptr_t args_kaddr)
{
    return g_shellui_direct_state.valid &&
           g_shellui_direct_state.pid == shellui_pid &&
           g_shellui_direct_state.args_kaddr == args_kaddr;
}

/* Fast single-attempt PT_IO write — no retries, no sleep.
 * PT_ATTACH stops SceShellCore briefly to write pad_data+seq.
 * The stub background thread then resumes and calls VDI independently. */
static int
shellui_pad_ptrace_update(pid_t shellui_pid, intptr_t args_kaddr,
                          const void *pad_data, uint32_t pad_data_len,
                          uint32_t new_seq)
{
    intptr_t data_field = args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, pad_data);
    intptr_t seq_field  = args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, seq);

    if (sys_ptrace(PT_ATTACH, shellui_pid, 0, 0) != 0)
        return -1;   /* busy — skip this packet, next one will try again */

    waitpid(shellui_pid, NULL, 0);

    if (pt_io_write(shellui_pid, data_field, pad_data, pad_data_len) ||
        pt_io_write(shellui_pid, seq_field,  &new_seq,  sizeof(new_seq))) {
        sys_ptrace(PT_DETACH, shellui_pid, (caddr_t)1, 0);
        return -1;
    }

    sys_ptrace(PT_DETACH, shellui_pid, (caddr_t)1, 0);
    return 0;
}

/* pt_call — call fn(a1..a6) inside a stopped process via INT3; returns RAX */
static int64_t
pt_call(pid_t pid, intptr_t fn, intptr_t trap_rip,
        uint64_t a1, uint64_t a2, uint64_t a3,
        uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct reg regs, saved;
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
    if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0))    return -1;

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
            klog_printf("[Ghostpad] pt_call: waitpid error errno=%d\n", errno);
            break;
        }
        if (r == 0) {
            usleep(1000);   /* 1 ms — process hasn't stopped yet */
            total_ms++;
            continue;
        }
        /* Process stopped */
        if (!WIFSTOPPED(status)) {
            klog_printf("[Ghostpad] pt_call: process exited status=0x%x\n", status);
            sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -1;
        }
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) { got_trap = 1; break; }
        if (sig == SIGBUS || sig == SIGSEGV) {
            klog_printf("[Ghostpad] pt_call: suppressing fatal target sig=%d\n",
                        sig);
            (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -2;
        }
        /* Suppress SIGCHLD — forwarding it during injected execution causes panics */
        int fwd = (sig == 17) ? 0 : sig;
        if (fwd != sig)
            klog_printf("[Ghostpad] pt_call: suppressing SIGCHLD\n");
        else
            klog_printf("[Ghostpad] pt_call: forwarding sig=%d\n", sig);
        sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, fwd);
    }
    if (!got_trap) {
        klog_printf("[Ghostpad] pt_call: timed out waiting for SIGTRAP fn=0x%lx\n", fn);
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) return -1;
    int64_t retval = (int64_t)regs.r_rax;
    klog_printf("[Ghostpad] pt_call: fn=0x%lx rip=0x%lx rax=0x%lx\n",
                fn, (uint64_t)regs.r_rip, (uint64_t)retval);

    sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return retval;
}

static int64_t
pt_call_with_copy(pid_t pid, intptr_t fn, intptr_t trap_rip,
                  uint64_t a1, const void *buf, size_t len)
{
    struct reg regs, saved;
    int status;
    int got_trap = 0;
    uint64_t retval = (uint64_t)-1;
    size_t copy_len = (len + 15) & ~(size_t)15;
    intptr_t ret_rsp;
    intptr_t buf_addr;

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) return -1;
    memcpy(&saved, &regs, sizeof(regs));

    /* Emulate CALL's pushed return address: callee entry RSP % 16 == 8. */
    ret_rsp = ((regs.r_rsp - 256) & ~(intptr_t)0xf) - 8;
    buf_addr = ret_rsp - (intptr_t)copy_len;

    if (pt_io_write(pid, buf_addr, buf, len)) return -1;
    if (pt_io_write(pid, ret_rsp, &trap_rip, 8)) return -1;

    regs.r_rsp = ret_rsp;
    regs.r_rip = fn;
    regs.r_rdi = a1;
    regs.r_rsi = (uint64_t)buf_addr;
    regs.r_rdx = 0;
    regs.r_rcx = 0;
    regs.r_r8  = 0;
    regs.r_r9  = 0;

    if (sys_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0)) return -1;
    if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0))    return -1;

    for (int total_ms = 0; total_ms < 5000; ) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r < 0) {
            klog_printf("[Ghostpad] pt_call_with_copy: waitpid error errno=%d\n", errno);
            break;
        }
        if (r == 0) {
            usleep(1000);
            total_ms++;
            continue;
        }
        if (!WIFSTOPPED(status)) {
            klog_printf("[Ghostpad] pt_call_with_copy: process exited status=0x%x\n", status);
            sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -1;
        }
        {
            int sig = WSTOPSIG(status);
            if (sig == SIGTRAP) {
                got_trap = 1;
                break;
            }
            sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, (sig == 17) ? 0 : sig);
        }
    }
    if (!got_trap) {
        klog_printf("[Ghostpad] pt_call_with_copy: timed out waiting for SIGTRAP fn=0x%lx\n", fn);
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) {
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    retval = (uint64_t)regs.r_rax;
    sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return (int64_t)retval;
}

/* Call a two-argument target function where argument two is an output buffer.
 * The scratch buffer lives on the stopped target thread's stack.  Unlike the
 * generic pt_call helper, an unexpected signal is suppressed: registers are
 * restored and the caller can detach without delivering the signal to the
 * system process.
 */
static int64_t
pt_call_with_output(pid_t pid, intptr_t fn, intptr_t trap_rip,
                    uint64_t a1, void *buf, size_t len)
{
    struct reg regs, saved;
    int status;
    int got_trap = 0;
    size_t copy_len = (len + 15) & ~(size_t)15;
    intptr_t ret_rsp;
    intptr_t buf_addr;

    if (!buf || len == 0 || copy_len > 4096)
        return -1;
    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0))
        return -1;
    memcpy(&saved, &regs, sizeof(regs));

    /* Emulate CALL's pushed return address: callee entry RSP % 16 == 8. */
    ret_rsp = ((regs.r_rsp - 256) & ~(intptr_t)0xf) - 8;
    buf_addr = ret_rsp - (intptr_t)copy_len;

    memset(buf, 0, len);
    if (pt_io_write(pid, buf_addr, buf, len) ||
        pt_io_write(pid, ret_rsp, &trap_rip, 8))
        return -1;

    regs.r_rsp = ret_rsp;
    regs.r_rip = fn;
    regs.r_rdi = a1;
    regs.r_rsi = (uint64_t)buf_addr;
    regs.r_rdx = 0;
    regs.r_rcx = 0;
    regs.r_r8  = 0;
    regs.r_r9  = 0;

    if (sys_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0) ||
        sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0))
        return -1;

    for (int total_ms = 0; total_ms < 5000; ) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r < 0)
            break;
        if (r == 0) {
            usleep(1000);
            total_ms++;
            continue;
        }
        if (!WIFSTOPPED(status)) {
            klog_printf("[Ghostpad] pt_call_with_output: target exited status=0x%x\n",
                        status);
            return -1;
        }
        if (WSTOPSIG(status) == SIGTRAP) {
            got_trap = 1;
            break;
        }

        klog_printf("[Ghostpad] pt_call_with_output: suppressing unexpected sig=%d\n",
                    WSTOPSIG(status));
        (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -2;
    }

    if (!got_trap) {
        (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) {
        (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }

    int64_t retval = (int64_t)regs.r_rax;
    if (mdbg_copyout(pid, buf_addr, buf, len) != 0)
        retval = -1;
    (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return retval;
}

/* ============================================================
 * THE STUB — runs as a thread inside the target process.
 *
 * DESIGN: VDA is called from THIS running thread, not from pt_call.
 * Testing confirmed that VDA always returns 0x803b0001 when called via
 * pt_call (stopped-process main-thread context), but succeeds when called
 * from a running injected thread.  The injector (pt_call) only calls
 * scePadSetProcessPrivilege(1) — a process-level flag safe to set from
 * the stopped main thread.  Everything else runs here.
 *
 * Thread sequence:
 *   1. Sleep 500ms  (TLS, signal masks, stack guard fully initialized)
 *   2. deleteDevice(0..15)  — clean up any orphan from a previous run
 *   3. fp_vda(&param, 3)  — create virtual DualSense; exit on failure
 *   4. Auto-press Cross  — dismiss "who is using this controller?" dialog
 *   5. Insert loop  — forward pad data from main process at 60 Hz
 *   6. fp_del(handle)  — delete virtual device on exit so next run is clean
 *
 * Rules: position-independent, no direct library calls, no globals/statics.
 * All calls go through fp_* pointers in ShellUiPadArgs.
 * ============================================================ */
extern void shellui_stub(void *arg);
extern void shellui_stub_end(void);

__attribute__((noinline, section(".text.stub")))
void shellui_stub(void *arg)
{
    ShellUiPadArgs *a = (ShellUiPadArgs *)arg;
    int32_t assignment_hint = 0;
    int32_t handle_from_vda_token = 0;
    int32_t initial_pad_handle = a->pad_handle;

    /* Early marker so the injector can distinguish "thread never ran" from
     * "thread started and died before reaching ready=1". */
    a->rc_log[15] = (int32_t)0x53545542u; /* "STUB" */

    /* pad_handle semantics set by injector (shellui_pad_inject Step 6.5):
     *   >= 0  injector obtained a handle via pt_call (stopped process)
     *   -1    SceShellCore (server-side) — thread libScePad calls are safe
     *   -2    client process (SceShellUI etc.) — ANY libScePad IPC deadlocks  */
    int32_t vda_handle = a->pad_handle;
    int32_t use_insert = 0;

    if (vda_handle >= 0) {
        /* ── FAST PATH ────────────────────────────────────────────────────────
         * The injector already resolved a handle via pt_call while SceShellUI's
         * main thread was stopped.  Skip ALL libScePad IPC here: SceShellUI's
         * main thread owns the IPC socket and concurrent access from this thread
         * deadlocks → kernel panic.
         *
         * scePadVirtualDeviceInsertData writes to a shared-memory ring buffer
         * and is not an IPC round-trip, so it is safe
         * to call from a secondary thread while the main thread uses the socket.
         * ───────────────────────────────────────────────────────────────────── */
        a->rc_log[0] = vda_handle;
        /* Injector encoded use_insert hint in seq (0=VDI first, 1=fp_insert first).
         * Reset seq to 0 so the insert loop starts clean. */
        use_insert = (int32_t)a->seq;
        a->seq = 0;

        /* In legacy client targets, establish an explicit pad context before the
         * assignment-screen follow-up. The main payload-side context setters
         * are not authoritative for the client process, so do the same setup
         * inside the recovered process before probing for the post-ASGN path. */
        if (a->fp_setloginuser) {
            a->rc_log[4] = a->fp_setloginuser(1);
        }
        if (a->fp_setusernumber) {
            a->rc_log[5] = a->fp_setusernumber(1);
        }
        if (a->fp_setfocus) {
            a->rc_log[6] = a->fp_setfocus(1, 0, 0, 0, 0, 0);
        }

        /*
         * Assignment-screen attempt from a running client-process thread.
         * If this succeeds, switch over to the virtual handle and drive it via
         * VDI so the PS5 can show the assignment UI and then accept our Cross
         * dismiss pulse.
         */
        if (a->fp_vda) {
            int32_t uid_try[3];
            int32_t ui;
            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            for (ui = 0; ui < 3; ui++) {
                struct { int32_t f[8]; } vdp;
                int32_t vi;
                int32_t vda_ret;
                for (vi = 0; vi < 8; vi++) vdp.f[vi] = 0;
                vdp.f[0] = 32;
                vdp.f[1] = uid_try[ui];
                vda_ret = a->fp_vda(&vdp, 3);
                a->rc_log[ui] = vda_ret;
                if (ui == 0) {
                    a->rc_log[8]  = vdp.f[0];
                    a->rc_log[9]  = vdp.f[1];
                    a->rc_log[10] = vdp.f[2];
                    a->rc_log[11] = vdp.f[3];
                    a->rc_log[12] = vdp.f[4];
                    a->rc_log[13] = vdp.f[5];
                    a->rc_log[14] = vdp.f[6];
                    a->rc_log[15] = vdp.f[7];
                }
                if (vda_ret >= 0) {
                    vda_handle = vda_ret;
                    use_insert = 0;
                    a->pad_handle = vda_handle;
                    a->rc_log[6] = (int32_t)0x60000001;
                    a->rc_log[7] = vda_handle;
                    break;
                }
                for (vi = 2; vi < 8; vi++) {
                    if (vdp.f[vi] != 0 &&
                        vdp.f[vi] != -1) {
                        vda_handle = vdp.f[vi];
                        use_insert = 0;
                        assignment_hint = 0;
                        a->pad_handle = vda_handle;
                        a->rc_log[6] = (int32_t)0x60000002;
                        a->rc_log[7] = vda_handle;
                        break;
                    }
                }
                if (!use_insert) {
                    break;
                }
                if ((uint32_t)vda_ret == (uint32_t)GHOSTPAD_ASSIGNMENT_SCREEN_RET) {
                    assignment_hint = 1;
                    a->rc_log[7] = 0x4153474Eu; /* "ASGN" marker: assignment screen branch observed */
                    a->fp_usleep(300000);
                }
            }
        }

        if (assignment_hint && (a->fp_gethandle || a->fp_gethandle_ext)) {
            int32_t uid_try[3];
            int32_t attempt;
            int32_t ui;

            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            for (attempt = 0; attempt < 30 && use_insert; attempt++) {
                for (ui = 0; ui < 3 && use_insert; ui++) {
                    int32_t idx;
                    for (idx = 0; idx < 8 && use_insert; idx++) {
                        int32_t gh = a->fp_gethandle_ext
                            ? a->fp_gethandle_ext(uid_try[ui], 3, idx, 0, 0, 0)
                            : a->fp_gethandle(uid_try[ui], 3, idx);
                        a->rc_log[3] = gh;
                        a->rc_log[6] = (attempt << 8) | (idx & 0xff);
                        if (gh >= 0) {
                            vda_handle = gh;
                            use_insert = 0;
                            a->rc_log[7] = 0x56444930; /* "VDI0" marker: recovered virtual handle after ASGN */
                        }
                    }
                }
                if (use_insert) {
                    a->fp_usleep(150000);
                }
            }
        }

    } else {
        /* ── SLOW PATH (SceShellCore -1, or client fallback -2) ─────────────
         * Only reach here from server-side processes or as a last resort.
         * 500ms sleep: let thread fully initialize (TLS, signal masks, stack). */
        a->fp_usleep(500000);

        /* setPriv: safe only for SceShellCore (-1); client (-2) would deadlock */
        if (vda_handle == -1 && a->fp_setpriv) {
            a->rc_log[2] = a->fp_setpriv(1);
        }

        if (vda_handle == -1) {
            if (a->fp_setloginuser) {
                a->rc_log[4] = a->fp_setloginuser(1);
            }
            if (a->fp_setusernumber) {
                a->rc_log[5] = a->fp_setusernumber(1);
            }
            if (a->fp_setfocus) {
                a->rc_log[6] = a->fp_setfocus(1, 0, 0, 0, 0, 0);
            }
        }

        if (vda_handle == -2) {
            a->rc_log[7] = (int32_t)-2;   /* marker: -2 sentinel received */
            vda_handle = -1;
        } else if (a->fp_vda) {
            /* SceShellCore: server-side, thread VDA causes no IPC loop */
            int32_t uid_try[3];
            int32_t ui;
            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
                struct { int32_t f[8]; } vdp;
                int32_t vi;
                for (vi = 0; vi < 8; vi++) vdp.f[vi] = 0;
                vdp.f[0] = 32;
                vdp.f[1] = uid_try[ui];
                int32_t vda_ret = a->fp_vda(&vdp, 3);
                a->rc_log[ui] = vda_ret;
                if (ui == 0) {
                    a->rc_log[8]  = vdp.f[0];
                    a->rc_log[9]  = vdp.f[1];
                    a->rc_log[10] = vdp.f[2];
                    a->rc_log[11] = vdp.f[3];
                    a->rc_log[12] = vdp.f[4];
                    a->rc_log[13] = vdp.f[5];
                    a->rc_log[14] = vdp.f[6];
                    a->rc_log[15] = vdp.f[7];
                }
                if (vda_ret >= 0) {
                    vda_handle = vda_ret;
                    a->pad_handle = vda_handle;
                    a->rc_log[6] = (int32_t)0x60000001;
                } else {
                    for (vi = 2; vi < 8 && vda_handle < 0; vi++) {
                        if (vdp.f[vi] != 0 &&
                            vdp.f[vi] != -1) {
                            vda_handle = vdp.f[vi];
                            handle_from_vda_token = 1;
                            a->pad_handle = vda_handle;
                            a->rc_log[6] = (int32_t)0x60000002;
                            a->rc_log[7] = vda_handle;
                        }
                    }
                }
                if ((uint32_t)vda_ret == (uint32_t)GHOSTPAD_ASSIGNMENT_SCREEN_RET) {
                    assignment_hint = 1;
                    a->rc_log[7] = 0x4153474Eu;
                    a->fp_usleep(300000);
                }
            }
        }

        if (handle_from_vda_token && (a->fp_gethandle || a->fp_gethandle_ext)) {
            int32_t uid_try[3];
            int32_t ui;

            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            for (ui = 0; ui < 3 && handle_from_vda_token; ui++) {
                int32_t idx;
                for (idx = 0; idx < 8 && handle_from_vda_token; idx++) {
                    int32_t gh = a->fp_gethandle_ext
                        ? a->fp_gethandle_ext(uid_try[ui], 3, idx, 0, 0, 0)
                        : a->fp_gethandle(uid_try[ui], 3, idx);
                    a->rc_log[3] = gh;
                    if (gh >= 0) {
                        vda_handle = gh;
                        a->pad_handle = gh;
                        a->rc_log[5] = (int32_t)0x70000001;
                        a->rc_log[7] = gh;
                        handle_from_vda_token = 0;
                    }
                }
            }
        }

        if (handle_from_vda_token && (a->fp_open || a->fp_open_ext || a->fp_open_ext2)) {
            int32_t uid_try[3];
            int32_t ui;

            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            for (ui = 0; ui < 3 && handle_from_vda_token; ui++) {
                int32_t oh = a->fp_open_ext2
                    ? a->fp_open_ext2(uid_try[ui], 3, 0, (void *)0, 0, 0)
                    : (a->fp_open_ext
                        ? a->fp_open_ext(uid_try[ui], 3, 0, (void *)0, 0, 0)
                        : (a->fp_open ? a->fp_open(uid_try[ui], 3, 0, (void *)0) : -1));
                a->rc_log[4] = oh;
                if (oh >= 0) {
                    vda_handle = oh;
                    a->pad_handle = oh;
                    a->rc_log[5] = (int32_t)0x70000002;
                    a->rc_log[7] = oh;
                    handle_from_vda_token = 0;
                }
            }
        }

        if (handle_from_vda_token) {
            vda_handle = -1;
        }

        /* Fallback: GetHandle(type=3) existing virtual DualSense */
        if (vda_handle < 0 && (a->fp_gethandle || a->fp_gethandle_ext)) {
            int32_t uid_try[3];
            int32_t ui;
            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            if (assignment_hint) {
                int32_t attempt;
                for (attempt = 0; attempt < 60 && vda_handle < 0; attempt++) {
                    for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
                        int32_t idx;
                        for (idx = 0; idx < 8 && vda_handle < 0; idx++) {
                            int32_t gh = a->fp_gethandle_ext
                                ? a->fp_gethandle_ext(uid_try[ui], 3, idx, 0, 0, 0)
                                : a->fp_gethandle(uid_try[ui], 3, idx);
                            a->rc_log[3] = gh;
                            a->rc_log[6] = (attempt << 8) | (idx & 0xff);
                            if (gh >= 0) vda_handle = gh;
                        }
                    }
                    if (vda_handle < 0) {
                        a->fp_usleep(150000);
                    }
                }
            } else {
                for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
                    int32_t idx;
                    for (idx = 0; idx < 8 && vda_handle < 0; idx++) {
                        int32_t gh = a->fp_gethandle_ext
                            ? a->fp_gethandle_ext(uid_try[ui], 3, idx, 0, 0, 0)
                            : a->fp_gethandle(uid_try[ui], 3, idx);
                        a->rc_log[3] = gh;
                        if (gh >= 0) vda_handle = gh;
                    }
                }
            }
        }

        if (vda_handle < 0 && (a->fp_gethandle || a->fp_gethandle_ext) && initial_pad_handle == -1 && assignment_hint) {
            int32_t uid_try[3];
            int32_t ui;
            int32_t attempt;
            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            for (attempt = 0; attempt < 40 && vda_handle < 0; attempt++) {
                for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
                    int32_t idx;
                    for (idx = 0; idx < 8 && vda_handle < 0; idx++) {
                        int32_t gh = a->fp_gethandle_ext
                            ? a->fp_gethandle_ext(uid_try[ui], 0, idx, 0, 0, 0)
                            : a->fp_gethandle(uid_try[ui], 0, idx);
                        a->rc_log[5] = gh;
                        a->rc_log[6] = 0x4000 | ((attempt & 0xff) << 4) | (idx & 0xf);
                        if (gh >= 0) { vda_handle = gh; use_insert = 1; }
                    }
                }
                if (vda_handle < 0) {
                    a->fp_usleep(150000);
                }
            }
        }

        /* Fallback: GetHandle(type=0) physical DualSense */
        if (vda_handle < 0 && (a->fp_gethandle || a->fp_gethandle_ext) && initial_pad_handle != -1) {
            int32_t uid_try[3];
            int32_t ui;
            uid_try[0] = 1;
            uid_try[1] = a->userId;
            uid_try[2] = 0x10000000;
            if (assignment_hint) {
                int32_t attempt;
                for (attempt = 0; attempt < 20 && vda_handle < 0; attempt++) {
                    for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
                        int32_t idx;
                        for (idx = 0; idx < 4 && vda_handle < 0; idx++) {
                            int32_t gh = a->fp_gethandle_ext
                                ? a->fp_gethandle_ext(uid_try[ui], 0, idx, 0, 0, 0)
                                : a->fp_gethandle(uid_try[ui], 0, idx);
                            a->rc_log[5] = gh;
                            a->rc_log[6] = 0x3000 | ((attempt & 0xff) << 4) | (idx & 0xf);
                            if (gh >= 0) { vda_handle = gh; use_insert = 1; }
                        }
                    }
                    if (vda_handle < 0) {
                        a->fp_usleep(150000);
                    }
                }
            } else {
                for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
                    int32_t idx;
                    for (idx = 0; idx < 4 && vda_handle < 0; idx++) {
                        int32_t gh = a->fp_gethandle_ext
                            ? a->fp_gethandle_ext(uid_try[ui], 0, idx, 0, 0, 0)
                            : a->fp_gethandle(uid_try[ui], 0, idx);
                        a->rc_log[5] = gh;
                        if (gh >= 0) { vda_handle = gh; use_insert = 1; }
                    }
                }
            }
        }

        if (vda_handle < 0) {
            a->ready = -1;
            return;
        }
    }

    a->pad_handle = vda_handle;
    a->ready = 1;
    a->rc_log[7] = use_insert ? 0x494e5331 : (a->rc_log[7] ? a->rc_log[7] : 0x56444930);

    /* Auto-press Cross to dismiss the "who is using this controller?" dialog.
     * Only needed for VDA virtual devices (type=3).
     * ScePadData byte layout:
     *   bytes [0..3] buttons LE — Cross = 0x00004000 → byte[1] = 0x40
     *   byte  [4]    leftStick.x  center=128
     *   byte  [5]    leftStick.y  center=128
     *   byte  [6]    rightStick.x center=128
     *   byte  [7]    rightStick.y center=128
     *   bytes [24..27] quat.w = 1.0f = 0x3F800000 LE
     *   byte  [76]   connected = 1
     */
    if (!use_insert) {
        uint8_t ap[SHELLUI_PAD_DATA_SIZE];
        int32_t ai;
        for (ai = 0; ai < SHELLUI_PAD_DATA_SIZE; ai++) ap[ai] = 0;
        ap[1]  = 0x40;
        ap[4]  = 128;
        ap[5]  = 128;
        ap[6]  = 128;
        ap[7]  = 128;
        ap[24] = 0x00;
        ap[25] = 0x00;
        ap[26] = 0x80;
        ap[27] = 0x3F;
        ap[76] = 1;

        a->fp_usleep(1000000);  /* 1000 ms: wait for PS5 UI dialog to render */

        int32_t apr = a->fp_vdi(vda_handle, ap);
        a->rc_log[5] = apr;     /* log first VDI result */

        a->fp_usleep(200000);   /* 200 ms hold */

        ap[1] = 0;              /* release Cross */
        a->fp_vdi(vda_handle, ap);

        a->fp_usleep(100000);
    }

    /* Insert loop: poll seq, inject pad data when main process increments seq.
     *
     * On the first injection attempt we log VDI's return code in rc_log[6].
     * Legacy insert fallback is disabled in current builds. */
    uint32_t last_seq = 0;
    int32_t probed = 0;   /* 0=not yet, 1=done */
    while (!a->stop) {
        uint32_t cur = a->seq;
        if (cur != last_seq) {
            if (!probed) {
                /* First data: try the preferred function and log the result. */
                int32_t r_ins = -1, r_vdi = -1;
                if (use_insert && a->fp_insert) {
                    r_ins = a->fp_insert(vda_handle, (const void *)a->pad_data);
                    a->rc_log[5] = r_ins;
                    if (r_ins < 0 && a->fp_vdi) {
                        /* Legacy insert rejected handle; try VDI. */
                        r_vdi = a->fp_vdi(vda_handle, (const void *)a->pad_data);
                        a->rc_log[6] = r_vdi;
                        if (r_vdi >= 0) use_insert = 0;   /* switch to VDI */
                    }
                } else if (a->fp_vdi) {
                    r_vdi = a->fp_vdi(vda_handle, (const void *)a->pad_data);
                    a->rc_log[6] = r_vdi;
                    if (r_vdi < 0 && a->fp_insert) {
                        /* Legacy insert fallback is disabled. */
                        r_ins = a->fp_insert(vda_handle, (const void *)a->pad_data);
                        a->rc_log[5] = r_ins;
                        if (r_ins >= 0) use_insert = 1;
                    }
                }
                probed = 1;
            } else {
                /* Subsequent data: use whichever function worked (or keep trying). */
                if (use_insert && a->fp_insert)
                    a->fp_insert(vda_handle, (const void *)a->pad_data);
                else if (a->fp_vdi)
                    a->fp_vdi(vda_handle, (const void *)a->pad_data);
            }
            last_seq = cur;
        }
        a->fp_usleep(500);
    }

    /* Delete virtual device on clean exit so next run doesn't get 0x803b0001.
     * Legacy insert handles (use_insert) are not used in current builds. */
    if (!use_insert && a->fp_del) a->fp_del(vda_handle);
}

__attribute__((noinline, section(".text.stub")))
void shellui_stub_end(void) { }

extern void shellui_stub_force_vda(void *arg);
extern void shellui_stub_force_vda_end(void);

__attribute__((noinline, section(".text.stubvda")))
void shellui_stub_force_vda(void *arg)
{
    ShellUiPadArgs *a = (ShellUiPadArgs *)arg;
    int32_t assignment_hint = 0;
    int32_t fallback_handle = a->rc_log[0];
    int32_t fallback_use_insert = a->rc_log[1];
    int32_t pad_type = (a->virtual_device_type >= 0) ? a->virtual_device_type : 3;
    int32_t vda_handle = -1;
    int32_t handle_from_vda_token = 0;
    int32_t use_insert = 0;
    int32_t button_probe_done = 0;
    int32_t uid_try[3];
    int32_t ui;

    if (fallback_handle < 0) fallback_handle = -1;
    if (fallback_use_insert < 0) fallback_use_insert = 0;

    a->rc_log[15] = (int32_t)0x53545542u; /* "STUB" */

    if (!a->fp_usleep) {
        a->ready = -1;
        return;
    }

    a->fp_usleep(500000);

    if (a->fp_setpriv) {
        a->rc_log[4] = a->fp_setpriv(1);
    }
    if (a->fp_setloginuser) {
        a->rc_log[5] = a->fp_setloginuser(1);
    }
    if (a->fp_setusernumber) {
        a->rc_log[6] = a->fp_setusernumber(1);
    }
    if (a->fp_setfocus) {
        a->rc_log[7] = a->fp_setfocus(1, 0, 0, 0, 0, 0);
    }

    /* Try real logged-in userId FIRST: if the pad daemon pre-assigns the device
     * to a known user, GetHandle(userId, type=3) works immediately and we skip
     * the assignment screen.  uid=1 is the fallback "anonymous" slot. */
    uid_try[0] = a->userId;
    uid_try[1] = 0x10000000;
    uid_try[2] = 1;

    if (a->fp_vda) {
        for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
            struct { int32_t f[8]; } vdp;
            int32_t vi;
            int32_t vda_ret;

            for (vi = 0; vi < 8; vi++) {
                vdp.f[vi] = 0;
            }
            vdp.f[0] = 32;
            vdp.f[1] = uid_try[ui];
            vda_ret = a->fp_vda(&vdp, pad_type);
            a->rc_log[ui] = vda_ret;

            if (ui == 0) {
                a->rc_log[8]  = vdp.f[0];
                a->rc_log[9]  = vdp.f[1];
                a->rc_log[10] = vdp.f[2];
                a->rc_log[11] = vdp.f[3];
                a->rc_log[12] = vdp.f[4];
                a->rc_log[13] = vdp.f[5];
                a->rc_log[14] = vdp.f[6];
                a->rc_log[15] = vdp.f[7];
            }

            if (vda_ret == 0) {
                a->rc_log[8]  = vdp.f[0];
                a->rc_log[9]  = vdp.f[1];
                a->rc_log[10] = vdp.f[2];
                a->rc_log[11] = vdp.f[3];
                a->rc_log[12] = vdp.f[4];
                a->rc_log[13] = vdp.f[5];
                a->rc_log[14] = vdp.f[6];
                a->rc_log[15] = vdp.f[7];
                for (vi = 2; vi < 8 && vda_handle < 0; vi++) {
                    if (vdp.f[vi] != 0 && vdp.f[vi] != -1) {
                        vda_handle = vdp.f[vi];
                        handle_from_vda_token = 1;
                        a->pad_handle = vda_handle;
                        a->rc_log[6] = (int32_t)0x60000002;
                        a->rc_log[7] = vda_handle;
                    }
                }
                if (vda_handle >= 0) {
                    break;
                }
                /* The surgical libScePad patch forces the IPC dispatch path to
                 * return 0 after creating the device.  That is success for
                 * creation, but it is not a usable VDI write handle. */
                assignment_hint = 1;
                a->rc_log[6] = (int32_t)0x60000000;
                a->rc_log[7] = (int32_t)0x56444130u; /* "VDA0" */
                break;
            }

            if (vda_ret > 0) {
                vda_handle = vda_ret;
                a->pad_handle = vda_handle;
                a->rc_log[6] = (int32_t)0x60000001;
                a->rc_log[7] = vda_handle;
                break;
            }

            for (vi = 2; vi < 8 && vda_handle < 0; vi++) {
                if (vdp.f[vi] != 0 && vdp.f[vi] != -1) {
                    vda_handle = vdp.f[vi];
                    handle_from_vda_token = 1;
                    a->pad_handle = vda_handle;
                    a->rc_log[6] = (int32_t)0x60000002;
                    a->rc_log[7] = vda_handle;
                }
            }

            if ((uint32_t)vda_ret == (uint32_t)GHOSTPAD_ASSIGNMENT_SCREEN_RET) {
                assignment_hint = 1;
                a->rc_log[7] = 0x4153474Eu; /* "ASGN" */
                a->fp_usleep(300000);
            }
        }
    }

    if (handle_from_vda_token && (a->fp_gethandle_ext || a->fp_gethandle)) {
        for (ui = 0; ui < 3 && handle_from_vda_token; ui++) {
            int32_t idx;
            for (idx = 0; idx < 8 && handle_from_vda_token; idx++) {
                int32_t gh = a->fp_gethandle_ext
                    ? a->fp_gethandle_ext(uid_try[ui], pad_type, idx, 0, 0, 0)
                    : a->fp_gethandle(uid_try[ui], pad_type, idx);
                a->rc_log[3] = gh;
                if (gh >= 0) {
                    vda_handle = gh;
                    a->pad_handle = gh;
                    a->rc_log[5] = (int32_t)0x70000001;
                    a->rc_log[7] = gh;
                    handle_from_vda_token = 0;
                }
            }
        }
    }

    if (handle_from_vda_token && (a->fp_open_ext2 || a->fp_open_ext || a->fp_open)) {
        for (ui = 0; ui < 3 && handle_from_vda_token; ui++) {
            int32_t oh = a->fp_open_ext2
                ? a->fp_open_ext2(uid_try[ui], pad_type, 0, (void *)0, 0, 0)
                : (a->fp_open_ext
                    ? a->fp_open_ext(uid_try[ui], pad_type, 0, (void *)0, 0, 0)
                    : (a->fp_open ? a->fp_open(uid_try[ui], pad_type, 0, (void *)0) : -1));
            a->rc_log[4] = oh;
            if (oh >= 0) {
                vda_handle = oh;
                a->pad_handle = oh;
                a->rc_log[5] = (int32_t)0x70000002;
                a->rc_log[7] = oh;
                handle_from_vda_token = 0;
            }
        }
    }

    if (vda_handle < 0 && (a->fp_gethandle_ext || a->fp_gethandle)) {
        for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
            int32_t idx;
            for (idx = 0; idx < 8 && vda_handle < 0; idx++) {
                int32_t gh = a->fp_gethandle_ext
                    ? a->fp_gethandle_ext(uid_try[ui], pad_type, idx, 0, 0, 0)
                    : a->fp_gethandle(uid_try[ui], pad_type, idx);
                a->rc_log[3] = gh;
                if (gh >= 0) {
                    vda_handle = gh;
                    a->pad_handle = gh;
                    a->rc_log[7] = gh;
                }
            }
        }
    }

    /* NOTE: removed the insert-based fallback (use_insert=1 + fp_insert with
     * fallback_handle).  The old insert function treated arg1 as an
     * object pointer, not an integer handle ID.  Using a type=0 handle from
     * scePadOpen here causes a page fault at the handle value — confirmed by
     * two SIGSEGV crashes.  If VDA produced no handle, leave vda_handle=-1
     * so the check below returns ready=-1 safely. */

    if (vda_handle < 0 && assignment_hint && (a->fp_gethandle_ext || a->fp_gethandle)) {
        /* FIRST: probe with userId=0xffffffff and userId=0 — the VDA-created
         * device has userId=0xffffffff before assignment.  GetHandle with the
         * assigned userIds (1, a->userId, 0x10000000) always fails until the
         * user dismisses the assignment screen, but the unassigned userId may
         * be directly queryable. */
        int32_t special_uid[2] = {(int32_t)0xffffffff, 0};
        int32_t sui, sidx;
        for (sui = 0; sui < 2 && vda_handle < 0; sui++) {
            for (sidx = 0; sidx < 8 && vda_handle < 0; sidx++) {
                int32_t gh = a->fp_gethandle_ext
                    ? a->fp_gethandle_ext(special_uid[sui], pad_type, sidx, 0, 0, 0)
                    : a->fp_gethandle(special_uid[sui], pad_type, sidx);
                a->rc_log[3] = gh;
                a->rc_log[6] = (int32_t)(0x5000 | (sui << 4) | (sidx & 0xf));
                if (gh >= 0) {
                    vda_handle = gh;
                    a->pad_handle = gh;
                    a->rc_log[5] = (int32_t)0x70000007;
                    a->rc_log[7] = (int32_t)0x56444931u; /* "VDI1" unassigned path */
                }
            }
        }
        /* THEN: also try Open(userId=0xffffffff, type=3) — in case GetHandle
         * rejects 0xffffffff but Open resolves the unassigned slot. */
        if (vda_handle < 0 && (a->fp_open_ext2 || a->fp_open_ext || a->fp_open)) {
            for (sui = 0; sui < 2 && vda_handle < 0; sui++) {
                int32_t oh = a->fp_open_ext2
                    ? a->fp_open_ext2(special_uid[sui], pad_type, 0, (void *)0, 0, 0)
                    : (a->fp_open_ext
                        ? a->fp_open_ext(special_uid[sui], pad_type, 0, (void *)0, 0, 0)
                        : a->fp_open(special_uid[sui], pad_type, 0, (void *)0));
                a->rc_log[4] = oh;
                a->rc_log[6] = (int32_t)(0x5100 | (sui & 0xf));
                if (oh >= 0) {
                    vda_handle = oh;
                    a->pad_handle = oh;
                    a->rc_log[5] = (int32_t)0x70000008;
                    a->rc_log[7] = (int32_t)0x56444932u; /* "VDI2" open-unassigned path */
                }
            }
        }
        /* FINALLY: poll with canonical userIds waiting for post-assignment.
         * 400 iterations × 150ms = 60 seconds — allows manual user dismiss. */
        int32_t attempt;
        for (attempt = 0; attempt < 400 && vda_handle < 0; attempt++) {
            for (ui = 0; ui < 3 && vda_handle < 0; ui++) {
                int32_t idx;
                for (idx = 0; idx < 8 && vda_handle < 0; idx++) {
                    int32_t gh = a->fp_gethandle_ext
                        ? a->fp_gethandle_ext(uid_try[ui], pad_type, idx, 0, 0, 0)
                        : a->fp_gethandle(uid_try[ui], pad_type, idx);
                    a->rc_log[3] = gh;
                    a->rc_log[6] = (int32_t)(0x4100 | ((attempt & 0xff) << 4) | (idx & 0xf));
                    if (gh >= 0) {
                        vda_handle = gh;
                        a->pad_handle = gh;
                        a->rc_log[5] = (int32_t)0x70000006;
                        a->rc_log[7] = (int32_t)0x56444930u; /* "VDI0" */
                    }
                }
            }
            if (vda_handle < 0) {
                a->fp_usleep(150000);
            }
        }
    }

    /* Do not issue a second VDA call after external Mbus assignment. Live klog
     * showed this creates another unassigned virtual pad instead of a write
     * handle, which pollutes the assignment state and obscures diagnostics. */

    if (vda_handle < 0) {
        a->ready = -1;
        return;
    }

    {
        uint8_t ap[SHELLUI_PAD_DATA_SIZE];
        int32_t ai;
        int32_t frame;
        int32_t press_ret = 0;
        int32_t release_ret = 0;

        for (ai = 0; ai < SHELLUI_PAD_DATA_SIZE; ai++) {
            ap[ai] = 0;
        }
        ap[4]  = 128;
        ap[5]  = 128;
        ap[6]  = 128;
        ap[7]  = 128;
        ap[24] = 0x00;
        ap[25] = 0x00;
        ap[26] = 0x80;
        ap[27] = 0x3F;
        ap[76] = 1;

        a->rc_log[12] = GHOSTPAD_AUTO_DISMISS_ACTIVE;
        a->rc_log[13] = use_insert ? (int32_t)0x494E5331u : (int32_t)0x56444930u;
        a->rc_log[14] = 0;
        a->rc_log[15] = 0;

        a->fp_usleep(1000000);

        for (frame = 0; frame < 12; frame++) {
            if (use_insert && a->fp_insert) {
                a->fp_insert(vda_handle, (const void *)ap);
            } else if (a->fp_vdi) {
                a->fp_vdi(vda_handle, (const void *)ap);
            }
            a->fp_usleep(16000);
        }

        ap[1] = 0x40;
        for (frame = 0; frame < 30; frame++) {
            if (use_insert && a->fp_insert) {
                press_ret = a->fp_insert(vda_handle, (const void *)ap);
            } else if (a->fp_vdi) {
                press_ret = a->fp_vdi(vda_handle, (const void *)ap);
            }
            a->fp_usleep(16000);
        }

        ap[1] = 0x00;
        for (frame = 0; frame < 18; frame++) {
            if (use_insert && a->fp_insert) {
                release_ret = a->fp_insert(vda_handle, (const void *)ap);
            } else if (a->fp_vdi) {
                release_ret = a->fp_vdi(vda_handle, (const void *)ap);
            }
            a->fp_usleep(16000);
        }

        a->rc_log[12] = GHOSTPAD_AUTO_DISMISS_DONE;
        a->rc_log[14] = press_ret;
        a->rc_log[15] = release_ret;
        button_probe_done = 1;
    }

    a->pad_handle = vda_handle;
    a->ready = 1;
    if (a->rc_log[7] == 0 || a->rc_log[7] == (int32_t)0x4153474Eu) {
        a->rc_log[7] = (int32_t)0x56444930u; /* "VDI0" */
    }

    {
        uint32_t last_seq = 0;
        while (!a->stop) {
            uint32_t cur = a->seq;
            if (cur != last_seq) {
                uint32_t buttons = *(const uint32_t *)(const void *)a->pad_data;

                {
                    int32_t active_ret = (int32_t)0xDEADBEEFu;

                    if (use_insert && a->fp_insert) {
                        active_ret = a->fp_insert(vda_handle, (const void *)a->pad_data);
                    } else if (a->fp_vdi) {
                        active_ret = a->fp_vdi(vda_handle, (const void *)a->pad_data);
                    } else if (a->fp_insert) {
                        use_insert = 1;
                        active_ret = a->fp_insert(vda_handle, (const void *)a->pad_data);
                    }

                    if (buttons != 0 && !button_probe_done) {
                        a->rc_log[12] = (int32_t)buttons;
                        a->rc_log[13] = use_insert
                                      ? (int32_t)0xB7001001u
                                      : (int32_t)0xB7001002u;
                        a->rc_log[14] = active_ret;
                        a->rc_log[15] = vda_handle;
                        button_probe_done = 1;
                    }
                }
                last_seq = cur;
            }
            a->fp_usleep(500);
        }
    }

    if (!use_insert && a->fp_del) {
        a->fp_del(vda_handle);
    }
}

__attribute__((noinline, section(".text.stubvda")))
void shellui_stub_force_vda_end(void) { }

/* shellui_pad_inject — inject a pad stub into an attachable target process.
 * PT_ATTACH happens first; nothing is written until attach succeeds. */
int
shellui_pad_inject(int32_t userId,
                   int      force_virtual_vda,
                   int32_t  virtual_device_type,
                   pid_t    *out_shellui_pid,
                   intptr_t *out_args_kaddr)
{
    /* Candidate processes: SceShellCore first (server-side VDA), SceShellUI as fallback */
    static const char *const candidates_vda[] = {
        "SceShellCore",
        "SceShellUI",
        NULL
    };
    static const char *const candidates_normal[] = {
        "SceShellCore",
        "SceShellUI",
        NULL
    };
    const char *const *candidates = force_virtual_vda ? candidates_vda : candidates_normal;

    for (int i = 0; candidates[i] != NULL; i++) {
        pid_t target_pids[16];
        size_t target_pid_count = 0;
        const char *target_name = candidates[i];

        target_pid_count = find_pids(target_name, target_pids, sizeof(target_pids) / sizeof(target_pids[0]));
        if (target_pid_count == 0) {
            klog_printf("[Ghostpad] %s: not found\n", target_name);
            continue;
        }

        for (size_t pid_index = 0; pid_index < target_pid_count; pid_index++) {
        pid_t       target_pid  = target_pids[pid_index];
        uint32_t libpad_h = 0, libkernel_h = 0, libpthread_h = 0, liblibc_h = 0;
        intptr_t fn_gethandle = 0, fn_gethandle_ext = 0, fn_open = 0, fn_open_ext = 0, fn_open_ext2 = 0, fn_insert = 0, fn_vdi = 0;
        intptr_t fn_vda = 0, fn_del = 0, fn_setpriv = 0, fn_setloginuser = 0;
        intptr_t fn_setusernumber = 0, fn_setfocus = 0, fn_usleep = 0, fn_pthread_create = 0, fn_mmap = 0;
        intptr_t fn_malloc = 0, fn_free = 0;
        size_t stub_code_size = 0, args_offset = 0, alloc_size = 0;
        size_t stub_alloc_size = 0, args_alloc_size = 0;
        intptr_t stub_mem = 0, trap_mem = 0, args_mem = 0, args_kaddr = 0, thread_storage = 0, stub_fn = 0;
        intptr_t init_mem = 0, fini_mem = 0;
        const void *stub_src = NULL;
        const void *stub_end = NULL;
        int is_shellcore = 0;
        int is_shellui = 0;
        int32_t pt_pad_handle = -1;
        int32_t pt_use_insert = 0;
        uint8_t int3 = 0xCC;

        klog_printf("[Ghostpad] PT_ATTACH(%s pid=%d)...\n", target_name, target_pid);
        if (sys_ptrace(PT_ATTACH, target_pid, 0, 0) != 0) {
            klog_printf("[Ghostpad] PT_ATTACH(%s): errno=%d\n", target_name, errno);
            continue;
        }
        waitpid(target_pid, NULL, 0);
        klog_printf("[Ghostpad] attached to %s (pid=%d)  authid=0x%016lx\n",
                    target_name, target_pid, kernel_get_ucred_authid(target_pid));

        if (get_lib(target_pid, "libScePad", &libpad_h) ||
            get_lib(target_pid, "libkernel_sys", &libkernel_h)) {
            klog_printf("[Ghostpad] library lookup failed in %s\n", target_name);
            sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
            continue;
        }
        get_lib(target_pid, "libpthread", &libpthread_h);
        get_lib(target_pid, "libSceLibcInternal", &liblibc_h);

        fn_gethandle      = resolve_sym(target_pid, libpad_h,    "scePadGetHandle");
        fn_open           = resolve_sym(target_pid, libpad_h,    "scePadOpen");
        fn_open_ext       = resolve_sym(target_pid, libpad_h,    "scePadOpenExt");
        fn_open_ext2      = resolve_sym(target_pid, libpad_h,    "scePadOpenExt2");
        fn_insert         = 0;
        fn_vdi            = resolve_sym(target_pid, libpad_h,    "scePadVirtualDeviceInsertData");
        fn_gethandle_ext  = fn_gethandle;
        fn_vda            = resolve_sym(target_pid, libpad_h,    "scePadVirtualDeviceAddDevice");
        fn_del            = resolve_sym(target_pid, libpad_h,    "scePadVirtualDeviceDeleteDevice");
        fn_setpriv        = resolve_sym(target_pid, libpad_h,    "scePadSetProcessPrivilege");
        fn_setloginuser   = resolve_sym(target_pid, libpad_h,    "scePadSetLoginUserNumber");
        fn_setusernumber  = resolve_sym(target_pid, libpad_h,    "scePadSetUserNumber");
        fn_setfocus       = resolve_sym(target_pid, libpad_h,    "scePadSetProcessFocus");
        fn_usleep         = resolve_sym(target_pid, libkernel_h, "usleep");
        fn_pthread_create = resolve_sym(target_pid, libkernel_h, "pthread_create");
        fn_mmap           = resolve_sym(target_pid, libkernel_h, "mmap");
        if (liblibc_h) {
            fn_malloc = resolve_sym(target_pid, liblibc_h, "malloc");
            fn_free   = resolve_sym(target_pid, liblibc_h, "free");
        }

        if (libpthread_h) {
            if (!fn_pthread_create)
                fn_pthread_create = resolve_sym(target_pid, libpthread_h, "pthread_create");
            if (!fn_usleep)
                fn_usleep = resolve_sym(target_pid, libpthread_h, "usleep");
        }

        klog_printf("[Ghostpad] scePadGetHandle                 @ 0x%lx\n", fn_gethandle);
        klog_printf("[Ghostpad] scePadOpen                      @ 0x%lx\n", fn_open);
        klog_printf("[Ghostpad] scePadOpenExt                   @ 0x%lx\n", fn_open_ext);
        klog_printf("[Ghostpad] scePadOpenExt2                  @ 0x%lx\n", fn_open_ext2);
        klog_printf("[Ghostpad] scePadVirtualDeviceInsertData   @ 0x%lx\n", fn_vdi);
        klog_printf("[Ghostpad] scePadVirtualDeviceAddDevice    @ 0x%lx\n", fn_vda);
        klog_printf("[Ghostpad] scePadVirtualDeviceDeleteDevice @ 0x%lx\n", fn_del);
        klog_printf("[Ghostpad] scePadSetProcessPrivilege       @ 0x%lx\n", fn_setpriv);
        klog_printf("[Ghostpad] scePadSetLoginUserNumber       @ 0x%lx\n", fn_setloginuser);
        klog_printf("[Ghostpad] scePadSetUserNumber            @ 0x%lx\n", fn_setusernumber);
        klog_printf("[Ghostpad] scePadSetProcessFocus          @ 0x%lx\n", fn_setfocus);
        klog_printf("[Ghostpad] usleep                          @ 0x%lx\n", fn_usleep);
        klog_printf("[Ghostpad] pthread_create                  @ 0x%lx\n", fn_pthread_create);
        klog_printf("[Ghostpad] mmap                            @ 0x%lx\n", fn_mmap);
        klog_printf("[Ghostpad] malloc                          @ 0x%lx\n", fn_malloc);
        klog_printf("[Ghostpad] free                            @ 0x%lx\n", fn_free);

        if (force_virtual_vda) {
            if (!fn_vda || !fn_vdi || !fn_usleep || !fn_pthread_create) {
                klog_printf("[Ghostpad] VDA symbol resolution failed in %s\n", target_name);
                sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
                continue;
            }
        } else if (!fn_gethandle || !fn_vdi || !fn_usleep || !fn_pthread_create) {
            klog_printf("[Ghostpad] symbol resolution failed in %s\n", target_name);
            sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
            continue;
        }

        if (force_virtual_vda) {
            stub_src = (const void *)shellui_stub_force_vda;
            stub_end = (const void *)shellui_stub_force_vda_end;
        } else {
            stub_src = (const void *)shellui_stub;
            stub_end = (const void *)shellui_stub_end;
        }

        stub_code_size = (size_t)((const char *)stub_end - (const char *)stub_src);
        args_offset    = (16 + stub_code_size + 15) & ~(size_t)15;
        alloc_size     = args_offset + sizeof(ShellUiPadArgs) + 16;

        klog_printf("[Ghostpad] stub_code=%zu args_off=%zu alloc=%zu force_vda=%d\n",
                    stub_code_size, args_offset, alloc_size, force_virtual_vda);

        init_mem = kernel_dynlib_init_addr(target_pid, libpad_h);
        fini_mem = kernel_dynlib_fini_addr(target_pid, libpad_h);
        trap_mem = init_mem ? init_mem : fini_mem;
        if (!trap_mem) {
            klog_printf("[Ghostpad] no code cave available in %s\n", target_name);
            sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
            continue;
        }

        klog_printf("[Ghostpad] init cave @ 0x%lx  fini cave @ 0x%lx\n",
                    init_mem, fini_mem);

        stub_alloc_size = (16 + stub_code_size + 15) & ~(size_t)15;
        args_alloc_size = sizeof(ShellUiPadArgs) + 16;

        if (kernel_set_vmem_protection(target_pid, trap_mem, stub_alloc_size,
                                       PROT_READ | PROT_WRITE | PROT_EXEC)) {
            klog_printf("[Ghostpad] kernel_set_vmem_protection(RWX trap) failed in %s\n", target_name);
            sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
            continue;
        }
        klog_printf("[Ghostpad] trap cave @ 0x%lx RWX ok\n", trap_mem);

        stub_mem = trap_mem;
        args_mem = stub_mem;
        pt_io_write(target_pid, stub_mem, &int3, 1);
        if (fn_malloc) {
            int64_t malloc_ret = pt_call(target_pid, fn_malloc, stub_mem,
                                         (uint64_t)args_alloc_size, 0, 0, 0, 0, 0);
            if (malloc_ret > 0) {
                args_mem = (intptr_t)malloc_ret;
                klog_printf("[Ghostpad] malloc args block @ 0x%lx (%zu bytes)\n",
                            args_mem, args_alloc_size);
            } else {
                klog_printf("[Ghostpad] malloc args block failed -> %lld; falling back to code cave storage\n",
                            (long long)malloc_ret);
            }
        }
        if (args_mem == stub_mem && init_mem && fini_mem && init_mem != fini_mem) {
            args_mem = (stub_mem == init_mem) ? fini_mem : init_mem;
            if (kernel_set_vmem_protection(target_pid, args_mem, args_alloc_size,
                                          PROT_READ | PROT_WRITE | PROT_EXEC)) {
                klog_printf("[Ghostpad] separate args cave protection failed in %s; reusing stub cave\n",
                            target_name);
                args_mem = stub_mem;
            }
        }

        if (args_mem != stub_mem && args_mem != init_mem && args_mem != fini_mem) {
            klog_printf("[Ghostpad] conservative stub cave @ 0x%lx  heap args block @ 0x%lx\n",
                        stub_mem, args_mem);
            args_kaddr = args_mem;
        } else if (args_mem == stub_mem) {
            klog_printf("[Ghostpad] conservative code cave @ 0x%lx RWX ok (shared stub/args, mmap disabled)\n",
                        stub_mem);
            args_kaddr = stub_mem + (intptr_t)args_offset;
        } else {
            klog_printf("[Ghostpad] conservative stub cave @ 0x%lx  separate args cave @ 0x%lx\n",
                        stub_mem, args_mem);
            args_kaddr = args_mem + 16;
        }

        thread_storage = stub_mem + 8;
        stub_fn        = stub_mem + 16;
        klog_printf("[Ghostpad] stub_fn=0x%lx  args=0x%lx\n", stub_fn, args_kaddr);

        if (fn_setpriv) {
            int64_t spriv = pt_call(target_pid, fn_setpriv, stub_mem, 1, 0, 0, 0, 0, 0);
            klog_printf("[Ghostpad] scePadSetProcessPrivilege(1) in %s -> %lld\n",
                        target_name, (long long)spriv);
        }

        is_shellcore = (strcmp(target_name, "SceShellCore") == 0);
        is_shellui = (strcmp(target_name, "SceShellUI") == 0);

        if (!force_virtual_vda && !is_shellcore && fn_gethandle) {
            int32_t try_users[2];
            int tu, ti;
            try_users[0] = userId;
            try_users[1] = 0x10000000;
            for (tu = 0; tu < 2 && pt_pad_handle < 0; tu++) {
                for (ti = 0; ti < 4 && pt_pad_handle < 0; ti++) {
                    int64_t gh = pt_call(target_pid, fn_gethandle, stub_mem,
                                         (uint64_t)try_users[tu], 0, (uint64_t)ti,
                                         0, 0, 0);
                    klog_printf("[Ghostpad] pt_call GetHandle(0x%x,0,%d) -> 0x%llx\n",
                                try_users[tu], ti, (unsigned long long)(uint64_t)gh);
                    if ((int32_t)gh >= 0) { pt_pad_handle = (int32_t)gh; pt_use_insert = 0; }
                }
            }
        }

        if (!force_virtual_vda && !is_shellcore && pt_pad_handle < 0 && fn_open) {
            int32_t try_users[2];
            int tu2;
            try_users[0] = userId;
            try_users[1] = 0x10000000;
            for (tu2 = 0; tu2 < 2 && pt_pad_handle < 0; tu2++) {
                int64_t oh = pt_call(target_pid, fn_open, stub_mem,
                                     (uint64_t)try_users[tu2], 0, 0, 0, 0, 0);
                klog_printf("[Ghostpad] pt_call scePadOpen(0x%x,0) -> 0x%llx\n",
                            try_users[tu2], (unsigned long long)(uint64_t)oh);
                if ((int32_t)oh >= 0) { pt_pad_handle = (int32_t)oh; pt_use_insert = 1; }
            }
        }

        if (force_virtual_vda && !is_shellcore && pt_pad_handle < 0 && fn_open) {
            int32_t try_users[2];
            int tu2;
            try_users[0] = userId;
            try_users[1] = 0x10000000;
            for (tu2 = 0; tu2 < 2 && pt_pad_handle < 0; tu2++) {
                int64_t oh = pt_call(target_pid, fn_open, stub_mem,
                                     (uint64_t)try_users[tu2], 0, 0, 0, 0, 0);
                klog_printf("[Ghostpad] pt_call fallback scePadOpen(0x%x,0) -> 0x%llx\n",
                            try_users[tu2], (unsigned long long)(uint64_t)oh);
                if ((int32_t)oh >= 0) { pt_pad_handle = (int32_t)oh; pt_use_insert = 1; }
            }
        }

        if (force_virtual_vda && !is_shellcore && pt_pad_handle < 0 && (fn_gethandle_ext || fn_gethandle)) {
            int32_t try_users[2];
            int tu, ti;
            try_users[0] = userId;
            try_users[1] = 0x10000000;
            for (tu = 0; tu < 2 && pt_pad_handle < 0; tu++) {
                for (ti = 0; ti < 4 && pt_pad_handle < 0; ti++) {
                    int64_t gh = fn_gethandle_ext
                        ? pt_call(target_pid, fn_gethandle_ext, stub_mem,
                                  (uint64_t)try_users[tu], 0, (uint64_t)ti,
                                  0, 0, 0)
                        : pt_call(target_pid, fn_gethandle, stub_mem,
                                  (uint64_t)try_users[tu], 0, (uint64_t)ti,
                                  0, 0, 0);
                    klog_printf("[Ghostpad] pt_call fallback GetHandle(0x%x,0,%d) -> 0x%llx\n",
                                try_users[tu], ti, (unsigned long long)(uint64_t)gh);
                    if ((int32_t)gh >= 0) { pt_pad_handle = (int32_t)gh; pt_use_insert = 1; }
                }
            }
        }

        if (force_virtual_vda && !is_shellcore && pt_pad_handle < 0 &&
            (fn_open_ext2 || fn_open_ext)) {
            int32_t try_users[2];
            int tu2;
            try_users[0] = userId;
            try_users[1] = 0x10000000;
            for (tu2 = 0; tu2 < 2 && pt_pad_handle < 0; tu2++) {
                int64_t oh = fn_open_ext2
                    ? pt_call(target_pid, fn_open_ext2, stub_mem,
                              (uint64_t)try_users[tu2], 0, 0, 0, 0, 0)
                    : (fn_open_ext
                        ? pt_call(target_pid, fn_open_ext, stub_mem,
                                  (uint64_t)try_users[tu2], 0, 0, 0, 0, 0)
                        : pt_call(target_pid, fn_open, stub_mem,
                                  (uint64_t)try_users[tu2], 0, 0, 0, 0, 0));
                klog_printf("[Ghostpad] pt_call fallback scePadOpen*(0x%x,0) -> 0x%llx\n",
                            try_users[tu2], (unsigned long long)(uint64_t)oh);
                if ((int32_t)oh >= 0) { pt_pad_handle = (int32_t)oh; pt_use_insert = 1; }
            }
        }

        if (force_virtual_vda && pt_pad_handle >= 0)
            klog_printf("[Ghostpad] force_virtual_vda=1: captured fallback handle=%d use_insert=%d; stub will try VDA first\n",
                        pt_pad_handle, pt_use_insert);
        else if (force_virtual_vda)
            klog_printf("[Ghostpad] force_virtual_vda=1: no pt_call fallback handle; stub will try VDA in thread context\n");
        else if (pt_pad_handle >= 0)
            klog_printf("[Ghostpad] pt_call handle=%d use_insert=%d — stub skips IPC\n",
                        pt_pad_handle, pt_use_insert);
        else if (!is_shellcore)
            klog_printf("[Ghostpad] pt_call GetHandle/Open all failed — stub pad_handle=-2\n");

        if (is_shellui) {
            klog_printf("[Ghostpad] skipping unsafe pthread_create in %s: thread launch remains crash-prone on this firmware\n",
                        target_name);
            sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
            continue;
        }

        if (!force_virtual_vda && !is_shellcore && pt_pad_handle < 0) {
            klog_printf("[Ghostpad] skipping unsafe pthread_create in %s: no usable pt_call pad handle\n",
                        target_name);
            sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
            continue;
        }

        pt_io_write(target_pid, stub_fn, (void *)stub_src, stub_code_size);

        {
            ShellUiPadArgs args;
            memset(&args, 0, sizeof(args));
            args.fp_gethandle = (void *)fn_gethandle;
            args.fp_gethandle_ext = (void *)fn_gethandle_ext;
            args.fp_open      = (void *)fn_open;
            args.fp_open_ext  = (void *)fn_open_ext;
            args.fp_open_ext2 = (void *)fn_open_ext2;
            args.fp_insert    = (void *)fn_insert;
            args.fp_vdi       = (void *)fn_vdi;
            args.fp_vda       = (void *)fn_vda;
            args.fp_del       = (void *)fn_del;
            args.fp_setpriv   = (void *)fn_setpriv;
            args.fp_setloginuser = (void *)fn_setloginuser;
            args.fp_setusernumber = (void *)fn_setusernumber;
            args.fp_setfocus  = (void *)fn_setfocus;
            args.fp_usleep    = (void *)fn_usleep;
            args.userId       = userId;
            args.virtual_device_type = virtual_device_type;
            args.pad_handle   = (pt_pad_handle >= 0) ? pt_pad_handle : ((is_shellcore || force_virtual_vda) ? -1 : -2);
            args.seq          = (uint32_t)pt_use_insert;
            if (force_virtual_vda) {
                args.pad_handle = -1;
                args.rc_log[0] = pt_pad_handle;
                args.rc_log[1] = pt_use_insert;
            }
            pt_io_write(target_pid, args_kaddr, &args, sizeof(args));
        }

        {
            int64_t pret = pt_call(target_pid, fn_pthread_create, stub_mem,
                                   (uint64_t)thread_storage, 0,
                                   (uint64_t)stub_fn, (uint64_t)args_kaddr, 0, 0);
            klog_printf("[Ghostpad] pthread_create(%s) -> %lld\n",
                        target_name, (long long)pret);
            if (pret == 0) {
                /* Save relaunch state so post-GBND can restart stub with a known handle */
                g_relaunch_pid            = target_pid;
                g_relaunch_args_kaddr     = args_kaddr;
                g_relaunch_stub_fn        = stub_fn;
                g_relaunch_thread_storage = thread_storage;
                g_relaunch_pthread_fn     = fn_pthread_create;
                g_relaunch_trap_rip       = stub_mem;  /* stub_mem+0 is the INT3 trap */
                g_relaunch_malloc_fn      = fn_malloc;
                klog_printf("[Ghostpad] relaunch state saved: stub_fn=0x%lx pthread=0x%lx trap=0x%lx\n",
                            stub_fn, fn_pthread_create, stub_mem);

                if (pt_pad_handle >= 0 &&
                    ((pt_use_insert && fn_insert) || (!pt_use_insert && fn_vdi))) {
                    g_shellui_direct_state.valid = 1;
                    g_shellui_direct_state.attached = 0;
                    g_shellui_direct_state.pid = target_pid;
                    g_shellui_direct_state.args_kaddr = args_kaddr;
                    g_shellui_direct_state.trap_rip = stub_mem;
                    g_shellui_direct_state.fn_setpriv = fn_setpriv;
                    g_shellui_direct_state.fn_setloginuser = fn_setloginuser;
                    g_shellui_direct_state.fn_setusernumber = fn_setusernumber;
                    g_shellui_direct_state.fn_setfocus = fn_setfocus;
                    g_shellui_direct_state.fn_usleep = fn_usleep;
                    g_shellui_direct_state.fn_gethandle = fn_gethandle;
                    g_shellui_direct_state.fn_gethandle_ext = fn_gethandle_ext;
                    g_shellui_direct_state.fn_open = fn_open;
                    g_shellui_direct_state.fn_open_ext = fn_open_ext;
                    g_shellui_direct_state.fn_open_ext2 = fn_open_ext2;
                    g_shellui_direct_state.fn_insert = fn_insert;
                    g_shellui_direct_state.fn_vdi = fn_vdi;
                    g_shellui_direct_state.pad_handle = pt_pad_handle;
                    g_shellui_direct_state.use_insert = pt_use_insert ? 1 : 0;
                    klog_printf("[Ghostpad] cached direct insert path handle=%d use_insert=%d trap=0x%lx\n",
                                pt_pad_handle, pt_use_insert ? 1 : 0, stub_mem);
                } else if (force_virtual_vda) {
                    g_shellui_direct_state.valid = 1;
                    g_shellui_direct_state.attached = 0;
                    g_shellui_direct_state.pid = target_pid;
                    g_shellui_direct_state.args_kaddr = args_kaddr;
                    g_shellui_direct_state.trap_rip = stub_mem;
                    g_shellui_direct_state.fn_setpriv = fn_setpriv;
                    g_shellui_direct_state.fn_setloginuser = fn_setloginuser;
                    g_shellui_direct_state.fn_setusernumber = fn_setusernumber;
                    g_shellui_direct_state.fn_setfocus = fn_setfocus;
                    g_shellui_direct_state.fn_usleep = fn_usleep;
                    g_shellui_direct_state.fn_gethandle = fn_gethandle;
                    g_shellui_direct_state.fn_gethandle_ext = fn_gethandle_ext;
                    g_shellui_direct_state.fn_open = fn_open;
                    g_shellui_direct_state.fn_open_ext = fn_open_ext;
                    g_shellui_direct_state.fn_open_ext2 = fn_open_ext2;
                    g_shellui_direct_state.fn_insert = fn_insert;
                    g_shellui_direct_state.fn_vdi = fn_vdi;
                    g_shellui_direct_state.pad_handle = -1;
                    g_shellui_direct_state.use_insert = 0;
                    klog_printf("[Ghostpad] cached direct recovery context trap=0x%lx (no initial handle)\n",
                                stub_mem);
                } else {
                    memset(&g_shellui_direct_state, 0, sizeof(g_shellui_direct_state));
                }
                *out_shellui_pid = target_pid;
                *out_args_kaddr  = args_kaddr;
                sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
                klog_printf("[Ghostpad] detached from %s  ret=0\n", target_name);
                return 0;
            }
            klog_printf("[Ghostpad] pthread_create failed in %s\n", target_name);
            if (fn_free && args_mem != 0 && args_mem != stub_mem &&
                args_mem != init_mem && args_mem != fini_mem) {
                int64_t free_ret = pt_call(target_pid, fn_free, stub_mem,
                                           (uint64_t)args_mem, 0, 0, 0, 0, 0);
                klog_printf("[Ghostpad] free(args_mem=0x%lx) -> %lld\n",
                            args_mem, (long long)free_ret);
            }
        }

        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        klog_printf("[Ghostpad] detached from %s  ret=-1\n", target_name);
        }
    }

    klog_printf("[Ghostpad] no attachable injection target found\n");
    return -1;
}

/* shellui_pad_update — write new pad data at 60 Hz via mdbg_copyin */
int
shellui_pad_update(pid_t shellui_pid, intptr_t args_kaddr,
                   const void *pad_data, uint32_t pad_data_len)
{
    static pid_t cached_pid = -1;
    static intptr_t cached_args = 0;
    static uint32_t cached_seq = 0;
    static int logged_context = 0;
    static int logged_data_copy_failure = 0;
    static int logged_seq_copy_failure = 0;

    if (pad_data_len > SHELLUI_PAD_DATA_SIZE)
        pad_data_len = SHELLUI_PAD_DATA_SIZE;

    if (shellui_pid != cached_pid || args_kaddr != cached_args) {
        uint32_t observed_seq = (uint32_t)mdbg_getint(
            shellui_pid,
            args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, seq));
        cached_pid = shellui_pid;
        cached_args = args_kaddr;
        /*
         * Target-side seq readback can degrade to 0 after startup on the
         * working assignment-screen recovery path. Seed from the observed
         * value when available, otherwise jump to 1 so the first outbound
         * write still advances the stub's last-seen sequence.
         */
        cached_seq = (observed_seq != 0) ? observed_seq : 1;
        if (!logged_context) {
            klog_printf("[Ghostpad] shellui_pad_update context pid=%d args=0x%lx observed_seq=%u cached_seq=%u ready=%d handle=%d\n",
                        shellui_pid, (unsigned long)args_kaddr, observed_seq,
                        cached_seq,
                        (int32_t)mdbg_getint(shellui_pid,
                            args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, ready)),
                        (int32_t)mdbg_getint(shellui_pid,
                            args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, pad_handle)));
            logged_context = 1;
        }
    }

    uint32_t new_seq = cached_seq + 1;
    intptr_t data_field = args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, pad_data);

    /* Elevate to ptrace authid for mdbg_copyin; restore game authid immediately after */
    pid_t    _mypid    = getpid();
    uint64_t _saved_au = kernel_get_ucred_authid(_mypid);
    if (_saved_au) kernel_set_ucred_authid(_mypid, 0x4800000000010003l);

    int copy_ret = mdbg_copyin(shellui_pid, pad_data, data_field, pad_data_len);
    if (_saved_au) kernel_set_ucred_authid(_mypid, _saved_au);

    if (copy_ret) {
        if (!logged_data_copy_failure) {
            klog_printf("[Ghostpad] shellui_pad_update data copy failed ret=%d errno=%d pid=%d\n",
                        copy_ret, errno, shellui_pid);
            logged_data_copy_failure = 1;
        }
        if (shellui_pad_ptrace_update(shellui_pid, args_kaddr, pad_data,
                                      pad_data_len, new_seq) == 0) {
            cached_seq = new_seq;
            return 0;
        }
        return -1;
    }

    if (shellui_pid != cached_pid || args_kaddr != cached_args) {
        uint32_t observed_seq = (uint32_t)mdbg_getint(
            shellui_pid,
            args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, seq));
        cached_pid = shellui_pid;
        cached_args = args_kaddr;
        /*
         * Target-side seq readback can degrade to 0 after startup on the
         * working assignment-screen recovery path. Seed from the observed
         * value when available, otherwise jump to 1 so the first outbound
         * write still advances the stub's last-seen sequence.
         */
        cached_seq = (observed_seq != 0) ? observed_seq : 1;
        if (!logged_context) {
            klog_printf("[Ghostpad] shellui_pad_update context pid=%d args=0x%lx observed_seq=%u cached_seq=%u ready=%d handle=%d\n",
                        shellui_pid, (unsigned long)args_kaddr, observed_seq,
                        cached_seq,
                        (int32_t)mdbg_getint(shellui_pid,
                            args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, ready)),
                        (int32_t)mdbg_getint(shellui_pid,
                            args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, pad_handle)));
            logged_context = 1;
        }
    }

    intptr_t seq_field = args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, seq);
    _saved_au = kernel_get_ucred_authid(_mypid);
    if (_saved_au) kernel_set_ucred_authid(_mypid, 0x4800000000010003l);
    copy_ret = mdbg_copyin(shellui_pid, &new_seq, seq_field, 4);
    if (_saved_au) kernel_set_ucred_authid(_mypid, _saved_au);
    if (copy_ret) {
        if (!logged_seq_copy_failure) {
            klog_printf("[Ghostpad] shellui_pad_update seq copy failed ret=%d errno=%d pid=%d args=0x%lx seq=0x%lx old=%u new=%u\n",
                        copy_ret, errno, shellui_pid, (unsigned long)args_kaddr,
                        (unsigned long)seq_field, cached_seq, new_seq);
            logged_seq_copy_failure = 1;
        }
        if (shellui_pad_ptrace_update(shellui_pid, args_kaddr, pad_data,
                                      pad_data_len, new_seq) == 0) {
            cached_seq = new_seq;
            return 0;
        }
        return -1;
    }

    cached_seq = new_seq;
    return 0;
}

int
shellui_pad_stop(pid_t shellui_pid, intptr_t args_kaddr)
{
    int32_t stop = 1;
    intptr_t stop_addr =
        args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, stop);
    pid_t self = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (saved_authid)
        kernel_set_ucred_authid(self, 0x4800000000010003l);
    int result =
        mdbg_copyin(shellui_pid, &stop, stop_addr, sizeof(stop));
    if (saved_authid)
        kernel_set_ucred_authid(self, saved_authid);
    if (result == 0)
        return 0;

    klog_printf("[Ghostpad] shellui_pad_stop: mdbg failed ret=%d, "
                "using one-time ptrace write\n", result);
    if (sys_ptrace(PT_ATTACH, shellui_pid, 0, 0) != 0)
        return -1;
    waitpid(shellui_pid, NULL, 0);
    result = pt_io_write(shellui_pid, stop_addr, &stop, sizeof(stop));
    (void)sys_ptrace(PT_DETACH, shellui_pid, (caddr_t)1, 0);
    return result == 0 ? 0 : -1;
}

int
shellui_pad_direct_usable(pid_t shellui_pid, intptr_t args_kaddr)
{
    return shellui_pad_direct_context_usable(shellui_pid, args_kaddr) &&
           g_shellui_direct_state.pad_handle >= 0 &&
           !g_shellui_direct_state.use_insert;
}

int
shellui_pad_direct_mode(pid_t shellui_pid, intptr_t args_kaddr)
{
    if (!shellui_pad_direct_context_usable(shellui_pid, args_kaddr) ||
        g_shellui_direct_state.pad_handle < 0) {
        return -1;
    }
    return g_shellui_direct_state.use_insert ? 1 : 0;
}

int
shellui_pad_direct_adopt_vdi_handle(pid_t shellui_pid, intptr_t args_kaddr,
                                    int32_t vdi_handle)
{
    if (!shellui_pad_direct_context_usable(shellui_pid, args_kaddr) ||
        !g_shellui_direct_state.fn_vdi ||
        vdi_handle <= 0) {
        return -1;
    }
    g_shellui_direct_state.pad_handle = vdi_handle;
    g_shellui_direct_state.use_insert = 0;
    shellui_pad_direct_set_last_status(0x7100, vdi_handle);
    klog_printf("[Ghostpad] direct_adopt_vdi_handle handle=0x%x pid=%d trap=0x%lx\n",
                (uint32_t)vdi_handle, shellui_pid,
                (unsigned long)g_shellui_direct_state.trap_rip);
    return 0;
}

int
shellui_pad_direct_recover(pid_t shellui_pid, intptr_t args_kaddr, int32_t userId, int32_t altUserId)
{
    int32_t try_users[4];
    int try_user_count = 0;
    int32_t handle = -1;
    int use_insert = 0;
    int begin_ret = 0;
    int64_t setup_ret = 0;

    shellui_pad_direct_set_last_status(0x1000, 0);
    if (!shellui_pad_direct_context_usable(shellui_pid, args_kaddr)) {
        shellui_pad_direct_set_last_status(0x1001, -2);
        klog_printf("[Ghostpad] direct_recover context mismatch: requested pid=%d args=0x%lx cached valid=%d pid=%d args=0x%lx handle=%d attached=%d\n",
                    shellui_pid,
                    (unsigned long)args_kaddr,
                    g_shellui_direct_state.valid ? 1 : 0,
                    g_shellui_direct_state.pid,
                    (unsigned long)g_shellui_direct_state.args_kaddr,
                    g_shellui_direct_state.pad_handle,
                    g_shellui_direct_state.attached ? 1 : 0);
        return -2;
    }
    if (g_shellui_direct_state.pad_handle >= 0) {
        shellui_pad_direct_set_last_status(0x1002, g_shellui_direct_state.pad_handle);
        klog_printf("[Ghostpad] direct_recover reusing cached handle=%d use_insert=%d\n",
                    g_shellui_direct_state.pad_handle,
                    g_shellui_direct_state.use_insert ? 1 : 0);
        return 0;
    }
    begin_ret = shellui_pad_direct_begin(shellui_pid, args_kaddr);
    if (begin_ret != 0) {
        shellui_pad_direct_set_last_status(0x1003, begin_ret);
        klog_printf("[Ghostpad] direct_recover begin failed ret=%d pid=%d args=0x%lx\n",
                    begin_ret, shellui_pid, (unsigned long)args_kaddr);
        return begin_ret;
    }

    if (g_shellui_direct_state.fn_setpriv) {
        setup_ret = pt_call(shellui_pid, g_shellui_direct_state.fn_setpriv, g_shellui_direct_state.trap_rip,
                            1, 0, 0, 0, 0, 0);
        shellui_pad_direct_set_last_status(0x1101, setup_ret);
        klog_printf("[Ghostpad] direct_recover setpriv(1) -> 0x%llx\n",
                    (unsigned long long)(uint64_t)setup_ret);
    }
    if (g_shellui_direct_state.fn_setloginuser) {
        setup_ret = pt_call(shellui_pid, g_shellui_direct_state.fn_setloginuser, g_shellui_direct_state.trap_rip,
                            1, 0, 0, 0, 0, 0);
        shellui_pad_direct_set_last_status(0x1102, setup_ret);
        klog_printf("[Ghostpad] direct_recover setloginuser(1) -> 0x%llx\n",
                    (unsigned long long)(uint64_t)setup_ret);
    }
    if (g_shellui_direct_state.fn_setusernumber) {
        setup_ret = pt_call(shellui_pid, g_shellui_direct_state.fn_setusernumber, g_shellui_direct_state.trap_rip,
                            1, 0, 0, 0, 0, 0);
        shellui_pad_direct_set_last_status(0x1103, setup_ret);
        klog_printf("[Ghostpad] direct_recover setusernumber(1) -> 0x%llx\n",
                    (unsigned long long)(uint64_t)setup_ret);
    }
    if (g_shellui_direct_state.fn_setfocus) {
        setup_ret = pt_call(shellui_pid, g_shellui_direct_state.fn_setfocus, g_shellui_direct_state.trap_rip,
                            1, 0, 0, 0, 0, 0);
        shellui_pad_direct_set_last_status(0x1104, setup_ret);
        klog_printf("[Ghostpad] direct_recover setfocus(1) -> 0x%llx\n",
                    (unsigned long long)(uint64_t)setup_ret);
    }
    if (g_shellui_direct_state.fn_usleep) {
        setup_ret = pt_call(shellui_pid, g_shellui_direct_state.fn_usleep, g_shellui_direct_state.trap_rip,
                            150000, 0, 0, 0, 0, 0);
        shellui_pad_direct_set_last_status(0x1105, setup_ret);
        klog_printf("[Ghostpad] direct_recover usleep(150000) -> 0x%llx\n",
                    (unsigned long long)(uint64_t)setup_ret);
    }

    try_users[try_user_count++] = 1;
    if (userId >= 0 && userId != try_users[0]) {
        try_users[try_user_count++] = userId;
    }
    if (0x10000000 != try_users[0] &&
        (try_user_count < 2 || 0x10000000 != try_users[1])) {
        try_users[try_user_count++] = 0x10000000;
    }
    if (altUserId >= 0) {
        int seen = 0;
        for (int ui = 0; ui < try_user_count; ui++) {
            if (try_users[ui] == altUserId) {
                seen = 1;
                break;
            }
        }
        if (!seen && try_user_count < (int)(sizeof(try_users) / sizeof(try_users[0]))) {
            try_users[try_user_count++] = altUserId;
        }
    }

    if (g_shellui_direct_state.fn_gethandle_ext || g_shellui_direct_state.fn_gethandle) {
        for (int ui = 0; ui < try_user_count && handle < 0; ui++) {
            for (int idx = 0; idx < 8 && handle < 0; idx++) {
                int64_t gh = g_shellui_direct_state.fn_gethandle_ext
                    ? pt_call(shellui_pid, g_shellui_direct_state.fn_gethandle_ext, g_shellui_direct_state.trap_rip,
                              (uint64_t)try_users[ui], 3, (uint64_t)idx, 0, 0, 0)
                    : pt_call(shellui_pid, g_shellui_direct_state.fn_gethandle, g_shellui_direct_state.trap_rip,
                              (uint64_t)try_users[ui], 3, (uint64_t)idx, 0, 0, 0);
                shellui_pad_direct_set_last_status(0x2000 | (idx & 0xff), gh);
                klog_printf("[Ghostpad] direct_recover GetHandle(0x%x,3,%d) -> 0x%llx\n",
                            try_users[ui], idx, (unsigned long long)(uint64_t)gh);
                if ((int32_t)gh >= 0) {
                    handle = (int32_t)gh;
                    use_insert = 0;
                }
            }
        }
    }

    if (handle < 0 &&
        g_shellui_direct_state.fn_usleep &&
        (g_shellui_direct_state.fn_gethandle_ext || g_shellui_direct_state.fn_gethandle)) {
        for (int attempt = 0; attempt < 60 && handle < 0; attempt++) {
            for (int ui = 0; ui < try_user_count && handle < 0; ui++) {
                for (int idx = 0; idx < 8 && handle < 0; idx++) {
                    int64_t gh = g_shellui_direct_state.fn_gethandle_ext
                        ? pt_call(shellui_pid, g_shellui_direct_state.fn_gethandle_ext, g_shellui_direct_state.trap_rip,
                                  (uint64_t)try_users[ui], 3, (uint64_t)idx, 0, 0, 0)
                        : pt_call(shellui_pid, g_shellui_direct_state.fn_gethandle, g_shellui_direct_state.trap_rip,
                                  (uint64_t)try_users[ui], 3, (uint64_t)idx, 0, 0, 0);
                    shellui_pad_direct_set_last_status(0x2100 | ((attempt & 0xff) << 4) | (idx & 0xf), gh);
                    klog_printf("[Ghostpad] direct_recover retry GetHandle(0x%x,3,%d) attempt=%d -> 0x%llx\n",
                                try_users[ui], idx, attempt, (unsigned long long)(uint64_t)gh);
                    if ((int32_t)gh >= 0) {
                        handle = (int32_t)gh;
                        use_insert = 0;
                    }
                }
            }
            if (handle < 0) {
                int64_t sleep_ret = pt_call(shellui_pid, g_shellui_direct_state.fn_usleep, g_shellui_direct_state.trap_rip,
                                            150000, 0, 0, 0, 0, 0);
                shellui_pad_direct_set_last_status(0x2200 | (attempt & 0xff), sleep_ret);
            }
        }
    }

    if (handle < 0 &&
        (g_shellui_direct_state.fn_gethandle_ext || g_shellui_direct_state.fn_gethandle)) {
        for (int ui = 0; ui < try_user_count && handle < 0; ui++) {
            for (int idx = 0; idx < 4 && handle < 0; idx++) {
                int64_t gh = g_shellui_direct_state.fn_gethandle_ext
                    ? pt_call(shellui_pid, g_shellui_direct_state.fn_gethandle_ext, g_shellui_direct_state.trap_rip,
                              (uint64_t)try_users[ui], 0, (uint64_t)idx, 0, 0, 0)
                    : pt_call(shellui_pid, g_shellui_direct_state.fn_gethandle, g_shellui_direct_state.trap_rip,
                              (uint64_t)try_users[ui], 0, (uint64_t)idx, 0, 0, 0);
                shellui_pad_direct_set_last_status(0x3000 | (idx & 0xff), gh);
                klog_printf("[Ghostpad] direct_recover GetHandle(0x%x,0,%d) -> 0x%llx\n",
                            try_users[ui], idx, (unsigned long long)(uint64_t)gh);
                if ((int32_t)gh >= 0) {
                    handle = (int32_t)gh;
                    use_insert = 1;
                }
            }
        }
    }

    if (handle < 0 && g_shellui_direct_state.fn_open) {
        for (int ui = 0; ui < try_user_count && handle < 0; ui++) {
            int64_t oh = pt_call(shellui_pid, g_shellui_direct_state.fn_open, g_shellui_direct_state.trap_rip,
                                 (uint64_t)try_users[ui], 3, 0, 0, 0, 0);
            shellui_pad_direct_set_last_status(0x5002, oh);
            klog_printf("[Ghostpad] direct_recover scePadOpen(0x%x,3) -> 0x%llx\n",
                        try_users[ui], (unsigned long long)(uint64_t)oh);
            if ((int32_t)oh >= 0) {
                handle = (int32_t)oh;
                use_insert = 0;
            }
        }
    }

    if (handle < 0 && g_shellui_direct_state.fn_open) {
        for (int ui = 0; ui < try_user_count && handle < 0; ui++) {
            int64_t oh = pt_call(shellui_pid, g_shellui_direct_state.fn_open, g_shellui_direct_state.trap_rip,
                                 (uint64_t)try_users[ui], 0, 0, 0, 0, 0);
            shellui_pad_direct_set_last_status(0x5000, oh);
            klog_printf("[Ghostpad] direct_recover scePadOpen(0x%x,0) -> 0x%llx\n",
                        try_users[ui], (unsigned long long)(uint64_t)oh);
            if ((int32_t)oh >= 0) {
                handle = (int32_t)oh;
                use_insert = 1;
            }
        }
    }

    if (handle < 0 &&
        (g_shellui_direct_state.fn_open_ext2 || g_shellui_direct_state.fn_open_ext)) {
        for (int ui = 0; ui < try_user_count && handle < 0; ui++) {
            int64_t oh = g_shellui_direct_state.fn_open_ext2
                ? pt_call(shellui_pid, g_shellui_direct_state.fn_open_ext2, g_shellui_direct_state.trap_rip,
                          (uint64_t)try_users[ui], 3, 0, 0, 0, 0)
                : pt_call(shellui_pid, g_shellui_direct_state.fn_open_ext, g_shellui_direct_state.trap_rip,
                          (uint64_t)try_users[ui], 3, 0, 0, 0, 0);
            shellui_pad_direct_set_last_status(0x6002, oh);
            klog_printf("[Ghostpad] direct_recover scePadOpen*(0x%x,3) -> 0x%llx\n",
                        try_users[ui], (unsigned long long)(uint64_t)oh);
            if ((int32_t)oh >= 0) {
                handle = (int32_t)oh;
                use_insert = 0;
            }
        }
    }

    if (handle < 0 &&
        (g_shellui_direct_state.fn_open_ext2 || g_shellui_direct_state.fn_open_ext)) {
        for (int ui = 0; ui < try_user_count && handle < 0; ui++) {
            int64_t oh = g_shellui_direct_state.fn_open_ext2
                ? pt_call(shellui_pid, g_shellui_direct_state.fn_open_ext2, g_shellui_direct_state.trap_rip,
                          (uint64_t)try_users[ui], 0, 0, 0, 0, 0)
                : pt_call(shellui_pid, g_shellui_direct_state.fn_open_ext, g_shellui_direct_state.trap_rip,
                          (uint64_t)try_users[ui], 0, 0, 0, 0, 0);
            shellui_pad_direct_set_last_status(0x6000, oh);
            klog_printf("[Ghostpad] direct_recover scePadOpen*(0x%x,0) -> 0x%llx\n",
                        try_users[ui], (unsigned long long)(uint64_t)oh);
            if ((int32_t)oh >= 0) {
                handle = (int32_t)oh;
                use_insert = 1;
            }
        }
    }

    if (handle >= 0) {
        g_shellui_direct_state.pad_handle = handle;
        g_shellui_direct_state.use_insert = use_insert ? 1 : 0;
        shellui_pad_direct_set_last_status(0x7000 | (use_insert ? 1 : 0), handle);
        klog_printf("[Ghostpad] direct_recover cached handle=%d use_insert=%d\n",
                    handle, use_insert ? 1 : 0);
        return 0;
    }

    {
        int32_t last_stage = 0;
        int64_t last_value = 0;

        shellui_pad_direct_get_last_status(&last_stage, &last_value);
        klog_printf("[Ghostpad] direct_recover found no usable handle for pid=%d args=0x%lx user=0x%x last_stage=0x%08x last_value=0x%llx\n",
                    shellui_pid,
                    (unsigned long)args_kaddr,
                    (uint32_t)userId,
                    (uint32_t)last_stage,
                    (unsigned long long)(uint64_t)last_value);
    }
    shellui_pad_direct_end(shellui_pid, args_kaddr);
    return -3;
}

int
shellui_pad_direct_begin(pid_t shellui_pid, intptr_t args_kaddr)
{
    int attach_errno = 0;

    if (!shellui_pad_direct_context_usable(shellui_pid, args_kaddr)) {
        return -1;
    }
    if (g_shellui_direct_state.attached) {
        return 0;
    }
    for (int attempt = 0; attempt < 20; attempt++) {
        if (sys_ptrace(PT_ATTACH, shellui_pid, 0, 0) == 0) {
            waitpid(shellui_pid, NULL, 0);
            g_shellui_direct_state.attached = 1;
            if (attempt > 0) {
                klog_printf("[Ghostpad] direct_begin PT_ATTACH(pid=%d) succeeded after %d retries\n",
                            shellui_pid, attempt);
            }
            return 0;
        }
        attach_errno = errno;
        usleep(50000);
    }
    klog_printf("[Ghostpad] direct_begin PT_ATTACH(pid=%d) failed errno=%d\n",
                shellui_pid, attach_errno);
    return -attach_errno;
}

int
shellui_pad_direct_send(pid_t shellui_pid, intptr_t args_kaddr,
                        const void *pad_data, uint32_t pad_data_len)
{
    uint8_t temp[SHELLUI_PAD_DATA_SIZE];
    intptr_t fn;
    int attached_here = 0;
    int ret;

    if (!shellui_pad_direct_context_usable(shellui_pid, args_kaddr) ||
        g_shellui_direct_state.pad_handle < 0) {
        return -1;
    }
    if (g_shellui_direct_state.use_insert) {
        klog_printf("[Ghostpad] direct_send refused unsafe remote insert handle=%d\n",
                    g_shellui_direct_state.pad_handle);
        return -2;
    }

    memset(temp, 0, sizeof(temp));
    if (pad_data_len > SHELLUI_PAD_DATA_SIZE) {
        pad_data_len = SHELLUI_PAD_DATA_SIZE;
    }
    memcpy(temp, pad_data, pad_data_len);

    fn = g_shellui_direct_state.use_insert
       ? g_shellui_direct_state.fn_insert
       : g_shellui_direct_state.fn_vdi;
    if (!fn) {
        return -1;
    }

    if (!g_shellui_direct_state.attached) {
        ret = shellui_pad_direct_begin(shellui_pid, args_kaddr);
        if (ret != 0) {
            return ret;
        }
        attached_here = 1;
    }

    ret = (int)pt_call_with_copy(shellui_pid, fn, g_shellui_direct_state.trap_rip,
                                 (uint64_t)g_shellui_direct_state.pad_handle,
                                 temp, sizeof(temp));
    if (attached_here) {
        shellui_pad_direct_end(shellui_pid, args_kaddr);
    }
    return ret;
}

void
shellui_pad_direct_end(pid_t shellui_pid, intptr_t args_kaddr)
{
    if (!g_shellui_direct_state.attached ||
        !shellui_pad_direct_context_usable(shellui_pid, args_kaddr)) {
        return;
    }
    sys_ptrace(PT_DETACH, shellui_pid, (caddr_t)1, 0);
    g_shellui_direct_state.attached = 0;
}

/* ============================================================
 * shellui_pad_dismiss_assignment_screen
 *
 * Called after SceShellCore VDA injection creates the virtual device.
 * SceShellUI has already opened the device (visible in klog as
 * "Open Pad [deviceId, 0, 0]: ret=HANDLE").  We PT_ATTACH SceShellUI,
 * call scePadGetHandle(userId, type=3, idx) via pt_call (the process
 * executes normally during the call so the pad IPC can respond), then
 * send Cross × 30 frames + release × 18 frames via pt_call_with_copy
 * against scePadVirtualDeviceInsertData.  VDI writes to shared memory
 * so it is safe from the stopped-thread injection context.
 * ============================================================ */
int
shellui_pad_dismiss_assignment_screen(int32_t userId, uint64_t virtualDeviceId)
{
    pid_t pids[16];
    size_t count = find_pids("SceShellUI", pids, 16);
    if (count == 0) {
        klog_printf("[Ghostpad] dismiss: SceShellUI not found\n");
        return -1;
    }
    pid_t target_pid = pids[0];

    klog_printf("[Ghostpad] dismiss: PT_ATTACH(SceShellUI pid=%d)...\n", target_pid);
    if (sys_ptrace(PT_ATTACH, target_pid, 0, 0) != 0) {
        klog_printf("[Ghostpad] dismiss: PT_ATTACH failed errno=%d\n", errno);
        return -1;
    }
    waitpid(target_pid, NULL, 0);
    klog_printf("[Ghostpad] dismiss: attached\n");

    uint32_t libpad_h = 0, libkernel_h = 0;
    if (get_lib(target_pid, "libScePad", &libpad_h)) {
        klog_printf("[Ghostpad] dismiss: libScePad not found in SceShellUI\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }
    get_lib(target_pid, "libkernel_sys", &libkernel_h);

    intptr_t fn_gethandle = resolve_sym(target_pid, libpad_h, "scePadGetHandle");
    intptr_t fn_open      = resolve_sym(target_pid, libpad_h, "scePadOpen");
    intptr_t fn_open_ext  = resolve_sym(target_pid, libpad_h, "scePadOpenExt");
    intptr_t fn_open_ext2 = resolve_sym(target_pid, libpad_h, "scePadOpenExt2");
    intptr_t fn_usleep    = libkernel_h ? resolve_sym(target_pid, libkernel_h, "usleep") : 0;
    intptr_t fn_vdi       = resolve_sym(target_pid, libpad_h, "scePadVirtualDeviceInsertData");
    if (!fn_vdi) {
        klog_printf("[Ghostpad] dismiss: VDI symbol missing\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }
    klog_printf("[Ghostpad] dismiss: GH=0x%lx Open=0x%lx VDI=0x%lx usleep=0x%lx\n",
                fn_gethandle, fn_open, fn_vdi, fn_usleep);

    intptr_t trap_mem = kernel_dynlib_init_addr(target_pid, libpad_h);
    if (!trap_mem) trap_mem = kernel_dynlib_fini_addr(target_pid, libpad_h);
    if (!trap_mem) {
        klog_printf("[Ghostpad] dismiss: no code cave in SceShellUI libScePad\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }
    if (kernel_set_vmem_protection(target_pid, trap_mem, 16,
                                   PROT_READ | PROT_WRITE | PROT_EXEC)) {
        klog_printf("[Ghostpad] dismiss: RWX failed\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }
    uint8_t int3 = 0xCC;
    pt_io_write(target_pid, trap_mem, &int3, 1);
    klog_printf("[Ghostpad] dismiss: trap=0x%lx\n", trap_mem);

    /* -- Phase 1: find the virtual device handle in SceShellUI ----------------
     * The device was created by VDA (userId=0xffffffff).  SceShellUI already
     * opened it ("Open Pad [deviceId,0,0]: ret=HANDLE" in klog).  Try all
     * plausible lookups.  Log every result so we know which call works. */
    int32_t try_uids[4] = {(int32_t)0xffffffff, 0, 1, userId};
    int32_t vd_handle = -1;

    /* A1: GetHandle(userId, type=0, idx=0..7) — after force_bind, the virtual device
     * appears in CIM slot N alongside the physical device.  The physical is at idx=0,
     * the virtual may be at idx=1 (or higher).  Try type=0 first since both show as
     * DualSense in the CIM (sub=22).  Log EVERY result to determine which idx succeeds. */
    if (fn_gethandle) {
        for (int i = 0; i < 4 && vd_handle < 0; i++) {
            for (int idx = 0; idx < 8; idx++) {
                int64_t gh = pt_call(target_pid, fn_gethandle, trap_mem,
                                     (uint64_t)(uint32_t)try_uids[i], 0, (uint64_t)idx,
                                     0, 0, 0);
                klog_printf("[Ghostpad] dismiss: GH(uid=0x%x,type=0,idx=%d)->0x%llx\n",
                            (uint32_t)try_uids[i], idx, (unsigned long long)(uint64_t)gh);
                if ((int32_t)gh >= 0 && vd_handle < 0) vd_handle = (int32_t)gh;
            }
        }
    }

    /* A2: GetHandle with type=3 as fallback */
    if (fn_gethandle && vd_handle < 0) {
        for (int i = 0; i < 4 && vd_handle < 0; i++) {
            for (int idx = 0; idx < 4 && vd_handle < 0; idx++) {
                int64_t gh = pt_call(target_pid, fn_gethandle, trap_mem,
                                     (uint64_t)(uint32_t)try_uids[i], 3, (uint64_t)idx,
                                     0, 0, 0);
                klog_printf("[Ghostpad] dismiss: GH(uid=0x%x,type=3,idx=%d)->0x%llx\n",
                            (uint32_t)try_uids[i], idx, (unsigned long long)(uint64_t)gh);
                if ((int32_t)gh >= 0) vd_handle = (int32_t)gh;
            }
        }
    }

    /* A3: GetHandle(deviceId_low32, type, idx) — direct device ID based lookup */
    if (fn_gethandle && vd_handle < 0 && virtualDeviceId != 0) {
        uint32_t dev32 = (uint32_t)(virtualDeviceId & 0xFFFFFFFF);
        int types[2] = {0, 3};
        for (int t = 0; t < 2 && vd_handle < 0; t++) {
            for (int idx = 0; idx < 4 && vd_handle < 0; idx++) {
                int64_t gh = pt_call(target_pid, fn_gethandle, trap_mem,
                                     (uint64_t)dev32, (uint64_t)types[t], (uint64_t)idx,
                                     0, 0, 0);
                klog_printf("[Ghostpad] dismiss: GH(devId=0x%x,type=%d,idx=%d)->0x%llx\n",
                            dev32, types[t], idx, (unsigned long long)(uint64_t)gh);
                if ((int32_t)gh >= 0) vd_handle = (int32_t)gh;
            }
        }
    }

    /* B: scePadOpen* variants (SceShellUI "Open Pad" may use Open not GetHandle) */
    if (vd_handle < 0) {
        for (int i = 0; i < 4 && vd_handle < 0; i++) {
            intptr_t fn = fn_open_ext2 ? fn_open_ext2
                        : fn_open_ext  ? fn_open_ext
                        : fn_open;
            if (!fn) break;
            int64_t oh = fn_open_ext2
                ? pt_call(target_pid, fn, trap_mem,
                          (uint64_t)(uint32_t)try_uids[i], 3, 0, 0, 0, 0)
                : fn_open_ext
                ? pt_call(target_pid, fn, trap_mem,
                          (uint64_t)(uint32_t)try_uids[i], 3, 0, 0, 0, 0)
                : pt_call(target_pid, fn, trap_mem,
                          (uint64_t)(uint32_t)try_uids[i], 3, 0, 0, 0, 0);
            klog_printf("[Ghostpad] dismiss: Open(0x%x,3)->0x%llx\n",
                        (uint32_t)try_uids[i], (unsigned long long)(uint64_t)oh);
            if ((int32_t)oh >= 0) vd_handle = (int32_t)oh;
        }
    }

    if (vd_handle < 0) {
        klog_printf("[Ghostpad] dismiss: no handle found — cannot send Cross\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }
    klog_printf("[Ghostpad] dismiss: handle=0x%x; sending Cross\n", (uint32_t)vd_handle);

    /* -- Phase 2: send Cross via VDI ----------------------------------------
     * ScePadData layout (byte view, LE):
     *   [0..3] buttons  — Cross = 0x00004000 → byte[1]=0x40
     *   [4]    LS.x=128, [5] LS.y=128, [6] RS.x=128, [7] RS.y=128
     *   [26]   quat.w low byte 0x80, [27] 0x3F  (1.0f LE)
     *   [76]   connected=1 */
    uint8_t press[SHELLUI_PAD_DATA_SIZE];
    uint8_t release_d[SHELLUI_PAD_DATA_SIZE];
    memset(press, 0, SHELLUI_PAD_DATA_SIZE);
    memset(release_d, 0, SHELLUI_PAD_DATA_SIZE);
    press[1] = 0x40;
    press[4] = 128; press[5] = 128; press[6] = 128; press[7] = 128;
    press[26] = 0x80; press[27] = 0x3F;
    press[76] = 1;
    release_d[4] = 128; release_d[5] = 128; release_d[6] = 128; release_d[7] = 128;
    release_d[26] = 0x80; release_d[27] = 0x3F;
    release_d[76] = 1;

    /* Press Cross × 30 frames (~500ms at 60Hz) */
    for (int frame = 0; frame < 30; frame++) {
        int64_t vret = pt_call_with_copy(target_pid, fn_vdi, trap_mem,
                                          (uint64_t)vd_handle, press, SHELLUI_PAD_DATA_SIZE);
        if (frame == 0)
            klog_printf("[Ghostpad] dismiss: VDI press frame0 -> %lld\n", (long long)vret);
        /* pace at ~16ms between frames if usleep available */
        if (fn_usleep)
            pt_call(target_pid, fn_usleep, trap_mem, 16000, 0, 0, 0, 0, 0);
    }
    /* Release × 12 frames */
    for (int frame = 0; frame < 12; frame++) {
        pt_call_with_copy(target_pid, fn_vdi, trap_mem,
                          (uint64_t)vd_handle, release_d, SHELLUI_PAD_DATA_SIZE);
        if (fn_usleep)
            pt_call(target_pid, fn_usleep, trap_mem, 16000, 0, 0, 0, 0, 0);
    }

    /* -- Phase 3: probe GetHandle post-dismiss --------------------------------
     * If assignment succeeded, the device now has userId=0x18c60ea1 and
     * GetHandle(userId, type=3, idx) should return the system padHandle. */
    if (fn_usleep)
        pt_call(target_pid, fn_usleep, trap_mem, 500000, 0, 0, 0, 0, 0);  /* 500ms */

    int32_t post_handle = -1;
    if (fn_gethandle) {
        int ptypes[2] = {0, 3};
        for (int t = 0; t < 2 && post_handle < 0; t++) {
            for (int idx = 0; idx < 8 && post_handle < 0; idx++) {
                int64_t ph = pt_call(target_pid, fn_gethandle, trap_mem,
                                     (uint64_t)(uint32_t)userId, (uint64_t)ptypes[t], (uint64_t)idx,
                                     0, 0, 0);
                klog_printf("[Ghostpad] dismiss: post GH(uid=0x%x,type=%d,%d)->0x%llx\n",
                            (uint32_t)userId, ptypes[t], idx, (unsigned long long)(uint64_t)ph);
                if ((int32_t)ph >= 0) post_handle = (int32_t)ph;
            }
        }
    }
    if (post_handle >= 0)
        klog_printf("[Ghostpad] dismiss: post-assign handle=0x%x — assignment SUCCESS\n",
                    (uint32_t)post_handle);
    else
        klog_printf("[Ghostpad] dismiss: post-dismiss GH still failed — may need more time\n");

    sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
    klog_printf("[Ghostpad] dismiss: done PT_DETACH\n");
    return (post_handle >= 0) ? 0 : 1;  /* 0=confirmed, 1=VDI sent but assignment unconfirmed */
}

/* shellui_pad_force_bind — call sceMbusBindDeviceWithUserId in SceShellUI via pt_call */

/* shellui_pad_test_vdi_cross — send a Cross press to a known padHandle via VDI */
static int
pad_test_vdi_cross_in_process(const char *process_name, const char *tag, int32_t pad_handle)
{
    pid_t pids[4];
    size_t n = find_pids(process_name, pids, 4);
    if (n == 0) {
        klog_printf("[Ghostpad] %s: %s not found\n", tag, process_name);
        return -1;
    }
    pid_t target_pid = pids[0];

    klog_printf("[Ghostpad] %s: pid=%d handle=0x%x\n", tag, target_pid, (uint32_t)pad_handle);
    if (pad_handle <= 0) {
        klog_printf("[Ghostpad] %s: invalid handle\n", tag);
        return -1;
    }

    if (sys_ptrace(PT_ATTACH, target_pid, 0, 0) != 0) {
        klog_printf("[Ghostpad] %s: PT_ATTACH failed errno=%d\n", tag, errno);
        return -1;
    }
    waitpid(target_pid, NULL, 0);

    uint32_t libpad_h = 0;
    get_lib(target_pid, "libScePad", &libpad_h);
    intptr_t fn_vdi = libpad_h ? resolve_sym(target_pid, libpad_h,
                                              "scePadVirtualDeviceInsertData") : 0;
    intptr_t trap_mem = libpad_h ? kernel_dynlib_init_addr(target_pid, libpad_h) : 0;
    if (!trap_mem && libpad_h) trap_mem = kernel_dynlib_fini_addr(target_pid, libpad_h);

    if (!fn_vdi || !trap_mem) {
        klog_printf("[Ghostpad] %s: symbol/cave fail vdi=0x%lx trap=0x%lx\n",
                    tag, fn_vdi, trap_mem);
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }
    kernel_set_vmem_protection(target_pid, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target_pid, trap_mem, &int3, 1);

    uint8_t press[SHELLUI_PAD_DATA_SIZE];
    uint8_t release_d[SHELLUI_PAD_DATA_SIZE];
    memset(press, 0, SHELLUI_PAD_DATA_SIZE);
    memset(release_d, 0, SHELLUI_PAD_DATA_SIZE);
    press[1] = 0x40;  /* Cross */
    press[4] = 128; press[5] = 128; press[6] = 128; press[7] = 128;
    press[26] = 0x80; press[27] = 0x3F;
    press[76] = 1;
    release_d[4] = 128; release_d[5] = 128; release_d[6] = 128; release_d[7] = 128;
    release_d[26] = 0x80; release_d[27] = 0x3F;
    release_d[76] = 1;

    int result = 0;
    for (int frame = 0; frame < 15; frame++) {
        int64_t r = pt_call_with_copy(target_pid, fn_vdi, trap_mem,
                                       (uint64_t)pad_handle, press, SHELLUI_PAD_DATA_SIZE);
        if (frame == 0) {
            klog_printf("[Ghostpad] %s: VDI press frame0 -> %lld\n", tag, (long long)r);
            result = (int)r;
        }
    }
    for (int frame = 0; frame < 8; frame++) {
        pt_call_with_copy(target_pid, fn_vdi, trap_mem,
                          (uint64_t)pad_handle, release_d, SHELLUI_PAD_DATA_SIZE);
    }

    sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
    klog_printf("[Ghostpad] %s: done, first VDI ret=%d\n", tag, result);
    return result;
}

int
shellui_pad_test_vdi_cross(int32_t pad_handle)
{
    return pad_test_vdi_cross_in_process("SceShellUI", "vdi_cross_ui", pad_handle);
}

int
shellcore_pad_test_vdi_cross(int32_t pad_handle)
{
    return pad_test_vdi_cross_in_process("SceShellCore", "vdi_cross_core", pad_handle);
}

/* VDI probe with NEUTRAL state (buttons=0) — confirms VDI works without
 * sending any input to the UI. Used by GBND handler instead of Cross so
 * the UI is not disturbed when no assignment screen is visible.
 * Cross is only pressed by the stub when assignment_hint detects the screen. */
int
shellcore_pad_test_vdi_neutral(int32_t pad_handle)
{
    pid_t target_pid = -1;
    {
        pid_t pids[8]; size_t n;
        n = find_pids("SceShellCore", pids, 8);
        if (n > 0) target_pid = pids[0];
    }
    if (target_pid < 0) return -1;
    if (sys_ptrace(PT_ATTACH, target_pid, 0, 0)) return -1;
    waitpid(target_pid, NULL, 0);

    uint32_t libpad_h = 0;
    get_lib(target_pid, "libScePad", &libpad_h);
    intptr_t fn_vdi  = libpad_h ? resolve_sym(target_pid, libpad_h, "scePadVirtualDeviceInsertData") : 0;
    intptr_t trap_mem = libpad_h ? kernel_dynlib_init_addr(target_pid, libpad_h) : 0;
    if (!trap_mem && libpad_h) trap_mem = kernel_dynlib_fini_addr(target_pid, libpad_h);
    if (!fn_vdi || !trap_mem) { sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0); return -1; }

    kernel_set_vmem_protection(target_pid, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target_pid, trap_mem, &int3, 1);

    uint8_t neutral[SHELLUI_PAD_DATA_SIZE];
    memset(neutral, 0, SHELLUI_PAD_DATA_SIZE);
    neutral[4] = 128; neutral[5] = 128; neutral[6] = 128; neutral[7] = 128;
    neutral[26] = 0x80; neutral[27] = 0x3F;
    neutral[76] = 1;

    int64_t r = pt_call_with_copy(target_pid, fn_vdi, trap_mem,
                                   (uint64_t)pad_handle, neutral, SHELLUI_PAD_DATA_SIZE);
    klog_printf("[Ghostpad] vdi_neutral: handle=0x%x ret=%lld\n", (uint32_t)pad_handle, (long long)r);
    sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
    return (int)r;
}

/* shellui_pad_probe_legacy_disabled — legacy probe, disabled; returns -1 immediately */
/*
 * =====================================================================================
 *     MANIFEST-VERIFIED PS4 VDA PATCH FOR scePadVirtualDeviceAddDevice
 * =====================================================================================
 *
 * This patcher intentionally does not scan live code for generic patterns.
 * It applies only to the exact PS4 libScePad fingerprint reported by
 * vda_probe_report.txt:
 *
 *   scePadVirtualDeviceAddDevice rel  +0x5b40
 *   hash256                          0xbb22d8acd843d81e
 *   hash4k                           0x346f2b8071895f89
 *   patch call                       +0x0c0
 *   code cave                        +0x0dd2, 14-byte NOP run
 *
 * Patch shape:
 *   original call @ +0xc0 -> verified cave @ +0xdd2
 *   cave: call original dispatcher; xor eax,eax; ret
 *
 * Returning with RET uses the original call-site return address and resumes at
 * +0xc5, so the original canary check and epilogue remain intact.  Early
 * validation returns are left untouched.
 */

static int
shellui_pad_patch_vda_target(pid_t target, const char *target_name, int dump_only)
{
#if !GHOSTPAD_ENABLE_KNOWN_VDA_PATCH
    (void)dump_only;
    (void)target;
    (void)target_name;
    klog_printf("[Ghostpad] patch_vda: known VDA patcher disabled; compile with -DGHOSTPAD_ENABLE_KNOWN_VDA_PATCH=1\n");
    return 0;
#elif !defined(__ORBIS__)
    (void)dump_only;
    (void)target;
    (void)target_name;
    klog_printf("[Ghostpad] patch_vda: no verified manifest for this platform yet\n");
    return 0;
#else
    static const uint8_t expected_prologue32[32] = {
        0x55,0x48,0x89,0xe5,0x53,0x48,0x83,0xe4,
        0xe0,0x48,0x81,0xec,0x80,0x00,0x00,0x00,
        0x48,0x8b,0x1d,0xd1,0x64,0x00,0x00,0x48,
        0x8b,0x03,0x48,0x89,0x44,0x24,0x60,0xb8
    };
    static const uint8_t expected_call[5] = {
        0xe8,0x33,0xa5,0xff,0xff
    };
    static const uint8_t expected_after_call[17] = {
        0x48,0x8b,0x0b,0x48,0x3b,0x4c,0x24,0x60,
        0x75,0x07,0x48,0x8d,0x65,0xf8,0x5b,0x5d,0xc3
    };

    pid_t mypid = getpid();
    uint8_t privcaps[16] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };
    uint8_t saved_caps[16];
    uint64_t saved_authid = kernel_get_ucred_authid(mypid);
    int have_saved_caps = 0;

    if (saved_authid && kernel_get_ucred_caps(mypid, saved_caps) == 0) {
        have_saved_caps = 1;
        kernel_set_ucred_authid(mypid, 0x4800000000010003l);
        kernel_set_ucred_caps(mypid, privcaps);
    }

    if (!target_name) target_name = "target";
    klog_printf("[Ghostpad] patch_vda: %s pid=%d dump_only=%d manifest=PS4-libScePad-vda-0xbb22d8acd843d81e\n", target_name,
                target, dump_only);

    uint32_t libpad_h = 0;
    if (get_lib(target, "libScePad", &libpad_h)) {
        klog_printf("[Ghostpad] patch_vda: libScePad not found\n");
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return -1;
    }

    intptr_t libpad_init = kernel_dynlib_init_addr(target, libpad_h);
    intptr_t fn_vda = resolve_sym(target, libpad_h, "scePadVirtualDeviceAddDevice");
    if (!fn_vda) {
        klog_printf("[Ghostpad] patch_vda: scePadVirtualDeviceAddDevice not found\n");
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return -1;
    }

    klog_printf("[Ghostpad] patch_vda: libScePad init=0x%lx fn_vda=0x%lx rel=0x%lx\n",
                (unsigned long)libpad_init,
                (unsigned long)fn_vda,
                libpad_init ? (unsigned long)(fn_vda - libpad_init) : 0ul);

    if (libpad_init && (uint32_t)(fn_vda - libpad_init) != GHOSTPAD_VDA_PS4_LIBSCEPAD_VDA_OFF) {
        klog_printf("[Ghostpad] patch_vda: manifest reject: VDA offset 0x%lx != 0x%x\n",
                    (unsigned long)(fn_vda - libpad_init),
                    (unsigned)GHOSTPAD_VDA_PS4_LIBSCEPAD_VDA_OFF);
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return 0;
    }

    static uint8_t buf[4096];
    memset(buf, 0, sizeof(buf));
    if (mdbg_copyout(target, fn_vda, buf, sizeof(buf)) != 0) {
        klog_printf("[Ghostpad] patch_vda: mdbg_copyout failed len=%zu errno=%d\n", sizeof(buf), errno);
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return -1;
    }

    uint64_t hash256 = ghostpad_fnv1a64(buf, 256);
    uint64_t hash4k  = ghostpad_fnv1a64(buf, sizeof(buf));

    int already_patched = 0;
    int32_t patched_call_rel = (int32_t)((intptr_t)GHOSTPAD_VDA_PS4_CAVE_OFF -
                                         (intptr_t)GHOSTPAD_VDA_PS4_AFTER_CALL_OFF);
    if (buf[GHOSTPAD_VDA_PS4_CALL_OFF] == 0xe8) {
        int32_t cur_rel = (int32_t)((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 1] |
                                    ((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 2] << 8) |
                                    ((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 3] << 16) |
                                    ((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 4] << 24));
        already_patched = (cur_rel == patched_call_rel &&
                           buf[GHOSTPAD_VDA_PS4_CAVE_OFF + 0] == 0xe8 &&
                           buf[GHOSTPAD_VDA_PS4_CAVE_OFF + 5] == 0x31 &&
                           buf[GHOSTPAD_VDA_PS4_CAVE_OFF + 6] == 0xc0 &&
                           buf[GHOSTPAD_VDA_PS4_CAVE_OFF + 7] == 0xc3);
    }

    if (!already_patched) {
        if (memcmp(buf, expected_prologue32, sizeof(expected_prologue32)) != 0) {
            klog_printf("[Ghostpad] patch_vda: manifest reject: prologue32 mismatch\n");
            if (have_saved_caps) {
                kernel_set_ucred_authid(mypid, saved_authid);
                kernel_set_ucred_caps(mypid, saved_caps);
            }
            return 0;
        }
        if (hash256 != GHOSTPAD_VDA_PS4_HASH256 || hash4k != GHOSTPAD_VDA_PS4_HASH4K) {
            klog_printf("[Ghostpad] patch_vda: manifest reject: hash256=0x%016llx hash4k=0x%016llx\n",
                        (unsigned long long)hash256,
                        (unsigned long long)hash4k);
            if (have_saved_caps) {
                kernel_set_ucred_authid(mypid, saved_authid);
                kernel_set_ucred_caps(mypid, saved_caps);
            }
            return 0;
        }

        if (memcmp(buf + GHOSTPAD_VDA_PS4_CALL_OFF, expected_call, sizeof(expected_call)) != 0 ||
            memcmp(buf + GHOSTPAD_VDA_PS4_AFTER_CALL_OFF, expected_after_call, sizeof(expected_after_call)) != 0 ||
            buf[GHOSTPAD_VDA_PS4_BRANCH_OFF] != 0x75) {
            klog_printf("[Ghostpad] patch_vda: manifest reject: call/canary bytes mismatch\n");
            if (have_saved_caps) {
                kernel_set_ucred_authid(mypid, saved_authid);
                kernel_set_ucred_caps(mypid, saved_caps);
            }
            return 0;
        }

        if (!ghostpad_all_byte(buf + GHOSTPAD_VDA_PS4_CAVE_OFF,
                               GHOSTPAD_VDA_PS4_CAVE_LEN, 0x90)) {
            klog_printf("[Ghostpad] patch_vda: manifest reject: cave +0x%x is not the expected NOP run\n",
                        (unsigned)GHOSTPAD_VDA_PS4_CAVE_OFF);
            if (have_saved_caps) {
                kernel_set_ucred_authid(mypid, saved_authid);
                kernel_set_ucred_caps(mypid, saved_caps);
            }
            return 0;
        }
    }

    klog_printf("[Ghostpad] patch_vda: manifest match: hash256=0x%016llx hash4k=0x%016llx call=+0x%x cave=+0x%x%s\n",
                (unsigned long long)hash256,
                (unsigned long long)hash4k,
                (unsigned)GHOSTPAD_VDA_PS4_CALL_OFF,
                (unsigned)GHOSTPAD_VDA_PS4_CAVE_OFF,
                already_patched ? " already_patched=1" : "");

    if (dump_only || already_patched) {
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return 1;
    }

    intptr_t call_addr = fn_vda + (intptr_t)GHOSTPAD_VDA_PS4_CALL_OFF;
    intptr_t cave_addr = fn_vda + (intptr_t)GHOSTPAD_VDA_PS4_CAVE_OFF;
    intptr_t page_call = call_addr & ~(intptr_t)0xfff;
    intptr_t page_cave = cave_addr & ~(intptr_t)0xfff;

    int protect_call_ok = 0;
    int protect_cave_ok = 0;

    /* On some PS4/HEN combinations kernel_set_vmem_protection() rejects
     * SceShellCore text pages even though mdbg_copyin() can still perform a
     * privileged debug write.  Do not abort on protection failure: log it, try
     * a narrow unaligned range as a fallback, then attempt mdbg_copyin and
     * verify by reading the bytes back.  The pages are already executable; we
     * only need a reliable write primitive. */
    if (kernel_set_vmem_protection(target, page_call, 0x1000,
                                   PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        protect_call_ok = 1;
    } else if (kernel_set_vmem_protection(target, call_addr, 5,
                                          PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        protect_call_ok = 1;
        klog_printf("[Ghostpad] patch_vda: RWX call page failed, narrow call range accepted addr=0x%lx\n",
                    (unsigned long)call_addr);
    } else {
        klog_printf("[Ghostpad] patch_vda: RWX call page/range failed page=0x%lx addr=0x%lx; trying mdbg_copyin anyway\n",
                    (unsigned long)page_call, (unsigned long)call_addr);
    }

    if (page_cave == page_call) {
        protect_cave_ok = protect_call_ok;
    } else if (kernel_set_vmem_protection(target, page_cave, 0x1000,
                                          PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        protect_cave_ok = 1;
    } else if (kernel_set_vmem_protection(target, cave_addr, GHOSTPAD_VDA_PS4_CAVE_LEN,
                                          PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        protect_cave_ok = 1;
        klog_printf("[Ghostpad] patch_vda: RWX cave page failed, narrow cave range accepted addr=0x%lx\n",
                    (unsigned long)cave_addr);
    } else {
        klog_printf("[Ghostpad] patch_vda: RWX cave page/range failed page=0x%lx addr=0x%lx; trying mdbg_copyin anyway\n",
                    (unsigned long)page_cave, (unsigned long)cave_addr);
    }

    uint8_t cave_patch[GHOSTPAD_VDA_PS4_CAVE_LEN];
    memset(cave_patch, 0x90, sizeof(cave_patch));

    int32_t orig_rel32 = (int32_t)((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 1] |
                                   ((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 2] << 8) |
                                   ((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 3] << 16) |
                                   ((uint32_t)buf[GHOSTPAD_VDA_PS4_CALL_OFF + 4] << 24));
    intptr_t orig_target_off = (intptr_t)GHOSTPAD_VDA_PS4_AFTER_CALL_OFF + (intptr_t)orig_rel32;
    int32_t cave_call_rel32 = (int32_t)(orig_target_off -
                                        ((intptr_t)GHOSTPAD_VDA_PS4_CAVE_OFF + 5));

    cave_patch[0] = 0xe8;
    cave_patch[1] = (uint8_t)(cave_call_rel32 & 0xff);
    cave_patch[2] = (uint8_t)((cave_call_rel32 >> 8) & 0xff);
    cave_patch[3] = (uint8_t)((cave_call_rel32 >> 16) & 0xff);
    cave_patch[4] = (uint8_t)((cave_call_rel32 >> 24) & 0xff);
    cave_patch[5] = 0x31;
    cave_patch[6] = 0xc0;
    cave_patch[7] = 0xc3;

    if (target == getpid()) {
        memcpy(g_orig_self_vda_call, buf + GHOSTPAD_VDA_PS4_CALL_OFF, 5);
        memcpy(g_orig_self_vda_cave, buf + GHOSTPAD_VDA_PS4_CAVE_OFF, 8);
        g_self_vda_patched = 1;
        klog_printf("[Ghostpad] patch_vda: saved original VDA self patch bytes\n");
    } else {
        memcpy(g_orig_vda_call, buf + GHOSTPAD_VDA_PS4_CALL_OFF, 5);
        memcpy(g_orig_vda_cave, buf + GHOSTPAD_VDA_PS4_CAVE_OFF, 8);
        g_vda_patched = 1;
        g_vda_patched_pid = target;
        klog_printf("[Ghostpad] patch_vda: saved original VDA target patch bytes for pid=%d\n", target);
    }

    uint8_t call_patch[5];
    call_patch[0] = 0xe8;
    call_patch[1] = (uint8_t)(patched_call_rel & 0xff);
    call_patch[2] = (uint8_t)((patched_call_rel >> 8) & 0xff);
    call_patch[3] = (uint8_t)((patched_call_rel >> 16) & 0xff);
    call_patch[4] = (uint8_t)((patched_call_rel >> 24) & 0xff);

    if (mdbg_copyin(target, cave_patch, cave_addr, sizeof(cave_patch)) != 0) {


        klog_printf("[Ghostpad] patch_vda: cave write failed errno=%d\n", errno);
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return -1;
    }

    if (mdbg_copyin(target, call_patch, call_addr, sizeof(call_patch)) != 0) {
        uint8_t nop_restore[GHOSTPAD_VDA_PS4_CAVE_LEN];
        memset(nop_restore, 0x90, sizeof(nop_restore));
        (void)mdbg_copyin(target, nop_restore, cave_addr, sizeof(nop_restore));
        klog_printf("[Ghostpad] patch_vda: call-site write failed errno=%d; cave restored\n", errno);
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return -1;
    }

    uint8_t verify_cave[GHOSTPAD_VDA_PS4_CAVE_LEN];
    uint8_t verify_call[sizeof(call_patch)];
    memset(verify_cave, 0, sizeof(verify_cave));
    memset(verify_call, 0, sizeof(verify_call));
    if (mdbg_copyout(target, cave_addr, verify_cave, sizeof(verify_cave)) != 0 ||
        mdbg_copyout(target, call_addr, verify_call, sizeof(verify_call)) != 0 ||
        memcmp(verify_cave, cave_patch, sizeof(cave_patch)) != 0 ||
        memcmp(verify_call, call_patch, sizeof(call_patch)) != 0) {
        klog_printf("[Ghostpad] patch_vda: write verification failed; patch not trusted\n");
        if (have_saved_caps) {
            kernel_set_ucred_authid(mypid, saved_authid);
            kernel_set_ucred_caps(mypid, saved_caps);
        }
        return -1;
    }

    if (protect_call_ok) {
        (void)kernel_set_vmem_protection(target, page_call, 0x1000, PROT_READ | PROT_EXEC);
    }
    if (protect_cave_ok && page_cave != page_call) {
        (void)kernel_set_vmem_protection(target, page_cave, 0x1000, PROT_READ | PROT_EXEC);
    }

    klog_printf("[Ghostpad] patch_vda: PATCHED %s libScePad VDA call +0x%x -> cave +0x%x; dispatcher rel=0x%08x protect_call=%d protect_cave=%d\n",
                target_name,
                (unsigned)GHOSTPAD_VDA_PS4_CALL_OFF,
                (unsigned)GHOSTPAD_VDA_PS4_CAVE_OFF,
                (uint32_t)cave_call_rel32, protect_call_ok, protect_cave_ok);

    if (have_saved_caps) {
        kernel_set_ucred_authid(mypid, saved_authid);
        kernel_set_ucred_caps(mypid, saved_caps);
    }
    return 1;
#endif
}

int
shellui_pad_patch_vda_self(int dump_only)
{
    return shellui_pad_patch_vda_target(getpid(), "self", dump_only);
}

int
shellui_pad_patch_vda(int dump_only)
{
#if !GHOSTPAD_ENABLE_KNOWN_VDA_PATCH || !defined(__ORBIS__)
    return shellui_pad_patch_vda_target(0, "SceShellCore", dump_only);
#else
    pid_t pids[8];
    if (find_pids("SceShellCore", pids, 8) == 0) {
        klog_printf("[Ghostpad] patch_vda: SceShellCore not found\n");
        return -1;
    }
    return shellui_pad_patch_vda_target(pids[0], "SceShellCore", dump_only);
#endif
}

int32_t
shellui_pad_retry_vda_shellcore(int32_t userId)
{
    pid_t pids[8];
    if (find_pids("SceShellCore", pids, 8) == 0) {
        klog_printf("[Ghostpad] retry_vda: SceShellCore not found\n");
        return -1;
    }
    pid_t target = pids[0];

    klog_printf("[Ghostpad] retry_vda: PT_ATTACH(SceShellCore pid=%d)\n", target);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[Ghostpad] retry_vda: PT_ATTACH failed errno=%d\n", errno);
        return (int32_t)-errno;
    }
    waitpid(target, NULL, 0);

    uint32_t libpad_h = 0, libkernel_h = 0;
    get_lib(target, "libScePad",     &libpad_h);
    get_lib(target, "libkernel_sys", &libkernel_h);
    intptr_t fn_vda    = libpad_h ? resolve_sym(target, libpad_h, "scePadVirtualDeviceAddDevice") : 0;
    intptr_t trap_mem  = libpad_h ? kernel_dynlib_init_addr(target, libpad_h) : 0;
    if (!trap_mem && libpad_h) trap_mem = kernel_dynlib_fini_addr(target, libpad_h);

    if (!fn_vda || !trap_mem) {
        sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
        return -1;
    }
    kernel_set_vmem_protection(target, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target, trap_mem, &int3, 1);

    struct { int32_t f[8]; } vdp = {0};
    vdp.f[0] = 32;
    vdp.f[1] = userId;
    /* Write vdp struct onto the stack of the stopped process via PT_IO */
    struct reg regs;
    sys_ptrace(PT_GETREGS, target, (caddr_t)&regs, 0);
    intptr_t vdp_addr = (regs.r_rsp - 128 - (intptr_t)sizeof(vdp)) & ~(intptr_t)0xf;
    pt_io_write(target, vdp_addr, &vdp, sizeof(vdp));

    /* pt_call VDA(vdp_addr, 3) via fn_vda */
    int64_t vda_ret = pt_call(target, fn_vda, trap_mem,
                               (uint64_t)vdp_addr, 3, 0, 0, 0, 0);
    klog_printf("[Ghostpad] retry_vda: VDA(uid=0x%x, type=3) -> 0x%llx\n",
                (uint32_t)userId, (unsigned long long)(uint64_t)vda_ret);

    /* Read back vdp struct to see if VDA wrote a handle into f[2..7] */
    struct { int32_t f[8]; } vdp_out = {0};
    {
        struct ptrace_io_desc iod;
        iod.piod_op   = PIOD_READ_D;
        iod.piod_offs = (void *)vdp_addr;
        iod.piod_addr = &vdp_out;
        iod.piod_len  = sizeof(vdp_out);
        sys_ptrace(PT_IO, target, (caddr_t)&iod, 0);
    }
    klog_printf("[Ghostpad] retry_vda: vdp_out f[0..3]=0x%x 0x%x 0x%x 0x%x\n",
                (uint32_t)vdp_out.f[0], (uint32_t)vdp_out.f[1],
                (uint32_t)vdp_out.f[2], (uint32_t)vdp_out.f[3]);

    sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
    return (int32_t)vda_ret;
}

int32_t
shellui_pad_probe_legacy_disabled(int32_t userId)
{
    pid_t pids[8];
    (void)userId;
    klog_printf("[Ghostpad] legacy probe disabled; using SceShellCore/SceShellUI path\n");
    return -1;
    size_t n = find_pids("LegacyDisabled", pids, 8);
    if (n == 0) { klog_printf("[Ghostpad] probe_legacy: target not found\n"); return -1; }
    pid_t target = pids[0];

    klog_printf("[Ghostpad] probe_legacy: PT_ATTACH(pid=%d)\n", target);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[Ghostpad] probe_rp: PT_ATTACH failed errno=%d\n", errno);
        return -1;
    }
    waitpid(target, NULL, 0);

    uint32_t libpad_h = 0, libkernel_h = 0;
    get_lib(target, "libScePad",     &libpad_h);
    get_lib(target, "libkernel_sys", &libkernel_h);
    intptr_t fn_gethandle = libpad_h ? resolve_sym(target, libpad_h, "scePadGetHandle") : 0;
    intptr_t fn_open      = libpad_h ? resolve_sym(target, libpad_h, "scePadOpen")      : 0;
    intptr_t fn_open_ext  = libpad_h ? resolve_sym(target, libpad_h, "scePadOpenExt")   : 0;
    intptr_t trap_mem     = libpad_h ? kernel_dynlib_init_addr(target, libpad_h)        : 0;
    if (!trap_mem && libpad_h) trap_mem = kernel_dynlib_fini_addr(target, libpad_h);

    if (!trap_mem) {
        sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
        return -1;
    }
    kernel_set_vmem_protection(target, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target, trap_mem, &int3, 1);

    int32_t found = -1;
    int32_t uids[4] = {userId, 0x10000000, 1, (int32_t)0xffffffff};
    int types[3] = {0, 3, 16};

    /* GetHandle sweeps across all userId × type × idx */
    if (fn_gethandle) {
        for (int u = 0; u < 4 && found < 0; u++) {
            for (int t = 0; t < 3 && found < 0; t++) {
                for (int idx = 0; idx < 8 && found < 0; idx++) {
                    int64_t r = pt_call(target, fn_gethandle, trap_mem,
                                        (uint64_t)(uint32_t)uids[u],
                                        (uint64_t)types[t], (uint64_t)idx, 0, 0, 0);
                    klog_printf("[Ghostpad] probe_rp: GH(0x%x,t=%d,i=%d)->0x%llx\n",
                                (uint32_t)uids[u], types[t], idx,
                                (unsigned long long)(uint64_t)r);
                    if ((int32_t)r >= 0) found = (int32_t)r;
                }
            }
        }
    }

    /* scePadOpen sweeps if GetHandle didn't find it */
    if (found < 0 && (fn_open || fn_open_ext)) {
        for (int u = 0; u < 4 && found < 0; u++) {
            for (int t = 0; t < 2 && found < 0; t++) {
                for (int idx = 0; idx < 4 && found < 0; idx++) {
                    intptr_t fn = fn_open_ext ? fn_open_ext : fn_open;
                    int64_t r = fn_open_ext
                        ? pt_call(target, fn, trap_mem,
                                  (uint64_t)(uint32_t)uids[u], (uint64_t)types[t],
                                  (uint64_t)idx, 0, 0, 0)
                        : pt_call(target, fn, trap_mem,
                                  (uint64_t)(uint32_t)uids[u], (uint64_t)types[t],
                                  (uint64_t)idx, 0, 0, 0);
                    klog_printf("[Ghostpad] probe_rp: Open(0x%x,t=%d,i=%d)->0x%llx\n",
                                (uint32_t)uids[u], types[t], idx,
                                (unsigned long long)(uint64_t)r);
                    if ((int32_t)r >= 0) found = (int32_t)r;
                }
            }
        }
    }

    sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
    klog_printf("[Ghostpad] probe_rp: result=%d\n", found);
    return found;
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
    uint8_t pad_data[SHELLUI_PAD_DATA_SIZE];
} RemotePadReaderArgs;

extern void remote_pad_reader_stub(void *arg);
extern void remote_pad_reader_stub_end(void);

/*
 * Position-independent target thread. Do not call local symbols or touch
 * globals: every target-side call is made through a resolved function pointer
 * stored in RemotePadReaderArgs.
 */
__attribute__((noinline, used, section(".text.ds4reader")))
void
remote_pad_reader_stub(void *arg)
{
    RemotePadReaderArgs *a = (RemotePadReaderArgs *)arg;
    typedef int32_t (*read_fn_t)(int32_t, void *);
    typedef void (*usleep_fn_t)(unsigned int);
    read_fn_t readstate = (read_fn_t)(uintptr_t)a->fp_readstate;
    usleep_fn_t sleep_us = (usleep_fn_t)(uintptr_t)a->fp_usleep;

    a->ready = 1;
    while (!a->stop) {
        int32_t result = readstate(a->pad_handle, a->pad_data);
        a->last_result = result;
        if (result == 0) {
            __asm__ volatile("" ::: "memory");
            a->seq++;
        }
        sleep_us(a->interval_us);
    }
    a->ready = 2;
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
extern void game_pad_bridge_receiver_stub(void *arg);
extern void game_pad_bridge_stub_end(void);

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_state_stub(int32_t handle, void *out,
                         GamePadBridgeArgs *args)
{
    typedef int32_t (*state_internal_fn)(int32_t, void *, int32_t);
    if (args && args->magic == DS4TOD5_GAME_BRIDGE_MAGIC &&
        args->active && handle == args->pad_handle && out &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before = args->seq;
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            __asm__ volatile("" ::: "memory");
            if (before == args->seq) {
                args->read_state_calls++;
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
        args->active && handle == args->pad_handle && out &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before = args->seq;
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            __asm__ volatile("" ::: "memory");
            if (before == args->seq) {
                args->read_state_ext_calls++;
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
        args->active && handle == args->pad_handle && out && num > 0 &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before = args->seq;
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            __asm__ volatile("" ::: "memory");
            if (before == args->seq) {
                args->read_calls++;
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
        args->active && handle == args->pad_handle && out && num > 0 &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before = args->seq;
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            __asm__ volatile("" ::: "memory");
            if (before == args->seq) {
                args->read_ext_calls++;
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
        args->active && handle == args->pad_handle && out &&
        args->pad_size == DS4TOD5_GAME_BRIDGE_PAD_SIZE) {
        volatile uint8_t *destination = (volatile uint8_t *)out;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            uint32_t before = args->seq;
            if (before & 1u)
                continue;
            for (unsigned byte = 0;
                 byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
                 ++byte)
                destination[byte] = args->pad_data[byte];
            __asm__ volatile("" ::: "memory");
            if (before == args->seq) {
                args->data_internal_calls++;
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
    if (args)
        args->controller_info_calls++;
    if (result == 0 && args &&
        args->magic == DS4TOD5_GAME_BRIDGE_MAGIC && args->active &&
        handle == args->pad_handle && out &&
        args->pad_data[offsetof(ScePadData, connected)] != 0) {
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
        args->controller_info_spoofs++;
    }
    return result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
void
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
    args->receiver_last_result = fd;
    if (fd < 0) {
        args->receiver_ready = -1;
        return;
    }
    int32_t bind_result = bind_call(fd, &loopback, sizeof(loopback));
    args->receiver_last_result = bind_result;
    if (bind_result != 0) {
        args->receiver_ready = -2;
        close_call(fd);
        return;
    }
    args->receiver_ready = 1;
    while (!args->receiver_stop) {
        int64_t received = recvfrom_call(
            fd, packet, sizeof(packet), 0, (void *)0, (void *)0);
        args->receiver_last_result = (int32_t)received;
        if (received == (int64_t)sizeof(uint32_t)) {
            uint32_t magic = 0;
            for (unsigned byte = 0; byte < sizeof(magic); ++byte)
                ((uint8_t *)(void *)&magic)[byte] = packet[byte];
            if (magic == DS4TOD5_GAME_BRIDGE_STOP_MAGIC)
                break;
        }
        if (received != (int64_t)DS4TOD5_GAME_BRIDGE_PAD_SIZE)
            continue;
        uint32_t odd = (args->seq + 1u) | 1u;
        args->seq = odd;
        __asm__ volatile("" ::: "memory");
        for (unsigned byte = 0;
             byte < DS4TOD5_GAME_BRIDGE_PAD_SIZE;
             ++byte)
            args->pad_data[byte] = packet[byte];
        __asm__ volatile("" ::: "memory");
        args->seq = odd + 1u;
        args->active = 1;
        args->receiver_packets++;
    }
    args->active = 0;
    args->receiver_ready = 2;
    close_call(fd);
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
    if (saved_authid)
        kernel_set_ucred_authid(self, 0x4800000000010003l);
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
    if (saved_authid)
        kernel_set_ucred_authid(self, 0x4800000000010003l);
    int result = mdbg_copyin(pid, buf, addr, len);
    if (saved_authid)
        kernel_set_ucred_authid(self, saved_authid);
    return result;
#endif
}

int
shellui_pad_remote_reset(void)
{
#if !defined(__PROSPERO__)
    return -1;
#else
    pid_t pids[8];
    size_t count = find_pids("SceRemotePlay", pids, 8);
    if (count == 0) {
        klog_printf("[DS4toDS5] remote reset: SceRemotePlay not found\n");
        return -1;
    }

    pid_t old_pid = pids[0];
    klog_printf("[DS4toDS5] remote reset: SIGKILL SceRemotePlay pid=%d\n",
                old_pid);
    if (kill(old_pid, SIGKILL) != 0) {
        klog_printf("[DS4toDS5] remote reset: kill failed errno=%d\n", errno);
        return -1;
    }

    for (unsigned attempt = 0; attempt < 30; attempt++) {
        usleep(200000);
        count = find_pids("SceRemotePlay", pids, 8);
        if (count > 0 && pids[0] != old_pid) {
            klog_printf("[DS4toDS5] remote reset: restarted pid=%d\n",
                        pids[0]);
            usleep(500000);
            return 0;
        }
    }

    klog_printf("[DS4toDS5] remote reset: restart timeout\n");
    return -1;
#endif
}

int
shellui_pad_remote_reader_start(int32_t userId, pid_t *out_pid,
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
    waitpid(target, NULL, 0);

    uint32_t libpad_h = 0, libkernel_h = 0, libpthread_h = 0;
    uint32_t liblibc_h = 0;
    get_lib(target, "libScePad", &libpad_h);
    get_lib(target, "libkernel_sys", &libkernel_h);
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
    if (!fn_pthread_create && libpthread_h)
        fn_pthread_create =
            resolve_sym(target, libpthread_h, "pthread_create");

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
                                         PROT_READ | PROT_EXEC);
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
    (void)shellui_pad_remote_reader_stop(target, args_addr);
    return -1;
#endif
}

int
shellui_pad_remote_reader_read(pid_t pid, intptr_t args_kaddr,
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
    if (pad_data_len > SHELLUI_PAD_DATA_SIZE)
        pad_data_len = SHELLUI_PAD_DATA_SIZE;

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
        if (snapshot.seq == seq_after && snapshot.ready == 1 &&
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
shellui_pad_remote_reader_stop(pid_t pid, intptr_t args_kaddr)
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
        waitpid(pid, NULL, 0);
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

/*
 * Firmware-portability guard.  The exact 11.60 manifest remains the primary
 * path.  On another firmware we may only proceed when the exported wrappers
 * retain the small, relocation-free shapes that this bridge understands and
 * their relative-call targets remain inside the same libScePad image.  This
 * is deliberately structural rather than a blind "unknown firmware" bypass:
 * a changed ABI fails closed before any target bytes are written.
 */
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
game_bridge_generic_wrapper_variant(const uint8_t *code, unsigned kind,
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
game_bridge_generic_target_ok(intptr_t base, intptr_t target)
{
    if (base <= 0 || target < base)
        return 0;
    uintptr_t delta = (uintptr_t)(target - base);
    /* Keep a malformed rel32 from escaping into another mapping. */
    return delta < (uintptr_t)0x01000000u;
}

static int
game_bridge_generic_controller_info(const uint8_t *code)
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

typedef struct {
    intptr_t image_start;
    intptr_t image_end;
    intptr_t text_start;
    intptr_t text_end;
    uint64_t image_size;
} GameBridgeModuleBounds;

/* Mapbase is the ELF image base for normal PS5 dynlibs.  Derive the actual
 * PT_LOAD/PF_X bounds instead of treating an arbitrary +16 MiB window as code;
 * this keeps the generic firmware path fail-closed while allowing ASLR and
 * firmware-specific image sizes. */
static int
game_bridge_module_bounds(pid_t target, intptr_t base,
                           GameBridgeModuleBounds *out)
{
#if !defined(__PROSPERO__)
    (void)target; (void)base; (void)out;
    return -1;
#else
    if (!out || base <= 0)
        return -1;
    uint8_t header[0x1000];
    memset(header, 0, sizeof(header));
    if (mdbg_copyout(target, base, header, sizeof(header)) != 0)
        return -1;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)header;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > 64 ||
        ehdr->e_phoff > sizeof(header) ||
        ehdr->e_phnum >
            (sizeof(header) - (size_t)ehdr->e_phoff) /
            sizeof(Elf64_Phdr))
        return -1;

    const Elf64_Phdr *phdrs =
        (const Elf64_Phdr *)(header + (size_t)ehdr->e_phoff);
    uint64_t image_end = 0;
    uint64_t text_start = UINT64_MAX;
    uint64_t text_end = 0;
    for (uint16_t index = 0; index < ehdr->e_phnum; ++index) {
        const Elf64_Phdr *phdr = &phdrs[index];
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0 ||
            phdr->p_vaddr > UINT64_MAX - phdr->p_memsz)
            continue;
        uint64_t end = phdr->p_vaddr + phdr->p_memsz;
        if (end > image_end)
            image_end = end;
        if ((phdr->p_flags & PF_X) != 0) {
            if (phdr->p_vaddr < text_start)
                text_start = phdr->p_vaddr;
            if (end > text_end)
                text_end = end;
        }
    }
    if (image_end == 0 || image_end > UINT64_C(0x04000000) ||
        text_start == UINT64_MAX || text_end <= text_start ||
        (uint64_t)base > (uint64_t)INTPTR_MAX - image_end)
        return -1;
    out->image_start = base;
    out->image_end = base + (intptr_t)image_end;
    out->text_start = base + (intptr_t)text_start;
    out->text_end = base + (intptr_t)text_end;
    out->image_size = image_end;
    return 0;
#endif
}

static int
game_bridge_address_in_text(const GameBridgeModuleBounds *bounds,
                            intptr_t address, size_t length)
{
    if (!bounds || address < bounds->text_start ||
        address > bounds->text_end ||
        length > (size_t)(bounds->text_end - address))
        return 0;
    return 1;
}

int
shellui_pad_game_bridge_install(int32_t user_id, pid_t *out_game_pid,
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
    int generic_manifest = 0;
    GameBridgeModuleBounds module_bounds;
    memset(&module_bounds, 0, sizeof(module_bounds));

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
    int generic_wrapper_bytes =
        game_bridge_generic_wrapper_variant(
            read_state_code, 0, &read_state_jump_offset) &&
        game_bridge_generic_wrapper_variant(
            read_state_ext_code, 2, &read_state_ext_jump_offset) &&
        game_bridge_generic_wrapper_variant(
            read_code, 1, &read_jump_offset) &&
        game_bridge_generic_wrapper_variant(
            read_ext_code, 3, &read_ext_jump_offset) &&
        game_bridge_generic_wrapper_variant(
            data_internal_code, 4, &data_internal_jump_offset) &&
        game_bridge_generic_controller_info(controller_info_code);
    /* Keep an actionable read-only fingerprint when a new firmware is
     * rejected.  These bytes are captured before any target write and let a
     * future manifest be added without guessing from a generic failure. */
    report_printf(
        report_fd,
        "wrapper_shapes=state:%d state_ext:%d read:%d read_ext:%d "
        "data:%d info:%d\n"
        "info_prefix=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        game_bridge_generic_wrapper_variant(
            read_state_code, 0, &read_state_jump_offset),
        game_bridge_generic_wrapper_variant(
            read_state_ext_code, 2, &read_state_ext_jump_offset),
        game_bridge_generic_wrapper_variant(
            read_code, 1, &read_jump_offset),
        game_bridge_generic_wrapper_variant(
            read_ext_code, 3, &read_ext_jump_offset),
        game_bridge_generic_wrapper_variant(
            data_internal_code, 4, &data_internal_jump_offset),
        game_bridge_generic_controller_info(controller_info_code),
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
    if (exact_manifest) {
        if (!exact_wrapper_bytes) {
            report_printf(report_fd, "error=wrapper_bytes_mismatch\n");
            result = 0;
            goto cleanup;
        }
        report_printf(report_fd, "manifest_mode=11.60-exact\n");
    } else {
        if (firmware == DS4TOD5_GAME_BRIDGE_FW_1160 ||
            !generic_wrapper_bytes) {
            report_printf(report_fd,
                          "error=manifest_mismatch\n");
            result = 0;
            goto cleanup;
        }
        generic_manifest = 1;
        report_printf(report_fd, "manifest_mode=generic-structural\n");
    }

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
    if (generic_manifest) {
        if (game_bridge_module_bounds(target, base, &module_bounds) != 0) {
            report_printf(report_fd, "error=module_bounds\n");
            result = 0;
            goto cleanup;
        }
        report_printf(
            report_fd,
            "module_image_size=0x%llx module_text=0x%lx-0x%lx\n",
            (unsigned long long)module_bounds.image_size,
            (unsigned long)module_bounds.text_start,
            (unsigned long)module_bounds.text_end);
    }
    int generic_addresses_ok = !generic_manifest ||
        (game_bridge_address_in_text(
             &module_bounds, read_state, 16) &&
         game_bridge_address_in_text(
             &module_bounds, read_state_ext, 16) &&
         game_bridge_address_in_text(
             &module_bounds, read_fn, 16) &&
         game_bridge_address_in_text(
             &module_bounds, read_ext, 16) &&
         game_bridge_address_in_text(
             &module_bounds, data_internal, 16) &&
         game_bridge_address_in_text(
             &module_bounds, controller_info,
             sizeof(expected_controller_info_prologue)) &&
         game_bridge_address_in_text(
             &module_bounds, state_internal, 1) &&
         game_bridge_address_in_text(
             &module_bounds, read_internal, 1) &&
         game_bridge_address_in_text(
             &module_bounds, data_internal_target, 1));
    if (!wrapper_targets_ok || !generic_addresses_ok ||
        state_internal != state_ext_internal ||
        read_internal != read_ext_internal ||
        !game_bridge_generic_target_ok(base, state_internal) ||
        !game_bridge_generic_target_ok(base, read_internal) ||
        !game_bridge_generic_target_ok(base, data_internal_target) ||
        (exact_manifest &&
         ((uint32_t)(state_internal - base) != 0x1610u ||
          (uint32_t)(read_internal - base) != 0x1870u ||
          (uint32_t)(data_internal_target - base) != 0x0cb0u))) {
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
    if (exact_manifest) {
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
    } else if (generic_manifest) {
        /* The client-table stride is private and changes across firmware. */
        report_printf(report_fd, "type0_table_matches=skipped_generic\n");
    }

    uintptr_t local_stub_base = (uintptr_t)game_pad_read_state_stub;
    uintptr_t local_stub_end = (uintptr_t)game_pad_bridge_stub_end;
    uintptr_t local_stub_addresses[6] = {
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
    intptr_t wrapper_addresses[5] = {
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
    }
    if (result != 1 && patched_count > 0) {
        intptr_t wrapper_addresses[5] = {
            read_state, read_state_ext, read_fn, read_ext, data_internal};
        const uint8_t *originals[5] = {
            read_state_code, read_state_ext_code, read_code, read_ext_code,
            data_internal_code};
        for (int index = patched_count - 1; index >= 0; --index)
            (void)pt_io_write(target, wrapper_addresses[index],
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
done:
    if (result != 1)
        report_printf(report_fd, "result=%d\n", result);
    if (report_fd >= 0)
        close(report_fd);
    return result;
#endif
}

int
shellui_pad_game_bridge_update(pid_t game_pid, intptr_t args_kaddr,
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
shellui_pad_game_bridge_status(pid_t game_pid, intptr_t args_kaddr,
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
    if (remote_reader_copyout(
            game_pid, args_kaddr, &args, sizeof(args)) != 0 ||
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
shellui_pad_game_bridge_remove(pid_t game_pid, intptr_t args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr;
    return -1;
#else
    if (!args_kaddr)
        return -1;
    if (g_game_bridge_sender_fd < 0)
        g_game_bridge_sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_game_bridge_sender_fd >= 0) {
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
        (void)sendto(
            g_game_bridge_sender_fd, &stop_magic, sizeof(stop_magic), 0,
            (const struct sockaddr *)(const void *)&loopback,
            sizeof(loopback));
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            int32_t ready = 0;
            if (remote_reader_copyout(
                    game_pid,
                    args_kaddr + (intptr_t)offsetof(
                        GamePadBridgeArgs, receiver_ready),
                    &ready, sizeof(ready)) == 0 && ready == 2)
                break;
            usleep(10000);
        }
        close(g_game_bridge_sender_fd);
        g_game_bridge_sender_fd = -1;
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

static int
dump_remote_raw_mapping(pid_t pid, const char *process_name,
                        const char *module_name, uint32_t handle,
                        intptr_t base, int report_fd)
{
    char path[256];
    snprintf(path, sizeof(path),
             "/data/ds4tod5/remote-modules-1160/%s-%s.raw.bin",
             process_name, module_name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        report_printf(report_fd,
                      "process=%s pid=%d module=%s base=0x%lx "
                      "error=raw_open errno=%d\n",
                      process_name, pid, module_name, base, errno);
        return -1;
    }

    uint8_t page[0x1000];
    size_t copied = 0;
    int last_protection = 0;
    size_t limit = strcmp(module_name, "__main__") == 0
        ? 64u * 1024u * 1024u : 4u * 1024u * 1024u;
    while (copied < limit) {
        intptr_t address = base + (intptr_t)copied;
        int protection =
            kernel_get_vmem_protection(pid, address, sizeof(page));
        if (protection < 0 ||
            mdbg_copyout(pid, address, page, sizeof(page)) != 0)
            break;
        if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page))
            break;
        last_protection = protection;
        copied += sizeof(page);
    }
    close(fd);
    report_printf(report_fd,
                  "process=%s pid=%d module=%s handle=0x%x base=0x%lx "
                  "raw_copied=0x%zx last_protection=0x%x path=%s\n",
                  process_name, pid, module_name, handle, base, copied,
                  last_protection, path);
    return copied > 0 ? 1 : -1;
}

static int
dump_remote_module(pid_t pid, const char *process_name,
                   const char *module_name, int report_fd)
{
#if !defined(__PROSPERO__)
    (void)pid;
    (void)process_name;
    (void)module_name;
    (void)report_fd;
    return -1;
#else
    uint32_t handle = 0;
    if (strcmp(module_name, "__main__") != 0) {
        if (get_lib(pid, module_name, &handle) != 0)
            return 0;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    uint8_t header_page[0x1000];
    memset(header_page, 0, sizeof(header_page));
    if (!base ||
        mdbg_copyout(pid, base, header_page, sizeof(header_page)) != 0) {
        report_printf(report_fd,
                "process=%s pid=%d module=%s handle=0x%x base=0x%lx "
                "error=header_copyout errno=%d\n",
                process_name, pid, module_name, handle, base, errno);
        return -1;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)header_page;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > 64 ||
        ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) >
            sizeof(header_page)) {
        report_printf(report_fd,
                      "process=%s pid=%d module=%s handle=0x%x base=0x%lx "
                      "note=mapbase_is_raw_text\n",
                      process_name, pid, module_name, handle, base);
        return dump_remote_raw_mapping(
            pid, process_name, module_name, handle, base, report_fd);
    }

    const Elf64_Phdr *phdrs =
        (const Elf64_Phdr *)(header_page + ehdr->e_phoff);
    uint64_t image_size = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0)
            continue;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > image_size)
            image_size = end;
    }
    if (image_size == 0 || image_size > 64u * 1024u * 1024u) {
        report_printf(report_fd,
                "process=%s pid=%d module=%s base=0x%lx "
                "error=invalid_image_size size=0x%lx\n",
                process_name, pid, module_name, base,
                (unsigned long)image_size);
        return -1;
    }

    char safe_process[64];
    char safe_module[64];
    size_t process_len = 0;
    size_t module_len = 0;
    for (size_t i = 0;
         process_name[i] && process_len + 1 < sizeof(safe_process);
         ++i) {
        char c = process_name[i];
        safe_process[process_len++] =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ? c : '_';
    }
    safe_process[process_len] = '\0';
    for (size_t i = 0;
         module_name[i] && module_len + 1 < sizeof(safe_module);
         ++i) {
        char c = module_name[i];
        safe_module[module_len++] =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ? c : '_';
    }
    safe_module[module_len] = '\0';

    char path[256];
    snprintf(path, sizeof(path),
             "/data/ds4tod5/remote-modules-1160/%s-%s.elf",
             safe_process, safe_module);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)image_size) != 0) {
        report_printf(report_fd,
                "process=%s pid=%d module=%s base=0x%lx "
                "error=open_output path=%s errno=%d\n",
                process_name, pid, module_name, base, path, errno);
        if (fd >= 0)
            close(fd);
        return -1;
    }

    uint8_t *buffer = malloc(0x10000);
    int result = buffer ? 1 : -1;
    uint64_t total = 0;
    for (uint16_t i = 0; buffer && i < ehdr->e_phnum; ++i) {
        const Elf64_Phdr *phdr = &phdrs[i];
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue;
        for (uint64_t offset = 0; offset < phdr->p_memsz;) {
            size_t chunk = (size_t)(phdr->p_memsz - offset);
            if (chunk > 0x10000)
                chunk = 0x10000;
            if (mdbg_copyout(
                    pid, base + (intptr_t)phdr->p_vaddr +
                             (intptr_t)offset,
                    buffer, chunk) != 0 ||
                pwrite(fd, buffer, chunk,
                       (off_t)(phdr->p_vaddr + offset)) !=
                    (ssize_t)chunk) {
                report_printf(report_fd,
                        "process=%s pid=%d module=%s segment=%u "
                        "offset=0x%lx error=copy errno=%d\n",
                        process_name, pid, module_name, i,
                        (unsigned long)offset, errno);
                result = -1;
                break;
            }
            offset += chunk;
            total += chunk;
        }
        if (result < 0)
            break;
    }
    free(buffer);
    close(fd);

    report_printf(report_fd,
            "process=%s pid=%d module=%s handle=0x%x base=0x%lx "
            "phnum=%u image_size=0x%lx copied=0x%lx result=%d path=%s\n",
            process_name, pid, module_name, handle, base, ehdr->e_phnum,
            (unsigned long)image_size, (unsigned long)total, result, path);
    return result;
#endif
}

int
shellui_pad_dump_remote_modules(void)
{
#if !defined(__PROSPERO__)
    return -1;
#else
    static const char *const process_names[] = {
        "SceShellUI", "SceShellCore", "SceRemotePlay",
    };
    static const char *const module_names[] = {
        "__main__", "libSceMbus", "libScePad", "libSceRegMgr",
    };
    int successes = 0;
    int failures = 0;

    mkdir("/data/ds4tod5", 0755);
    mkdir("/data/ds4tod5/remote-modules-1160", 0755);
    int report_fd = open(
        "/data/ds4tod5/remote-modules-1160.txt",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (report_fd < 0)
        return -1;
    report_printf(report_fd, "firmware=0x%08x\n", kernel_get_fw_version());

    for (size_t p = 0;
         p < sizeof(process_names) / sizeof(process_names[0]);
         ++p) {
        pid_t pids[8];
        size_t count = find_pids(process_names[p], pids, 8);
        report_printf(report_fd, "process_lookup name=%s count=%zu\n",
                process_names[p], count);
        for (size_t i = 0; i < count; ++i) {
            for (size_t m = 0;
                 m < sizeof(module_names) / sizeof(module_names[0]);
                 ++m) {
                if (strcmp(process_names[p], "SceRemotePlay") == 0 &&
                    strcmp(module_names[m], "__main__") == 0)
                    continue;
                int result = dump_remote_module(
                    pids[i], process_names[p], module_names[m], report_fd);
                if (result > 0)
                    ++successes;
                else if (result < 0)
                    ++failures;
            }
        }
    }
    report_printf(report_fd, "successes=%d\nfailures=%d\n",
            successes, failures);
    close(report_fd);
    klog_printf("[DS4toDS5] remote module dump successes=%d failures=%d\n",
                successes, failures);
    return successes > 0 && failures == 0 ? successes :
        (successes > 0 ? successes : -1);
#endif
}

/*
 * Read-only capture of the boot-time controller-policy processes. This does
 * not attach, suspend, change VM protection, or write to a target process.
 * It is intentionally separate from the translator and hook experiments.
 */
int
shellui_pad_dump_controller_policy_modules(void)
{
#if !defined(__PROSPERO__)
    return -1;
#else
    static const char *const process_names[] = {
        "SceHidAuth",
        "SceHidMain",
        "SceBtDriver",
        "SceSysCore.elf",
    };
    static const char *const optional_modules[] = {
        "libSceRegMgr",
        "libScePad",
    };
    int successes = 0;
    int failures = 0;

    mkdir("/data/ds4tod5", 0755);
    mkdir("/data/ds4tod5/remote-modules-1160", 0755);
    int report_fd = open(
        "/data/ds4tod5/controller-policy-modules-1160.txt",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (report_fd < 0)
        return -1;

    report_printf(
        report_fd,
        "mode=read-only-no-attach-no-target-write\n"
        "firmware=0x%08x\n",
        kernel_get_fw_version());

    for (size_t p = 0;
         p < sizeof(process_names) / sizeof(process_names[0]);
         ++p) {
        pid_t pids[8];
        size_t count = find_pids(process_names[p], pids, 8);
        report_printf(
            report_fd, "process_lookup name=%s count=%zu\n",
            process_names[p], count);
        if (count == 0) {
            ++failures;
            continue;
        }

        for (size_t i = 0; i < count; ++i) {
            int result = dump_remote_module(
                pids[i], process_names[p], "__main__", report_fd);
            if (result > 0)
                ++successes;
            else
                ++failures;

            for (size_t m = 0;
                 m < sizeof(optional_modules) /
                     sizeof(optional_modules[0]);
                 ++m) {
                result = dump_remote_module(
                    pids[i], process_names[p], optional_modules[m],
                    report_fd);
                if (result > 0)
                    ++successes;
                else if (result < 0)
                    ++failures;
            }
        }
    }

    report_printf(
        report_fd, "successes=%d\nfailures=%d\n", successes, failures);
    close(report_fd);
    klog_printf(
        "[DS4toDS5] controller-policy dump successes=%d failures=%d\n",
        successes, failures);
    return successes > 0 ? successes : -1;
#endif
}

/*
 * Resolve and fingerprint the native PS4-compatibility entry points exported
 * by libScePad. This is a discovery-only operation: no ptrace attach, target
 * suspension, target write, VM-protection change, ioctl, or function call.
 *
 * The report supplies the exact 11.60 offsets and bytes needed to derive a
 * firmware-locked call/patch manifest before any state-changing experiment.
 */
int
shellui_pad_dump_native_pad_symbols(void)
{
#if !defined(__PROSPERO__)
    return -1;
#else
    static const char *const process_names[] = {
        "SceRemotePlay",
        "SceShellCore",
        "SceShellUI",
        "SceSysCore.elf",
        "SceHidMain",
        "SceHidAuth",
        "SceBtDriver",
        "eboot.bin",
    };
    static const char *const symbol_names[] = {
        "scePadEnablePs4CompatibleMode",
        "scePadIsDS4Connected",
        "scePadSetPS4BcVibrationMode",
        "scePadGetDeviceInfo",
        "scePadGetControllerInformation",
        "scePadGetExtControllerInformation",
        "scePadGetCapability",
        "scePadEnableAutoDetect",
        "scePadEnableSpecificDeviceClass",
        "scePadOpen",
        "scePadGetHandle",
        "scePadReadState",
        "scePadReadStateExt",
        "scePadRead",
        "scePadReadExt",
        "scePadReadHistory",
        "scePadReadForTracker",
        "scePadReadBlasterForTracker",
        "scePadGetDataInternal",
        "scePadGetLicenseControllerInformation",
        "scePadInit",
        "scePadMbusInit",
        "scePadSetProcessPrivilege",
    };
    int successes = 0;
    int failures = 0;

    mkdir("/data/ds4tod5", 0755);
    int report_fd = open(
        "/data/ds4tod5/native-pad-symbols-1160.txt",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (report_fd < 0)
        return -1;

    report_printf(
        report_fd,
        "mode=read-only-no-attach-no-target-write-no-target-call\n"
        "firmware=0x%08x\n",
        kernel_get_fw_version());

    for (size_t p = 0;
         p < sizeof(process_names) / sizeof(process_names[0]);
         ++p) {
        pid_t pids[16];
        size_t count = find_pids(process_names[p], pids, 16);
        report_printf(
            report_fd, "process_lookup name=%s count=%zu\n",
            process_names[p], count);

        for (size_t i = 0; i < count; ++i) {
            uint32_t handle = 0;
            if (get_lib(pids[i], "libScePad", &handle) != 0) {
                report_printf(
                    report_fd,
                    "module process=%s pid=%d name=libScePad present=0\n",
                    process_names[p], pids[i]);
                continue;
            }

            intptr_t base = kernel_dynlib_mapbase_addr(pids[i], handle);
            report_printf(
                report_fd,
                "module process=%s pid=%d name=libScePad handle=0x%x "
                "base=0x%lx\n",
                process_names[p], pids[i], handle,
                (unsigned long)base);

            for (size_t s = 0;
                 s < sizeof(symbol_names) / sizeof(symbol_names[0]);
                 ++s) {
                char nid[12];
                uint8_t code[256];
                intptr_t address =
                    resolve_sym(pids[i], handle, symbol_names[s]);
                nid_encode(symbol_names[s], nid);
                memset(code, 0, sizeof(code));

                if (!address ||
                    mdbg_copyout(
                        pids[i], address, code, sizeof(code)) != 0) {
                    report_printf(
                        report_fd,
                        "symbol process=%s pid=%d name=%s nid=%s "
                        "address=0x%lx offset=0x%lx readable=0 errno=%d\n",
                        process_names[p], pids[i], symbol_names[s], nid,
                        (unsigned long)address,
                        (unsigned long)(
                            base && address ? address - base : 0),
                        errno);
                    ++failures;
                    continue;
                }

                report_printf(
                    report_fd,
                    "symbol process=%s pid=%d name=%s nid=%s "
                    "address=0x%lx offset=0x%lx readable=1 "
                    "fnv256=0x%016llx bytes64=",
                    process_names[p], pids[i], symbol_names[s], nid,
                    (unsigned long)address,
                    (unsigned long)(base ? address - base : 0),
                    (unsigned long long)ghostpad_fnv1a64(
                        code, sizeof(code)));
                for (size_t b = 0; b < 64; ++b)
                    report_printf(report_fd, "%02x", code[b]);
                report_printf(report_fd, "\n");
                ++successes;
            }
        }
    }

    report_printf(
        report_fd, "successes=%d\nfailures=%d\n", successes, failures);
    close(report_fd);
    klog_printf(
        "[DS4toDS5] native-pad symbol dump successes=%d failures=%d\n",
        successes, failures);
    return successes > 0 ? successes : -1;
#endif
}

/*
 * Locate resolved libScePad function pointers in the native game's readable,
 * non-executable mappings.  This is a read-only precursor to a game-local GOT
 * hook: it does not attach, pause, mprotect, or write target memory.
 */
int
shellui_pad_dump_game_pad_imports(void)
{
#if !defined(__PROSPERO__)
    return -1;
#else
    static const char *const symbol_names[] = {
        "scePadGetHandle",
        "scePadOpen",
        "scePadReadState",
        "scePadReadStateExt",
        "scePadRead",
        "scePadReadExt",
        "scePadReadHistory",
        "scePadReadForTracker",
        "scePadReadBlasterForTracker",
        "scePadGetDataInternal",
        "scePadGetControllerInformation",
        "scePadGetExtControllerInformation",
        "scePadGetDeviceInfo",
        "scePadGetCapability",
        "scePadIsDS4Connected",
        "scePadGetDeviceId",
        "scePadGetLicenseControllerInformation",
    };
    enum {
        SYMBOL_COUNT = sizeof(symbol_names) / sizeof(symbol_names[0]),
        MAX_MATCH_REPORTS = 64,
    };
    pid_t pids[8];
    size_t process_count = find_pids("eboot.bin", pids, 8);
    mkdir("/data/ds4tod5", 0755);
    int report_fd = open(
        "/data/ds4tod5/game-pad-imports-1160.txt",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (report_fd < 0)
        return -1;
    report_printf(
        report_fd,
        "mode=read-only-no-attach-no-target-write-no-target-call\n"
        "firmware=0x%08x\nprocess_count=%zu\n",
        kernel_get_fw_version(), process_count);
    if (process_count != 1) {
        report_printf(report_fd, "error=process_count\n");
        close(report_fd);
        return -1;
    }
    pid_t target = pids[0];
    uint32_t libpad_handle = 0;
    if (get_lib(target, "libScePad", &libpad_handle) != 0) {
        report_printf(report_fd, "pid=%d\nerror=libpad\n", target);
        close(report_fd);
        return -1;
    }
    intptr_t libpad_base =
        kernel_dynlib_mapbase_addr(target, libpad_handle);
    intptr_t symbol_addresses[SYMBOL_COUNT];
    uint64_t match_counts[SYMBOL_COUNT];
    uint32_t reported_matches[SYMBOL_COUNT];
    memset(symbol_addresses, 0, sizeof(symbol_addresses));
    memset(match_counts, 0, sizeof(match_counts));
    memset(reported_matches, 0, sizeof(reported_matches));
    report_printf(
        report_fd, "pid=%d\nlibpad_handle=0x%x\nlibpad_base=0x%lx\n",
        target, libpad_handle, (unsigned long)libpad_base);
    for (size_t symbol = 0; symbol < SYMBOL_COUNT; ++symbol) {
        symbol_addresses[symbol] =
            resolve_sym(target, libpad_handle, symbol_names[symbol]);
        report_printf(
            report_fd, "symbol name=%s address=0x%lx offset=0x%lx\n",
            symbol_names[symbol],
            (unsigned long)symbol_addresses[symbol],
            (unsigned long)(symbol_addresses[symbol] && libpad_base
                ? symbol_addresses[symbol] - libpad_base : 0));
    }

    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, target};
    size_t map_size = 0;
    pid_t self = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (saved_authid)
        kernel_set_ucred_authid(self, UINT64_C(0x4800000000010003));
    int sysctl_result = sysctl(mib, 4, NULL, &map_size, NULL, 0);
    uint8_t *map_buffer = NULL;
    if (sysctl_result == 0 && map_size > 0 && map_size <= 4u * 1024u * 1024u)
        map_buffer = malloc(map_size + 4096u);
    size_t map_capacity = map_buffer ? map_size + 4096u : 0;
    if (map_buffer) {
        map_size = map_capacity;
        sysctl_result =
            sysctl(mib, 4, map_buffer, &map_size, NULL, 0);
    }
    report_printf(
        report_fd, "vmmap_result=%d vmmap_size=%zu errno=%d\n",
        sysctl_result, map_size, errno);
    if (sysctl_result != 0 || !map_buffer) {
        if (saved_authid)
            kernel_set_ucred_authid(self, saved_authid);
        free(map_buffer);
        close(report_fd);
        return -1;
    }

    uint8_t *page = malloc(0x4000);
    uint64_t scanned_bytes = 0;
    uint64_t scan_budget = UINT64_C(512) * 1024u * 1024u;
    unsigned map_index = 0;
    unsigned scanned_maps = 0;
    unsigned failed_maps = 0;
    for (uint8_t *cursor = map_buffer;
         cursor + sizeof(int) <= map_buffer + map_size;) {
        struct kinfo_vmentry *entry =
            (struct kinfo_vmentry *)(void *)cursor;
        if (entry->kve_structsize <= 0 ||
            cursor + (size_t)entry->kve_structsize > map_buffer + map_size)
            break;
        uint64_t start = entry->kve_start;
        uint64_t end = entry->kve_end;
        uint64_t length = end > start ? end - start : 0;
        int readable = (entry->kve_protection & KVME_PROT_READ) != 0;
        int executable = (entry->kve_protection & KVME_PROT_EXEC) != 0;
        int scan = page && readable && !executable && length > 0 &&
            length <= UINT64_C(128) * 1024u * 1024u &&
            scanned_bytes + length <= scan_budget;
        report_printf(
            report_fd,
            "map index=%u start=0x%llx end=0x%llx size=0x%llx "
            "prot=0x%x scan=%d path=%.512s\n",
            map_index, (unsigned long long)start,
            (unsigned long long)end, (unsigned long long)length,
            entry->kve_protection, scan, entry->kve_path);
        if (scan) {
            int map_failed = 0;
            for (uint64_t offset = 0; offset < length; offset += 0x4000u) {
                size_t chunk = (size_t)(length - offset);
                if (chunk > 0x4000u)
                    chunk = 0x4000u;
                if (mdbg_copyout(
                        target, (intptr_t)(start + offset), page, chunk) != 0) {
                    report_printf(
                        report_fd,
                        "map_read_error index=%u address=0x%llx errno=%d\n",
                        map_index,
                        (unsigned long long)(start + offset), errno);
                    map_failed = 1;
                    break;
                }
                uintptr_t aligned =
                    ((uintptr_t)start + (uintptr_t)offset + 7u) &
                    ~(uintptr_t)7u;
                size_t first =
                    (size_t)(aligned - ((uintptr_t)start + offset));
                for (size_t byte = first; byte + 8u <= chunk; byte += 8u) {
                    uint64_t value = 0;
                    memcpy(&value, page + byte, sizeof(value));
                    for (size_t symbol = 0;
                         symbol < SYMBOL_COUNT; ++symbol) {
                        if (!symbol_addresses[symbol] ||
                            value != (uint64_t)symbol_addresses[symbol])
                            continue;
                        match_counts[symbol]++;
                        if (reported_matches[symbol] < MAX_MATCH_REPORTS) {
                            report_printf(
                                report_fd,
                                "match symbol=%s slot=0x%llx map=%u "
                                "prot=0x%x path=%.512s\n",
                                symbol_names[symbol],
                                (unsigned long long)(start + offset + byte),
                                map_index, entry->kve_protection,
                                entry->kve_path);
                            reported_matches[symbol]++;
                        }
                    }
                }
            }
            if (map_failed)
                failed_maps++;
            else {
                scanned_bytes += length;
                scanned_maps++;
            }
        }
        cursor += entry->kve_structsize;
        map_index++;
    }
    if (saved_authid)
        kernel_set_ucred_authid(self, saved_authid);
    free(page);
    free(map_buffer);
    uint64_t total_matches = 0;
    for (size_t symbol = 0; symbol < SYMBOL_COUNT; ++symbol) {
        report_printf(
            report_fd, "summary symbol=%s matches=%llu reported=%u\n",
            symbol_names[symbol],
            (unsigned long long)match_counts[symbol],
            reported_matches[symbol]);
        total_matches += match_counts[symbol];
    }
    report_printf(
        report_fd,
        "maps=%u scanned_maps=%u failed_maps=%u scanned_bytes=%llu "
        "total_matches=%llu\n",
        map_index, scanned_maps, failed_maps,
        (unsigned long long)scanned_bytes,
        (unsigned long long)total_matches);
    close(report_fd);
    klog_printf(
        "[DS4toDS5] game import scan maps=%u scanned=%u matches=%llu\n",
        map_index, scanned_maps, (unsigned long long)total_matches);
    return total_matches > 0 ? (int)total_matches : 0;
#endif
}

/*
 * Firmware-locked implementation of the native PS4-compatible pad switch.
 *
 * Live 11.60 disassembly corrected the original hypothesis: this is not a
 * global/no-argument switch.  The function consumes (pad_handle, enable),
 * verifies that the handle belongs to the current libScePad client, stores the
 * enable bit in that client's record, and issues ctrlp ioctl 0x8008485b with
 * mode 3 (enabled) or mode 1 (disabled).  Therefore every state-changing call
 * must first discover a genuine type-0 handle in the same target process.
 */
#define DS4TOD5_NATIVE_COMPAT_FW_1160 UINT32_C(0x11600005)
#define DS4TOD5_NATIVE_COMPAT_OFFSET_1160 UINT32_C(0x0000c890)
#define DS4TOD5_NATIVE_COMPAT_FNV256_1160 \
    UINT64_C(0x8659a7f9dc4368f6)
#define DS4TOD5_GET_HANDLE_OFFSET_1160 UINT32_C(0x00001560)
#define DS4TOD5_GET_HANDLE_FNV256_1160 \
    UINT64_C(0x6d0bdf486c63b986)
#define DS4TOD5_IS_DS4_OFFSET_1160 UINT32_C(0x000047a0)
#define DS4TOD5_IS_DS4_FNV256_1160 \
    UINT64_C(0xbb76044b6d67c7f3)
#define DS4TOD5_PAD_CLIENT_TABLE_OFFSET_1160 UINT32_C(0x00020018)
#define DS4TOD5_PAD_CLIENT_HANDLE_OFFSET_1160 UINT32_C(0x0000001c)
#define DS4TOD5_PAD_CLIENT_COMPAT_FLAGS_OFFSET_1160 UINT32_C(0x00000254)
#define DS4TOD5_PAD_CLIENT_STRIDE_1160 UINT32_C(0x000005c8)
#define DS4TOD5_PAD_CLIENT_SLOTS_1160 16

#ifndef DS4TOD5_ALLOW_NATIVE_COMPAT_CALL
#define DS4TOD5_ALLOW_NATIVE_COMPAT_CALL 0
#endif

int
shellui_pad_native_ps4_compat(const char *target_name, int32_t user_id,
                              int action)
{
#if !defined(__PROSPERO__)
    (void)target_name;
    (void)user_id;
    (void)action;
    return -1;
#else
    pid_t pids[8];
    uint8_t compat_code[256];
    uint8_t get_handle_code[256];
    uint8_t is_ds4_code[256];
    uint32_t handle = 0;
    uint32_t firmware = kernel_get_fw_version();
    int report_fd = -1;
    int attached = 0;
    int trap_saved = 0;
    int trap_writable = 0;
    int trap_modified = 0;
    uint8_t original_trap_byte = 0;
    intptr_t trap = 0;
    int original_protection = PROT_READ | PROT_EXEC;
    int result = -1;
    int32_t pad_handle = -1;
    int32_t is_ds4_before = -1;
    int32_t is_ds4_after = -1;
    int32_t compat_result = INT32_MIN;
    int selected_slot = -1;
    uint32_t compat_flags_before = 0;
    uint32_t compat_flags_after = 0;
    uint32_t table_handles[DS4TOD5_PAD_CLIENT_SLOTS_1160];
    memset(table_handles, 0, sizeof(table_handles));

    if (!target_name || !target_name[0])
        target_name = "SceRemotePlay";

    mkdir("/data/ds4tod5", 0755);
    report_fd = open(
        "/data/ds4tod5/native-pad-compat-last.txt",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    report_printf(
        report_fd,
        "firmware=0x%08x\ntarget=%s\nuser_id=0x%08x\naction=%d\n",
        firmware, target_name, (uint32_t)user_id, action);

    if (action < DS4TOD5_NATIVE_COMPAT_DISCOVER ||
        action > DS4TOD5_NATIVE_COMPAT_DISABLE) {
        report_printf(report_fd, "error=invalid_action\n");
        result = 0;
        goto native_compat_done;
    }
    if (firmware != DS4TOD5_NATIVE_COMPAT_FW_1160) {
        klog_printf(
            "[DS4toDS5] native compat refused: firmware=0x%08x "
            "expected=0x%08x\n",
            firmware, DS4TOD5_NATIVE_COMPAT_FW_1160);
        report_printf(report_fd, "error=firmware_mismatch\n");
        result = 0;
        goto native_compat_done;
    }

    size_t count = find_pids(target_name, pids, 8);
    report_printf(report_fd, "process_count=%zu\n", count);
    if (count != 1) {
        klog_printf(
            "[DS4toDS5] native compat refused: target=%s count=%zu "
            "(exactly one required)\n",
            target_name, count);
        report_printf(report_fd, "error=process_count\n");
        result = 0;
        goto native_compat_done;
    }
    pid_t target = pids[0];
    report_printf(report_fd, "pid=%d\n", target);

    if (get_lib(target, "libScePad", &handle) != 0) {
        klog_printf(
            "[DS4toDS5] native compat: target=%s pid=%d has no libScePad\n",
            target_name, target);
        report_printf(report_fd, "error=no_libScePad\n");
        result = 0;
        goto native_compat_done;
    }

    intptr_t base = kernel_dynlib_mapbase_addr(target, handle);
    intptr_t function =
        resolve_sym(target, handle, "scePadEnablePs4CompatibleMode");
    intptr_t get_handle =
        resolve_sym(target, handle, "scePadGetHandle");
    intptr_t is_ds4 =
        resolve_sym(target, handle, "scePadIsDS4Connected");
    if (!base || !function ||
        !get_handle || !is_ds4 ||
        mdbg_copyout(
            target, function, compat_code, sizeof(compat_code)) != 0 ||
        mdbg_copyout(
            target, get_handle, get_handle_code,
            sizeof(get_handle_code)) != 0 ||
        mdbg_copyout(
            target, is_ds4, is_ds4_code, sizeof(is_ds4_code)) != 0) {
        klog_printf(
            "[DS4toDS5] native compat: resolve/copy failed target=%s "
            "pid=%d base=0x%lx fn=0x%lx errno=%d\n",
            target_name, target, (unsigned long)base,
            (unsigned long)function, errno);
        report_printf(report_fd, "error=resolve_or_copy errno=%d\n", errno);
        result = -1;
        goto native_compat_done;
    }

    uint32_t compat_offset = (uint32_t)(function - base);
    uint32_t get_handle_offset = (uint32_t)(get_handle - base);
    uint32_t is_ds4_offset = (uint32_t)(is_ds4 - base);
    uint64_t compat_fingerprint =
        ghostpad_fnv1a64(compat_code, sizeof(compat_code));
    uint64_t get_handle_fingerprint =
        ghostpad_fnv1a64(get_handle_code, sizeof(get_handle_code));
    uint64_t is_ds4_fingerprint =
        ghostpad_fnv1a64(is_ds4_code, sizeof(is_ds4_code));
    klog_printf(
        "[DS4toDS5] native compat discovery target=%s pid=%d "
        "base=0x%lx fn=0x%lx offset=0x%x fnv256=0x%016llx action=%d\n",
        target_name, target, (unsigned long)base, (unsigned long)function,
        compat_offset, (unsigned long long)compat_fingerprint, action);
    report_printf(
        report_fd,
        "libpad_base=0x%lx\n"
        "compat_offset=0x%x\ncompat_fnv256=0x%016llx\n"
        "get_handle_offset=0x%x\nget_handle_fnv256=0x%016llx\n"
        "is_ds4_offset=0x%x\nis_ds4_fnv256=0x%016llx\n",
        (unsigned long)base,
        compat_offset, (unsigned long long)compat_fingerprint,
        get_handle_offset, (unsigned long long)get_handle_fingerprint,
        is_ds4_offset, (unsigned long long)is_ds4_fingerprint);

    if (compat_offset != DS4TOD5_NATIVE_COMPAT_OFFSET_1160 ||
        compat_fingerprint != DS4TOD5_NATIVE_COMPAT_FNV256_1160 ||
        get_handle_offset != DS4TOD5_GET_HANDLE_OFFSET_1160 ||
        get_handle_fingerprint != DS4TOD5_GET_HANDLE_FNV256_1160 ||
        is_ds4_offset != DS4TOD5_IS_DS4_OFFSET_1160 ||
        is_ds4_fingerprint != DS4TOD5_IS_DS4_FNV256_1160) {
        klog_printf(
            "[DS4toDS5] native compat apply refused: manifest mismatch "
            "offset=0x%x/0x%x fnv=0x%016llx/0x%016llx\n",
            compat_offset, DS4TOD5_NATIVE_COMPAT_OFFSET_1160,
            (unsigned long long)compat_fingerprint,
            (unsigned long long)DS4TOD5_NATIVE_COMPAT_FNV256_1160);
        report_printf(report_fd, "error=manifest_mismatch\n");
        result = 0;
        goto native_compat_done;
    }
    report_printf(report_fd, "manifest=verified\n");

    if (action == DS4TOD5_NATIVE_COMPAT_DISCOVER) {
        result = 1;
        goto native_compat_done;
    }

#if !DS4TOD5_ALLOW_NATIVE_COMPAT_CALL
    if (action >= DS4TOD5_NATIVE_COMPAT_ENABLE) {
        klog_printf(
            "[DS4toDS5] native compat apply compile-gated; "
            "DS4TOD5_ALLOW_NATIVE_COMPAT_CALL=0\n");
        report_printf(report_fd, "error=apply_compile_gated\n");
        result = 0;
        goto native_compat_done;
    }
#endif

    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf(
            "[DS4toDS5] native compat PT_ATTACH failed errno=%d\n",
            errno);
        report_printf(report_fd, "error=attach errno=%d\n", errno);
        result = -1;
        goto native_compat_done;
    }
    attached = 1;
    if (waitpid(target, NULL, 0) < 0) {
        report_printf(report_fd, "error=wait_attach errno=%d\n", errno);
        goto cleanup_native_compat;
    }

    trap = kernel_dynlib_fini_addr(target, handle);
    if (!trap)
        trap = kernel_dynlib_init_addr(target, handle);
    if (!trap ||
        mdbg_copyout(target, trap, &original_trap_byte, 1) != 0) {
        klog_printf(
            "[DS4toDS5] native compat trap discovery failed errno=%d\n",
            errno);
        report_printf(report_fd, "error=trap_discovery errno=%d\n", errno);
        goto cleanup_native_compat;
    }
    trap_saved = 1;

    {
        int protection =
            kernel_get_vmem_protection(target, trap, 1);
        if (protection >= 0)
            original_protection = protection;
    }
    if (kernel_set_vmem_protection(
            target, trap, 16,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        klog_printf(
            "[DS4toDS5] native compat trap RWX failed errno=%d\n",
            errno);
        report_printf(report_fd, "error=trap_protection errno=%d\n", errno);
        goto cleanup_native_compat;
    }
    trap_writable = 1;

    {
        uint8_t int3 = 0xcc;
        if (pt_io_write(target, trap, &int3, 1) != 0) {
            klog_printf(
                "[DS4toDS5] native compat trap write failed errno=%d\n",
                errno);
            report_printf(report_fd, "error=trap_write errno=%d\n", errno);
            goto cleanup_native_compat;
        }
        trap_modified = 1;
    }

    int32_t get_handle_result = (int32_t)pt_call(
        target, get_handle, trap,
        (uint32_t)user_id, 0, 0, 0, 0, 0);
    report_printf(
        report_fd, "get_handle_type0=0x%08x\n",
        (uint32_t)get_handle_result);
    if (get_handle_result < 0) {
        klog_printf(
            "[DS4toDS5] native compat getHandle(user=0x%08x,type=0) "
            "failed=0x%08x\n", (uint32_t)user_id,
            (uint32_t)get_handle_result);
    }

    /*
     * GetHandle is observational: map the game client's user/type/index view
     * before choosing a handle for a later compatibility-mode call.  A native
     * PS5 game may have valid type-0 client handles while IsDS4Connected is
     * still false; that is exactly the state this API is intended to change.
     */
    if (action == DS4TOD5_NATIVE_COMPAT_HANDLE_PROBE) {
        for (unsigned type = 0; type < 4; ++type) {
            for (unsigned index = 0; index < 8; ++index) {
                int32_t gh = (int32_t)pt_call(
                    target, get_handle, trap,
                    (uint32_t)user_id, type, index, 0, 0, 0);
                report_printf(
                    report_fd,
                    "get_handle type=%u index=%u result=0x%08x\n",
                    type, index, (uint32_t)gh);
            }
        }
    }

    int handle_read_failed = 0;
    for (unsigned slot = 0;
         slot < DS4TOD5_PAD_CLIENT_SLOTS_1160;
         ++slot) {
        intptr_t handle_address =
            base + DS4TOD5_PAD_CLIENT_TABLE_OFFSET_1160 +
            DS4TOD5_PAD_CLIENT_HANDLE_OFFSET_1160 +
            (intptr_t)slot * DS4TOD5_PAD_CLIENT_STRIDE_1160;
        if (mdbg_copyout(
                target, handle_address, &table_handles[slot],
                sizeof(table_handles[slot])) != 0) {
            report_printf(
                report_fd, "slot=%u error=handle_copyout errno=%d\n",
                slot, errno);
            handle_read_failed = 1;
            break;
        }
    }
    if (handle_read_failed) {
        result = -1;
        goto cleanup_native_compat;
    }

    int ds4_count = 0;
    int handle_call_failed = 0;
    for (unsigned slot = 0;
         slot < DS4TOD5_PAD_CLIENT_SLOTS_1160;
         ++slot) {
        uint32_t candidate = table_handles[slot];
        if (candidate == 0)
            continue;
        int32_t candidate_is_ds4 = (int32_t)pt_call(
            target, is_ds4, trap, candidate, 0, 0, 0, 0, 0);
        uint32_t candidate_compat_flags = 0;
        intptr_t candidate_flags_address =
            base + DS4TOD5_PAD_CLIENT_TABLE_OFFSET_1160 +
            (intptr_t)slot * DS4TOD5_PAD_CLIENT_STRIDE_1160 +
            DS4TOD5_PAD_CLIENT_COMPAT_FLAGS_OFFSET_1160;
        int candidate_flags_ok = mdbg_copyout(
            target, candidate_flags_address, &candidate_compat_flags,
            sizeof(candidate_compat_flags)) == 0;
        report_printf(
            report_fd,
            "slot=%u handle=0x%08x is_ds4=%d compat_flags=%s0x%08x\n",
            slot, candidate, candidate_is_ds4,
            candidate_flags_ok ? "" : "unreadable:",
            candidate_compat_flags);
        if (candidate_is_ds4 < 0) {
            handle_call_failed = 1;
            continue;
        }
        /*
         * 11.60's implementation returns zero for false. For true it sets
         * only BL=1 after EBX was initialized from the handle, so the upper
         * handle bytes remain in EAX (for example 0x030a0401), not integer 1.
         */
        if (candidate_is_ds4 != 0) {
            pad_handle = (int32_t)candidate;
            is_ds4_before = candidate_is_ds4;
            selected_slot = (int)slot;
            ds4_count++;
        }
    }
    report_printf(report_fd, "ds4_handle_count=%d\n", ds4_count);

    /*
     * A native PS5 title initially reports IsDS4Connected=false precisely
     * because PS4-compatible mode has not been enabled for its client handle.
     * For an exact eboot.bin target, select only the unique standard-pad handle
     * returned by GetHandle(user, type=0, index=0).  Shell processes continue
     * to require a uniquely identified DS4, which prevents us from changing an
     * unrelated system handle.
     */
    if (!handle_call_failed && strcmp(target_name, "eboot.bin") == 0 &&
        get_handle_result >= 0) {
        int get_handle_matches = 0;
        int get_handle_slot = -1;
        for (unsigned slot = 0;
             slot < DS4TOD5_PAD_CLIENT_SLOTS_1160;
             ++slot) {
            if (table_handles[slot] == (uint32_t)get_handle_result) {
                get_handle_matches++;
                get_handle_slot = (int)slot;
            }
        }
        report_printf(
            report_fd, "game_type0_handle_matches=%d\n",
            get_handle_matches);
        if (get_handle_matches == 1) {
            pad_handle = get_handle_result;
            selected_slot = get_handle_slot;
            is_ds4_before = 0;
            report_printf(report_fd, "selection_basis=game_type0_handle\n");
        }
    }

    if (handle_call_failed || selected_slot < 0) {
        report_printf(
            report_fd, "error=%s\n",
            handle_call_failed ? "handle_call_failed" :
                                 "no_unique_safe_handle");
        result = handle_call_failed ? -1 : 0;
        goto cleanup_native_compat;
    }
    report_printf(
        report_fd,
        "selected_slot=%d\nselected_pad_handle=0x%08x\n"
        "selected_is_ds4=0x%08x\n",
        selected_slot, (uint32_t)pad_handle, (uint32_t)is_ds4_before);

    intptr_t compat_flags_address =
        base + DS4TOD5_PAD_CLIENT_TABLE_OFFSET_1160 +
        (intptr_t)selected_slot * DS4TOD5_PAD_CLIENT_STRIDE_1160 +
        DS4TOD5_PAD_CLIENT_COMPAT_FLAGS_OFFSET_1160;
    if (mdbg_copyout(
            target, compat_flags_address, &compat_flags_before,
            sizeof(compat_flags_before)) != 0) {
        report_printf(
            report_fd, "error=compat_flags_before errno=%d\n", errno);
        result = -1;
        goto cleanup_native_compat;
    }
    report_printf(
        report_fd, "compat_flags_before=0x%08x\n",
        compat_flags_before);

    if (action == DS4TOD5_NATIVE_COMPAT_HANDLE_PROBE) {
        uint32_t libc_handle = 0;
        intptr_t read_state =
            resolve_sym(target, handle, "scePadReadState");
        intptr_t get_controller_info = resolve_sym(
            target, handle, "scePadGetControllerInformation");
        intptr_t malloc_fn = 0;
        intptr_t free_fn = 0;
        intptr_t remote_pad = 0;
        uint8_t pad_state[SHELLUI_PAD_DATA_SIZE];
        memset(pad_state, 0, sizeof(pad_state));

        if (get_lib(target, "libSceLibcInternal", &libc_handle) == 0) {
            malloc_fn = resolve_sym(target, libc_handle, "malloc");
            free_fn = resolve_sym(target, libc_handle, "free");
        }
        if (read_state && malloc_fn) {
            remote_pad = (intptr_t)pt_call(
                target, malloc_fn, trap, sizeof(pad_state), 0, 0, 0, 0, 0);
        }
        report_printf(
            report_fd,
            "read_state_address=0x%lx\n"
            "get_controller_info_address=0x%lx\n"
            "remote_pad_address=0x%lx\n",
            (unsigned long)read_state,
            (unsigned long)get_controller_info,
            (unsigned long)remote_pad);
        if (get_controller_info && remote_pad > 0 &&
            pt_io_write(
                target, remote_pad, pad_state, sizeof(pad_state)) == 0) {
            int32_t info_result = (int32_t)pt_call(
                target, get_controller_info, trap,
                (uint32_t)pad_handle, (uint64_t)remote_pad,
                0, 0, 0, 0);
            report_printf(
                report_fd, "controller_info_result=0x%08x\n",
                (uint32_t)info_result);
            if (mdbg_copyout(
                    target, remote_pad, pad_state,
                    sizeof(pad_state)) == 0) {
                report_printf(report_fd, "controller_info_prefix=");
                for (unsigned byte = 0; byte < 128; ++byte)
                    report_printf(report_fd, "%02x", pad_state[byte]);
                report_printf(report_fd, "\n");
            } else {
                report_printf(
                    report_fd,
                    "controller_info_copyout_error=%d\n", errno);
            }
            memset(pad_state, 0, sizeof(pad_state));

            int32_t type2_handle = (int32_t)pt_call(
                target, get_handle, trap,
                (uint32_t)user_id, 2, 0, 0, 0, 0);
            report_printf(
                report_fd, "type2_probe_handle=0x%08x\n",
                (uint32_t)type2_handle);
            if (type2_handle >= 0 && type2_handle != pad_handle &&
                pt_io_write(
                    target, remote_pad, pad_state,
                    sizeof(pad_state)) == 0) {
                int32_t type2_info_result = (int32_t)pt_call(
                    target, get_controller_info, trap,
                    (uint32_t)type2_handle, (uint64_t)remote_pad,
                    0, 0, 0, 0);
                report_printf(
                    report_fd, "type2_controller_info_result=0x%08x\n"
                    "type2_controller_info_prefix=",
                    (uint32_t)type2_info_result);
                if (mdbg_copyout(
                        target, remote_pad, pad_state,
                        sizeof(pad_state)) == 0) {
                    for (unsigned byte = 0; byte < 128; ++byte)
                        report_printf(report_fd, "%02x", pad_state[byte]);
                } else {
                    report_printf(report_fd, "copyout_error:%d", errno);
                }
                report_printf(report_fd, "\n");
                memset(pad_state, 0, sizeof(pad_state));
            }
        }
        if (read_state && remote_pad > 0 &&
            pt_io_write(
                target, remote_pad, pad_state, sizeof(pad_state)) == 0) {
            int32_t read_result = (int32_t)pt_call(
                target, read_state, trap,
                (uint32_t)pad_handle, (uint64_t)remote_pad,
                0, 0, 0, 0);
            report_printf(
                report_fd, "read_state_result=0x%08x\n",
                (uint32_t)read_result);
            if (read_result == 0 &&
                mdbg_copyout(
                    target, remote_pad, pad_state,
                    sizeof(pad_state)) == 0) {
                uint32_t buttons = 0;
                uint64_t timestamp = 0;
                memcpy(&buttons, pad_state, sizeof(buttons));
                memcpy(&timestamp, pad_state + 80, sizeof(timestamp));
                report_printf(
                    report_fd,
                    "pad_connected=%u\npad_buttons=0x%08x\n"
                    "pad_ls=%u,%u\npad_rs=%u,%u\n"
                    "pad_l2=%u\npad_r2=%u\npad_timestamp=%llu\n"
                    "pad_prefix=",
                    pad_state[76], buttons,
                    pad_state[4], pad_state[5],
                    pad_state[6], pad_state[7],
                    pad_state[8], pad_state[9],
                    (unsigned long long)timestamp);
                for (unsigned byte = 0; byte < 96; ++byte)
                    report_printf(report_fd, "%02x", pad_state[byte]);
                report_printf(report_fd, "\n");
            }
        }
        if (remote_pad > 0 && free_fn) {
            (void)pt_call(
                target, free_fn, trap,
                (uint64_t)remote_pad, 0, 0, 0, 0, 0);
        }
        result = 1;
        goto cleanup_native_compat;
    }

    int enable = action == DS4TOD5_NATIVE_COMPAT_ENABLE ? 1 : 0;
    compat_result = (int32_t)pt_call(
        target, function, trap,
        (uint32_t)pad_handle, (uint32_t)enable, 0, 0, 0, 0);
    report_printf(
        report_fd, "enable=%d\ncompat_result=0x%08x\n",
        enable, (uint32_t)compat_result);
    klog_printf(
        "[DS4toDS5] scePadEnablePs4CompatibleMode(handle=%d,enable=%d) "
        "-> 0x%08x\n", pad_handle, enable, (uint32_t)compat_result);

    is_ds4_after = (int32_t)pt_call(
        target, is_ds4, trap, (uint32_t)pad_handle, 0, 0, 0, 0, 0);
    report_printf(report_fd, "is_ds4_after=%d\n", is_ds4_after);

    if (mdbg_copyout(
            target, compat_flags_address, &compat_flags_after,
            sizeof(compat_flags_after)) != 0) {
        report_printf(
            report_fd, "error=compat_flags_after errno=%d\n", errno);
        compat_flags_after = compat_flags_before;
    }
    report_printf(
        report_fd, "compat_flags_after=0x%08x\n",
        compat_flags_after);

    int mode_bit_matches =
        (int)((compat_flags_after >> 31) & 1u) == enable;
    report_printf(
        report_fd, "mode_bit_matches=%d\n", mode_bit_matches);

    if (enable && (compat_result != 0 || !mode_bit_matches)) {
        int32_t rollback_result = (int32_t)pt_call(
            target, function, trap,
            (uint32_t)pad_handle, 0, 0, 0, 0, 0);
        uint32_t rollback_flags = compat_flags_after;
        (void)mdbg_copyout(
            target, compat_flags_address, &rollback_flags,
            sizeof(rollback_flags));
        report_printf(
            report_fd,
            "automatic_rollback=1\nrollback_result=0x%08x\n"
            "rollback_flags=0x%08x\n",
            (uint32_t)rollback_result, rollback_flags);
    }
    result = compat_result == 0 && mode_bit_matches ? 1 :
             (compat_result != 0 ? compat_result : 0);

cleanup_native_compat:
    if (trap_saved && trap_writable && trap_modified)
        (void)pt_io_write(
            target, trap, &original_trap_byte,
            sizeof(original_trap_byte));
    if (trap_writable)
        (void)kernel_set_vmem_protection(
            target, trap, 16, original_protection);
    if (attached)
        (void)sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);

native_compat_done:
    report_printf(report_fd, "result=%d\n", result);
    if (report_fd >= 0)
        close(report_fd);
    return result;
#endif
}

int
shellui_pad_remote_probe(int32_t userId, unsigned samples,
                         unsigned interval_us)
{
#if !defined(__PROSPERO__)
    (void)userId;
    (void)samples;
    (void)interval_us;
    return -1;
#else
    pid_t pids[8];
    size_t count = find_pids("SceRemotePlay", pids, 8);
    if (count == 0) {
        klog_printf("[DS4toDS5] remote probe: SceRemotePlay not found\n");
        return -1;
    }

    if (samples == 0) samples = 1;
    if (samples > 200) samples = 200;
    if (interval_us < 10000) interval_us = 10000;
    if (interval_us > 250000) interval_us = 250000;

    pid_t target = pids[0];
    int attached = 0;
    int successes = 0;
    int32_t handle = -1;
    intptr_t trap_mem = 0;
    intptr_t remote_pad = 0;
    uint8_t original_trap_byte = 0;
    int trap_saved = 0;
    int trap_writable = 0;

    klog_printf("[DS4toDS5] remote probe: PT_ATTACH SceRemotePlay pid=%d user=0x%08x\n",
                target, (uint32_t)userId);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[DS4toDS5] remote probe: PT_ATTACH failed errno=%d\n", errno);
        return -1;
    }
    attached = 1;
    waitpid(target, NULL, 0);

    uint32_t libpad_h = 0;
    uint32_t liblibc_h = 0;
    get_lib(target, "libScePad", &libpad_h);
    get_lib(target, "libSceLibcInternal", &liblibc_h);

    intptr_t fn_open = libpad_h
        ? resolve_sym(target, libpad_h, "scePadOpen") : 0;
    intptr_t fn_gethandle = libpad_h
        ? resolve_sym(target, libpad_h, "scePadGetHandle") : 0;
    intptr_t fn_close = libpad_h
        ? resolve_sym(target, libpad_h, "scePadClose") : 0;
    intptr_t fn_readstate = libpad_h
        ? resolve_sym(target, libpad_h, "scePadReadState") : 0;
    intptr_t fn_setpriv = libpad_h
        ? resolve_sym(target, libpad_h, "scePadSetProcessPrivilege") : 0;
    intptr_t fn_malloc = liblibc_h
        ? resolve_sym(target, liblibc_h, "malloc") : 0;
    intptr_t fn_free = liblibc_h
        ? resolve_sym(target, liblibc_h, "free") : 0;
    trap_mem = libpad_h ? kernel_dynlib_fini_addr(target, libpad_h) : 0;
    if (!trap_mem && libpad_h)
        trap_mem = kernel_dynlib_init_addr(target, libpad_h);

    klog_printf("[DS4toDS5] remote probe: get=0x%lx open=0x%lx read=0x%lx "
                "malloc=0x%lx free=0x%lx trap=0x%lx\n",
                fn_gethandle, fn_open, fn_readstate, fn_malloc, fn_free,
                trap_mem);
    if ((!fn_gethandle && !fn_open) || !fn_readstate || !fn_malloc ||
        !trap_mem)
        goto cleanup;

    if (mdbg_copyout(target, trap_mem, &original_trap_byte, 1) != 0) {
        klog_printf("[DS4toDS5] remote probe: save trap byte failed errno=%d\n", errno);
        goto cleanup;
    }
    trap_saved = 1;

    if (kernel_set_vmem_protection(target, trap_mem, 16,
                                   PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        klog_printf("[DS4toDS5] remote probe: trap RWX failed errno=%d\n", errno);
        goto cleanup;
    }
    trap_writable = 1;

    {
        uint8_t int3 = 0xcc;
        if (pt_io_write(target, trap_mem, &int3, 1) != 0) {
            klog_printf("[DS4toDS5] remote probe: trap write failed errno=%d\n", errno);
            goto cleanup;
        }
    }

    if (fn_setpriv) {
        int64_t pr = pt_call(target, fn_setpriv, trap_mem, 1, 0, 0, 0, 0, 0);
        klog_printf("[DS4toDS5] remote probe: scePadSetProcessPrivilege(1)=0x%llx\n",
                    (unsigned long long)(uint64_t)pr);
    }

    remote_pad = (intptr_t)pt_call(target, fn_malloc, trap_mem,
                                   256, 0, 0, 0, 0, 0);
    klog_printf("[DS4toDS5] remote probe: malloc(256)=0x%lx\n",
                remote_pad);
    if (remote_pad <= 0)
        goto cleanup;

    if (fn_gethandle) {
        int64_t result = pt_call(target, fn_gethandle, trap_mem,
                                 (uint32_t)userId, 0, 0, 0, 0, 0);
        klog_printf("[DS4toDS5] remote probe: scePadGetHandle(0x%08x,0,0)=0x%llx\n",
                    (uint32_t)userId,
                    (unsigned long long)(uint64_t)result);
        if ((int32_t)result >= 0)
            handle = (int32_t)result;
    }

    if (handle < 0 && fn_open) {
        int32_t candidates[4] = {
            userId, 0x10000000, 1, (int32_t)0xffffffffu
        };
        for (unsigned i = 0; i < 4 && handle < 0; i++) {
            int64_t result = pt_call(target, fn_open, trap_mem,
                                     (uint32_t)candidates[i], 0, 0, 0, 0, 0);
            klog_printf("[DS4toDS5] remote probe: scePadOpen(0x%08x,0,0)=0x%llx\n",
                        (uint32_t)candidates[i],
                        (unsigned long long)(uint64_t)result);
            if ((int32_t)result >= 0) {
                handle = (int32_t)result;
            }
        }
    }
    if (handle < 0)
        goto cleanup;

    for (unsigned sample = 0; sample < samples; sample++) {
        uint8_t pad[256];
        memset(pad, 0, sizeof(pad));

        if (pt_io_write(target, remote_pad, pad, sizeof(pad)) != 0) {
            klog_printf("[DS4toDS5] PAD #%u zero remote buffer failed errno=%d\n",
                        sample, errno);
            break;
        }

        int64_t rr = pt_call(target, fn_readstate, trap_mem,
                             (uint32_t)handle, (uint64_t)remote_pad,
                             0, 0, 0, 0);
        if ((int32_t)rr == 0 &&
            mdbg_copyout(target, remote_pad, pad, sizeof(pad)) != 0) {
            klog_printf("[DS4toDS5] PAD #%u copyout failed errno=%d\n",
                        sample, errno);
            break;
        }
        if ((int32_t)rr == 0) {
            uint32_t buttons = 0;
            uint64_t timestamp = 0;
            memcpy(&buttons, pad + 0, sizeof(buttons));
            memcpy(&timestamp, pad + 80, sizeof(timestamp));
            successes++;
            klog_printf("[DS4toDS5] PAD #%u conn=%u btn=0x%08x "
                        "LS=%3u,%3u RS=%3u,%3u L2=%3u R2=%3u ts=%llu\n",
                        sample, pad[76], buttons,
                        pad[4], pad[5], pad[6], pad[7], pad[8], pad[9],
                        (unsigned long long)timestamp);
        } else {
            klog_printf("[DS4toDS5] PAD #%u read ret=0x%llx errno=%d\n",
                        sample, (unsigned long long)(uint64_t)rr, errno);
            break;
        }
        usleep(interval_us);
    }

cleanup:
    /*
     * Reusing this process-level pad handle is intentional. Firmware 11.60
     * rejected scePadClose for the physical handle after an interrupted call.
     * SceRemotePlay is persistent, so later probes discover the existing
     * handle with scePadGetHandle.
     */
    (void)fn_close;
    if (remote_pad > 0 && fn_free && trap_mem)
        (void)pt_call(target, fn_free, trap_mem,
                      (uint64_t)remote_pad, 0, 0, 0, 0, 0);
    if (trap_saved && trap_writable)
        (void)pt_io_write(target, trap_mem, &original_trap_byte, 1);
    if (trap_writable)
        (void)kernel_set_vmem_protection(target, trap_mem, 16,
                                         PROT_READ | PROT_EXEC);
    if (attached)
        (void)sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);

    klog_printf("[DS4toDS5] remote probe complete: %d/%u samples\n",
                successes, samples);
    return successes > 0 ? successes : -1;
#endif
}

int
shellui_pad_disconnect_device(uint64_t physicalDeviceId)
{
    pid_t pids[4];
    if (find_pids("SceShellUI", pids, 4) == 0) {
        klog_printf("[Ghostpad] disconnect: SceShellUI not found, using direct fallback\n");
        goto fallback;
    }
    pid_t target = pids[0];
    klog_printf("[Ghostpad] disconnect: PT_ATTACH(SceShellUI pid=%d) dev=0x%llx\n",
                target, (unsigned long long)physicalDeviceId);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[Ghostpad] disconnect: PT_ATTACH failed errno=%d, using direct fallback\n", errno);
        goto fallback;
    }
    waitpid(target, NULL, 0);

    uint32_t mbus_h = 0;
    get_lib(target, "libSceMbus", &mbus_h);
    intptr_t fn_disc = mbus_h ? resolve_sym(target, mbus_h, "sceMbusDisconnectDevice") : 0;
    klog_printf("[Ghostpad] disconnect: sceMbusDisconnectDevice @ 0x%lx\n", fn_disc);

    uint32_t libpad_h = 0;
    get_lib(target, "libScePad", &libpad_h);
    intptr_t trap_mem = libpad_h ? kernel_dynlib_init_addr(target, libpad_h) : 0;
    if (!trap_mem && libpad_h) trap_mem = kernel_dynlib_fini_addr(target, libpad_h);

    if (!fn_disc || !trap_mem) {
        klog_printf("[Ghostpad] disconnect: symbol/cave fail\n");
        sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
        return -1;
    }
    kernel_set_vmem_protection(target, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target, trap_mem, &int3, 1);

    int64_t ret = pt_call(target, fn_disc, trap_mem,
                          (uint64_t)physicalDeviceId, 0, 0, 0, 0, 0);
    klog_printf("[Ghostpad] disconnect: sceMbusDisconnectDevice(0x%llx) -> %lld\n",
                (unsigned long long)physicalDeviceId, (long long)ret);
    sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
    return (ret == 0) ? 0 : (int)ret;

fallback:
#ifdef __PROSPERO__
    klog_printf("[Ghostpad] disconnect: direct fallback not supported on PS5\n");
    return -1;
#else
    {
        klog_printf("[Ghostpad] disconnect: executing direct fallback for 0x%llx\n", (unsigned long long)physicalDeviceId);
        void *h = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_LAZY);
        if (!h) {
            klog_printf("[Ghostpad] disconnect: direct fallback failed to dlopen libSceMbus.sprx\n");
            return -1;
        }
        typedef int (*fn_disconnect)(uint64_t);
        fn_disconnect f = (fn_disconnect)dlsym(h, "sceMbusDisconnectDevice");
        if (!f) {
            klog_printf("[Ghostpad] disconnect: direct fallback failed to dlsym sceMbusDisconnectDevice\n");
            dlclose(h);
            return -1;
        }
        /* Elevate to SceShellCore credentials for direct system service call */
        pid_t mypid = getpid();
        uint64_t saved_authid = kernel_get_ucred_authid(mypid);
        if (saved_authid) {
            kernel_set_ucred_authid(mypid, 0x4800000000000010l);
        }
        int r = f(physicalDeviceId);
        klog_printf("[Ghostpad] disconnect: direct sceMbusDisconnectDevice(0x%llx) returned %d\n",
                    (unsigned long long)physicalDeviceId, r);
        if (saved_authid) {
            kernel_set_ucred_authid(mypid, saved_authid);
        }
        dlclose(h);
        return r;
    }
#endif

}

int
shellui_pad_disconnect_first_physical_candidate(uint64_t skipDeviceId, uint64_t *outDeviceId)
{
    if (outDeviceId)
        *outDeviceId = 0;

    pid_t pids[4];
    if (find_pids("SceShellUI", pids, 4) == 0) {
        klog_printf("[Ghostpad] sweep_disconnect: SceShellUI not found\n");
        return -1;
    }
    pid_t target = pids[0];
    klog_printf("[Ghostpad] sweep_disconnect: PT_ATTACH(SceShellUI pid=%d) skip=0x%llx\n",
                target, (unsigned long long)skipDeviceId);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[Ghostpad] sweep_disconnect: PT_ATTACH failed errno=%d\n", errno);
        return -1;
    }
    waitpid(target, NULL, 0);

    uint32_t mbus_h = 0;
    get_lib(target, "libSceMbus", &mbus_h);
    intptr_t fn_disc = mbus_h ? resolve_sym(target, mbus_h, "sceMbusDisconnectDevice") : 0;

    uint32_t libpad_h = 0;
    get_lib(target, "libScePad", &libpad_h);
    intptr_t trap_mem = libpad_h ? kernel_dynlib_init_addr(target, libpad_h) : 0;
    if (!trap_mem && libpad_h)
        trap_mem = kernel_dynlib_fini_addr(target, libpad_h);

    if (!fn_disc || !trap_mem) {
        klog_printf("[Ghostpad] sweep_disconnect: symbol/cave fail fn=0x%lx trap=0x%lx\n",
                    fn_disc, trap_mem);
        sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
        return -1;
    }
    kernel_set_vmem_protection(target, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target, trap_mem, &int3, 1);

    int calls = 0;
    for (uint32_t prefix = 0; prefix <= 0xff; prefix++) {
        uint64_t dev = ((uint64_t)prefix << 16) | 0x0300u;
        if (!dev || dev == skipDeviceId)
            continue;
        calls++;
        int64_t ret = pt_call(target, fn_disc, trap_mem, dev, 0, 0, 0, 0, 0);
        if (ret == 0) {
            if (outDeviceId)
                *outDeviceId = dev;
            klog_printf("[Ghostpad] sweep_disconnect: sceMbusDisconnectDevice(0x%llx) -> 0 after %d calls\n",
                        (unsigned long long)dev, calls);
            sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
            return 0;
        }
    }

    klog_printf("[Ghostpad] sweep_disconnect: no physical 0x..0300 device disconnected after %d calls\n",
                calls);
    sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
    return -1;
}

int
shellui_pad_user_has_handle(int32_t userId, int32_t observedHandle)
{
    if (observedHandle < 0)
        return 0;

    pid_t pids[4];
    if (find_pids("SceShellUI", pids, 4) == 0) {
        klog_printf("[Ghostpad] handle_user: SceShellUI not found\n");
        return -1;
    }
    pid_t target = pids[0];
    klog_printf("[Ghostpad] handle_user: PT_ATTACH(SceShellUI pid=%d) user=0x%x observed=0x%x\n",
                target, (uint32_t)userId, (uint32_t)observedHandle);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[Ghostpad] handle_user: PT_ATTACH failed errno=%d\n", errno);
        return -1;
    }
    waitpid(target, NULL, 0);

    uint32_t libpad_h = 0;
    get_lib(target, "libScePad", &libpad_h);
    intptr_t fn_gethandle = libpad_h ? resolve_sym(target, libpad_h, "scePadGetHandle") : 0;
    intptr_t trap_mem = libpad_h ? kernel_dynlib_init_addr(target, libpad_h) : 0;
    if (!trap_mem && libpad_h)
        trap_mem = kernel_dynlib_fini_addr(target, libpad_h);

    if (!fn_gethandle || !trap_mem) {
        klog_printf("[Ghostpad] handle_user: symbol/cave fail GH=0x%lx trap=0x%lx\n",
                    fn_gethandle, trap_mem);
        sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
        return -1;
    }
    kernel_set_vmem_protection(target, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target, trap_mem, &int3, 1);

    int match = 0;
    const int types[] = {0, 3, 16};
    for (int t = 0; t < (int)(sizeof(types) / sizeof(types[0])) && !match; t++) {
        for (int idx = 0; idx < 8 && !match; idx++) {
            int64_t gh = pt_call(target, fn_gethandle, trap_mem,
                                 (uint64_t)(uint32_t)userId,
                                 (uint64_t)types[t], (uint64_t)idx,
                                 0, 0, 0);
            if ((int32_t)gh >= 0) {
                klog_printf("[Ghostpad] handle_user: GH(0x%x,t=%d,i=%d)->0x%llx\n",
                            (uint32_t)userId, types[t], idx,
                            (unsigned long long)(uint64_t)gh);
                if ((uint32_t)gh == (uint32_t)observedHandle)
                    match = 1;
            }
        }
    }

    klog_printf("[Ghostpad] handle_user: observed=0x%x user=0x%x match=%d\n",
                (uint32_t)observedHandle, (uint32_t)userId, match);
    sys_ptrace(PT_DETACH, target, (caddr_t)1, 0);
    return match;
}

int
shellui_pad_force_bind(uint64_t virtualDeviceId, int32_t userId)
{
    pid_t pids[16];
    size_t count = find_pids("SceShellUI", pids, 16);
    if (count == 0) {
        klog_printf("[Ghostpad] force_bind: SceShellUI not found, using direct fallback\n");
        goto fallback;
    }
    pid_t target_pid = pids[0];

    klog_printf("[Ghostpad] force_bind: PT_ATTACH(SceShellUI pid=%d) dev=0x%llx user=0x%x\n",
                target_pid, (unsigned long long)virtualDeviceId, (uint32_t)userId);
    if (sys_ptrace(PT_ATTACH, target_pid, 0, 0) != 0) {
        klog_printf("[Ghostpad] force_bind: PT_ATTACH failed errno=%d, using direct fallback\n", errno);
        goto fallback;
    }
    waitpid(target_pid, NULL, 0);

    uint32_t libmbus_h = 0;
    get_lib(target_pid, "libSceMbus", &libmbus_h);
    if (!libmbus_h) {
        klog_printf("[Ghostpad] force_bind: libSceMbus not found in SceShellUI\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }

    intptr_t fn_bind = resolve_sym(target_pid, libmbus_h, "sceMbusBindDeviceWithUserId");
    klog_printf("[Ghostpad] force_bind: sceMbusBindDeviceWithUserId @ 0x%lx\n", fn_bind);
    if (!fn_bind) {
        klog_printf("[Ghostpad] force_bind: symbol not found\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }

    /* Need a code cave for the INT3 trap.  Use libScePad init/fini if available. */
    uint32_t libpad_h = 0;
    get_lib(target_pid, "libScePad", &libpad_h);
    intptr_t trap_mem = libpad_h ? kernel_dynlib_init_addr(target_pid, libpad_h) : 0;
    if (!trap_mem && libpad_h) trap_mem = kernel_dynlib_fini_addr(target_pid, libpad_h);
    if (!trap_mem) {
        klog_printf("[Ghostpad] force_bind: no code cave\n");
        sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
        return -1;
    }
    kernel_set_vmem_protection(target_pid, trap_mem, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint8_t int3 = 0xCC;
    pt_io_write(target_pid, trap_mem, &int3, 1);

    /* sceMbusBindDeviceWithUserId(uint64_t deviceId, uint32_t userId) */
    int64_t ret = pt_call(target_pid, fn_bind, trap_mem,
                          (uint64_t)virtualDeviceId, (uint64_t)(uint32_t)userId,
                          0, 0, 0, 0);
    klog_printf("[Ghostpad] force_bind: sceMbusBindDeviceWithUserId(0x%llx, 0x%x) -> %lld\n",
                (unsigned long long)virtualDeviceId, (uint32_t)userId, (long long)ret);

    sys_ptrace(PT_DETACH, target_pid, (caddr_t)1, 0);
    return (ret == 0) ? 0 : (int)ret;

fallback:
#ifdef __PROSPERO__
    klog_printf("[Ghostpad] force_bind: direct fallback not supported on PS5\n");
    return -1;
#else
    {
        klog_printf("[Ghostpad] force_bind: executing direct fallback for 0x%llx -> 0x%x\n",
                    (unsigned long long)virtualDeviceId, (uint32_t)userId);
        void *h = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_LAZY);
        if (!h) {
            klog_printf("[Ghostpad] force_bind: direct fallback failed to dlopen libSceMbus.sprx\n");
            return -1;
        }
        typedef int (*fn_bind)(uint64_t, uint32_t);
        fn_bind f = (fn_bind)dlsym(h, "sceMbusBindDeviceWithUserId");
        if (!f) {
            klog_printf("[Ghostpad] force_bind: direct fallback failed to dlsym sceMbusBindDeviceWithUserId\n");
            dlclose(h);
            return -1;
        }
        /* Elevate to SceShellCore credentials for direct system service call */
        pid_t mypid = getpid();
        uint64_t saved_authid = kernel_get_ucred_authid(mypid);
        if (saved_authid) {
            kernel_set_ucred_authid(mypid, 0x4800000000000010l);
        }
        int r = f(virtualDeviceId, (uint32_t)userId);
        klog_printf("[Ghostpad] force_bind: direct sceMbusBindDeviceWithUserId(0x%llx, 0x%x) returned %d\n",
                    (unsigned long long)virtualDeviceId, (uint32_t)userId, r);
        if (saved_authid) {
            kernel_set_ucred_authid(mypid, saved_authid);
        }
        dlclose(h);
        return r;
    }
#endif

}

/* shellui_pad_relaunch_stub_with_handle — re-launch stub with known VDI handle.
 * Stub takes fast path (no VDA); subsequent updates use mdbg_copyin (no lag). */
int
shellui_pad_relaunch_stub_with_handle(int32_t handle)
{
    if (!g_relaunch_stub_fn || !g_relaunch_pthread_fn || g_relaunch_pid < 0) {
        klog_printf("[Ghostpad] relaunch: no injection state saved — inject first\n");
        return -1;
    }

    pid_t    pid       = g_relaunch_pid;
    intptr_t args_addr = g_relaunch_args_kaddr;

    klog_printf("[Ghostpad] relaunch: pid=%d handle=%d args=0x%lx\n",
                pid, handle, args_addr);

    /* PT_ATTACH, write args via PT_IO, launch stub thread */
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) {
        klog_printf("[Ghostpad] relaunch: PT_ATTACH failed errno=%d\n", errno);
        return -1;
    }
    waitpid(pid, NULL, 0);

    /* Allocate fresh heap in SceShellCore for shellui_stub via pt_call(malloc) */
    size_t reg_stub_len = (size_t)((uintptr_t)shellui_stub_end - (uintptr_t)shellui_stub);
    intptr_t new_stub_block = 0;
    intptr_t new_trap_rip   = g_relaunch_trap_rip;  /* fallback: original trap */
    intptr_t new_stub_fn    = g_relaunch_stub_fn;   /* fallback: original cave (unsafe) */

    if (g_relaunch_malloc_fn) {
        new_stub_block = (intptr_t)pt_call(pid, g_relaunch_malloc_fn, g_relaunch_trap_rip,
                                            (uint64_t)(reg_stub_len + 32), 0, 0, 0, 0, 0);
        klog_printf("[Ghostpad] relaunch: malloc(%zu) -> 0x%lx\n", reg_stub_len+32, new_stub_block);
    }

    if (new_stub_block) {
        kernel_set_vmem_protection(pid, new_stub_block, reg_stub_len + 32,
                                   PROT_READ | PROT_WRITE | PROT_EXEC);

        uint8_t int3 = 0xCC;
        pt_io_write(pid, new_stub_block, &int3, 1);         /* INT3 trap at offset 0 */

        if (!pt_io_write(pid, new_stub_block + 16, shellui_stub, reg_stub_len)) {
            klog_printf("[Ghostpad] relaunch: shellui_stub (%zu bytes) -> 0x%lx ok\n",
                        reg_stub_len, new_stub_block + 16);
            new_trap_rip = new_stub_block;
            new_stub_fn  = new_stub_block + 16;
        } else {
            klog_printf("[Ghostpad] relaunch: shellui_stub write failed errno=%d\n", errno);
            new_stub_block = 0;
        }
    }

    if (!new_stub_block) {
        klog_printf("[Ghostpad] relaunch: no new block — cannot safely run shellui_stub\n");
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -1;
    }

    /* Write handle + reset args */
    int32_t v32;
    v32 = handle;
    if (pt_io_write(pid, args_addr + (intptr_t)offsetof(ShellUiPadArgs, pad_handle), &v32, 4)) {
        klog_printf("[Ghostpad] relaunch: PT_IO write handle failed errno=%d\n", errno);
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -1;
    }
    klog_printf("[Ghostpad] relaunch: PT_IO pad_handle=%d ok\n", handle);

    v32 = 0;
    pt_io_write(pid, args_addr + (intptr_t)offsetof(ShellUiPadArgs, ready), &v32, 4);
    pt_io_write(pid, args_addr + (intptr_t)offsetof(ShellUiPadArgs, stop),  &v32, 4);

    /* seq=1: use_insert hint, fp_vda=NULL: skip VDA (device already bound) */
    v32 = 1;
    pt_io_write(pid, args_addr + (intptr_t)offsetof(ShellUiPadArgs, seq), &v32, 4);

    intptr_t null_fn = 0;
    pt_io_write(pid, args_addr + (intptr_t)offsetof(ShellUiPadArgs, fp_vda),
                &null_fn, sizeof(null_fn));

    /* Launch stub thread with correctly-sized allocation */
    int64_t pret = pt_call(pid, g_relaunch_pthread_fn, new_trap_rip,
                           (uint64_t)g_relaunch_thread_storage, 0,
                           (uint64_t)new_stub_fn,
                           (uint64_t)g_relaunch_args_kaddr, 0, 0);
    klog_printf("[Ghostpad] relaunch: pthread_create -> %lld\n", (long long)pret);

    /* PT_DETACH so SceShellCore resumes and the stub thread can run */
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    klog_printf("[Ghostpad] relaunch: SceShellCore detached — stub thread running\n");

    return (pret == 0) ? 0 : -1;
}

/*
 * =====================================================================================
 *            HOOK SceShellCore scePadGetHandle TO BYPASS PID IPC CHECK
 * =====================================================================================
 */

int
shellui_pad_hook_gethandle(void)
{
    pid_t pids[8];
    if (find_pids("SceShellCore", pids, 8) == 0) {
        klog_printf("[Ghostpad] hook_gh: SceShellCore not found\n");
        return -1;
    }
    pid_t target = pids[0];

    pid_t mypid = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(mypid);
    if (saved_authid) {
        kernel_set_ucred_authid(mypid, 0x4800000000010003l);
    }

    uint32_t libpad_h = 0;
    if (get_lib(target, "libScePad", &libpad_h)) {
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    intptr_t fn_gethandle = resolve_sym(target, libpad_h, "scePadGetHandle");
    intptr_t fn_vdi = resolve_sym(target, libpad_h, "scePadVirtualDeviceInsertData");
    intptr_t fn_vda = resolve_sym(target, libpad_h, "scePadVirtualDeviceAddDevice");

    if (!fn_gethandle || !fn_vdi || !fn_vda) {
        klog_printf("[Ghostpad] hook_gh: symbols not found (gh=%p, vdi=%p, vda=%p)\n",
                    (void *)fn_gethandle, (void *)fn_vdi, (void *)fn_vda);
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    /* Read original 5 bytes of scePadGetHandle */
    if (mdbg_copyout(target, fn_gethandle, g_orig_gethandle, 5) != 0) {
        klog_printf("[Ghostpad] hook_gh: failed to read original 5 bytes of gethandle\n");
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }
    g_gethandle_hooked = 1;

    /* Read original 128 bytes of scePadVirtualDeviceInsertData if not already done */
    if (!g_vdi_hooked) {
        if (mdbg_copyout(target, fn_vdi, g_orig_vdi_128, 128) == 0) {
            g_vdi_hooked = 1;
            klog_printf("[Ghostpad] hook_gh: captured original 128 bytes of vdi\n");
        } else {
            klog_printf("[Ghostpad] hook_gh: failed to read original 128 bytes of vdi\n");
        }
    }

    klog_printf("[Ghostpad] hook_gh: original gethandle bytes: %02x %02x %02x %02x %02x\n",
                g_orig_gethandle[0], g_orig_gethandle[1], g_orig_gethandle[2], g_orig_gethandle[3], g_orig_gethandle[4]);


    /* Construct 128-byte hook block */
    uint8_t hook[128];
    memset(hook, 0x90, sizeof(hook)); // pad with NOPs

    /* 1. cmp edi, 0xdeadbeef */
    hook[0] = 0x81; hook[1] = 0xFF;
    hook[2] = 0xEF; hook[3] = 0xBE; hook[4] = 0xAD; hook[5] = 0xDE;

    /* 2. jne +0x60 (trampoline at offset 104) */
    hook[6] = 0x75; hook[7] = 0x60;

    /* 3. push rbx */
    hook[8] = 0x53;
    /* 4. mov ebx, esi */
    hook[9] = 0x89; hook[10] = 0xF3;

    /* 5. push registers to preserve state */
    hook[11] = 0x57; // push rdi
    hook[12] = 0x56; // push rsi
    hook[13] = 0x52; // push rdx
    hook[14] = 0x51; // push rcx
    hook[15] = 0x41; hook[16] = 0x50; // push r8
    hook[17] = 0x41; hook[18] = 0x51; // push r9
    hook[19] = 0x41; hook[20] = 0x52; // push r10
    hook[21] = 0x41; hook[22] = 0x53; // push r11

    /* 6. sub rsp, 40 */
    hook[23] = 0x48; hook[24] = 0x83; hook[25] = 0xEC; hook[26] = 0x28;

    /* 7. mov dword ptr [rsp], 32 (vd_param.size) */
    hook[27] = 0xC7; hook[28] = 0x04; hook[29] = 0x24;
    hook[30] = 32; hook[31] = 0; hook[32] = 0; hook[33] = 0;

    /* 8. mov dword ptr [rsp+4], 0x10000000 (vd_param.userId) */
    hook[34] = 0xC7; hook[35] = 0x44; hook[36] = 0x24; hook[37] = 0x04;
    hook[38] = 0x00; hook[39] = 0x00; hook[40] = 0x00; hook[41] = 0x10;

    /* 9. mov qword ptr [rsp+8], 0 */
    hook[42] = 0x48; hook[43] = 0xC7; hook[44] = 0x44; hook[45] = 0x24; hook[46] = 0x08;
    hook[47] = 0; hook[48] = 0; hook[49] = 0; hook[50] = 0;

    /* 10. mov qword ptr [rsp+16], 0 */
    hook[51] = 0x48; hook[52] = 0xC7; hook[53] = 0x44; hook[54] = 0x24; hook[55] = 0x10;
    hook[56] = 0; hook[57] = 0; hook[58] = 0; hook[59] = 0;

    /* 11. mov qword ptr [rsp+24], 0 */
    hook[60] = 0x48; hook[61] = 0xC7; hook[62] = 0x44; hook[63] = 0x24; hook[64] = 0x18;
    hook[65] = 0; hook[66] = 0; hook[67] = 0; hook[68] = 0;

    /* 12. mov rdi, rsp */
    hook[69] = 0x48; hook[70] = 0x89; hook[71] = 0xE7;
    /* 13. mov esi, ebx */
    hook[72] = 0x89; hook[73] = 0xDE;

    /* 14. mov rax, fn_vda */
    hook[74] = 0x48; hook[75] = 0xB8;
    memcpy(&hook[76], &fn_vda, 8);

    /* 15. call rax */
    hook[84] = 0xFF; hook[85] = 0xD0;

    /* 16. add rsp, 40 */
    hook[86] = 0x48; hook[87] = 0x83; hook[88] = 0xC4; hook[89] = 0x28;

    /* 17. pop r11, r10, r9, r8, rcx, rdx, rsi, rdi, rbx */
    hook[90] = 0x41; hook[91] = 0x5B;
    hook[92] = 0x41; hook[93] = 0x5A;
    hook[94] = 0x41; hook[95] = 0x59;
    hook[96] = 0x41; hook[97] = 0x58;
    hook[98] = 0x59;
    hook[99] = 0x5A;
    hook[100] = 0x5E;
    hook[101] = 0x5F;
    hook[102] = 0x5B;

    /* 18. ret */
    hook[103] = 0xC3;

    /* ---- Trampoline at offset 104 (0x68) ---- */
    /* 1. Copy original 5 bytes of scePadGetHandle */
    memcpy(&hook[104], g_orig_gethandle, 5);

    /* 2. mov rax, fn_gethandle + 5 */
    hook[109] = 0x48; hook[110] = 0xB8;
    intptr_t ret_addr = fn_gethandle + 5;
    memcpy(&hook[111], &ret_addr, 8);

    /* 3. jmp rax */
    hook[119] = 0xFF; hook[120] = 0xE0;

    /* Write the hook block into SceShellCore's scePadVirtualDeviceInsertData */
    if (mdbg_copyin(target, hook, fn_vdi, 128) != 0) {
        klog_printf("[Ghostpad] hook_gh: failed to write hook block to SceShellCore\n");
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    /* Write 5-byte relative jump at scePadGetHandle */
    uint8_t detour[5];
    detour[0] = 0xE9;
    int32_t jmp_rel32 = (int32_t)(fn_vdi - (fn_gethandle + 5));
    memcpy(&detour[1], &jmp_rel32, 4);

    if (mdbg_copyin(target, detour, fn_gethandle, 5) != 0) {
        klog_printf("[Ghostpad] hook_gh: failed to write detour jump to SceShellCore\n");
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    klog_printf("[Ghostpad] hook_gh: scePadGetHandle HOOKED successfully (detour -> %p)\n", (void *)fn_vdi);

    if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
    return 0;
}

/*
 * =====================================================================================
 *            HOOK SceShellCore scePadSetProcessPrivilege FOR IN-PROCESS VDA
 * =====================================================================================
 */
int
shellui_pad_hook_setpriv(void)
{
#if !GHOSTPAD_ALLOW_UNSAFE_SETPRIV_HOOK
    klog_printf("[Ghostpad] hook_sp: disabled by default; compile with -DGHOSTPAD_ALLOW_UNSAFE_SETPRIV_HOOK=1 to enable\n");
    return 0;
#else
    pid_t pids[8];
    if (find_pids("SceShellCore", pids, 8) == 0) {
        klog_printf("[Ghostpad] hook_sp: SceShellCore not found\n");
        return -1;
    }
    pid_t target = pids[0];

    pid_t mypid = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(mypid);
    if (saved_authid) {
        kernel_set_ucred_authid(mypid, 0x4800000000010003l);
    }

    uint32_t libpad_h = 0;
    if (get_lib(target, "libScePad", &libpad_h)) {
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    intptr_t fn_setpriv = resolve_sym(target, libpad_h, "scePadSetProcessPrivilege");
    intptr_t fn_vdi = resolve_sym(target, libpad_h, "scePadVirtualDeviceInsertData");
    intptr_t fn_vda = resolve_sym(target, libpad_h, "scePadVirtualDeviceAddDevice");

    if (!fn_setpriv || !fn_vdi || !fn_vda) {
        klog_printf("[Ghostpad] hook_sp: symbols not found (sp=%p, vdi=%p, vda=%p)\n",
                    (void *)fn_setpriv, (void *)fn_vdi, (void *)fn_vda);
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    /* Read original 5 bytes of scePadSetProcessPrivilege */
    if (mdbg_copyout(target, fn_setpriv, g_orig_setpriv, 5) != 0) {
        klog_printf("[Ghostpad] hook_sp: failed to read original 5 bytes of setpriv\n");
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }
    g_setpriv_hooked = 1;

    /* Read original 128 bytes of scePadVirtualDeviceInsertData if not already done */
    if (!g_vdi_hooked) {
        if (mdbg_copyout(target, fn_vdi, g_orig_vdi_128, 128) == 0) {
            g_vdi_hooked = 1;
            klog_printf("[Ghostpad] hook_sp: captured original 128 bytes of vdi\n");
        } else {
            klog_printf("[Ghostpad] hook_sp: failed to read original 128 bytes of vdi\n");
        }
    }

    klog_printf("[Ghostpad] hook_sp: original setpriv bytes: %02x %02x %02x %02x %02x\n",
                g_orig_setpriv[0], g_orig_setpriv[1], g_orig_setpriv[2], g_orig_setpriv[3], g_orig_setpriv[4]);


    /* Construct 128-byte hook block */
    uint8_t hook[128];
    memset(hook, 0x90, sizeof(hook)); // pad with NOPs

    size_t off = 0;

    /* 1. cmp edi, 0xdeadbeef */
    hook[off++] = 0x81; hook[off++] = 0xFF;
    hook[off++] = 0xEF; hook[off++] = 0xBE; hook[off++] = 0xAD; hook[off++] = 0xDE;

    /* 2. jne displacement (trampoline at offset 104) */
    size_t jne_instr_off = off;
    hook[off++] = 0x75;
    hook[off++] = 0x00; // placeholder for displacement

    /* 3. push registers to preserve state */
    hook[off++] = 0x53; // push rbx
    hook[off++] = 0x55; // push rbp
    hook[off++] = 0x57; // push rdi
    hook[off++] = 0x56; // push rsi
    hook[off++] = 0x52; // push rdx
    hook[off++] = 0x51; // push rcx
    hook[off++] = 0x41; hook[off++] = 0x50; // push r8
    hook[off++] = 0x41; hook[off++] = 0x51; // push r9
    hook[off++] = 0x41; hook[off++] = 0x52; // push r10
    hook[off++] = 0x41; hook[off++] = 0x53; // push r11

    /* 4. sub rsp, 40 (allocate stack frame) */
    hook[off++] = 0x48; hook[off++] = 0x83; hook[off++] = 0xEC; hook[off++] = 0x28;

    /* 5. mov dword ptr [rsp], 32 (vd_param.size) */
    hook[off++] = 0xC7; hook[off++] = 0x04; hook[off++] = 0x24;
    hook[off++] = 32; hook[off++] = 0; hook[off++] = 0; hook[off++] = 0;

    /* 6. mov dword ptr [rsp+4], 0x10000000 (vd_param.userId) */
    hook[off++] = 0xC7; hook[off++] = 0x44; hook[off++] = 0x24; hook[off++] = 0x04;
    hook[off++] = 0x00; hook[off++] = 0x00; hook[off++] = 0x00; hook[off++] = 0x10;

    /* 7. mov qword ptr [rsp+8], 0 */
    hook[off++] = 0x48; hook[off++] = 0xC7; hook[off++] = 0x44; hook[off++] = 0x24; hook[off++] = 0x08;
    hook[off++] = 0; hook[off++] = 0; hook[off++] = 0; hook[off++] = 0;

    /* 8. mov qword ptr [rsp+16], 0 */
    hook[off++] = 0x48; hook[off++] = 0xC7; hook[off++] = 0x44; hook[off++] = 0x24; hook[off++] = 0x10;
    hook[off++] = 0; hook[off++] = 0; hook[off++] = 0; hook[off++] = 0;

    /* 9. mov qword ptr [rsp+24], 0 */
    hook[off++] = 0x48; hook[off++] = 0xC7; hook[off++] = 0x44; hook[off++] = 0x24; hook[off++] = 0x18;
    hook[off++] = 0; hook[off++] = 0; hook[off++] = 0; hook[off++] = 0;

    /* 10. mov rdi, rsp */
    hook[off++] = 0x48; hook[off++] = 0x89; hook[off++] = 0xE7;

    /* 11. mov esi, 3 */
    hook[off++] = 0xBE; hook[off++] = 0x03; hook[off++] = 0x00; hook[off++] = 0x00; hook[off++] = 0x00;

    /* 12. mov rax, fn_vda */
    hook[off++] = 0x48; hook[off++] = 0xB8;
    memcpy(&hook[off], &fn_vda, 8);
    off += 8;

    /* 13. call rax */
    hook[off++] = 0xFF; hook[off++] = 0xD0;

    /* 14. add rsp, 40 */
    hook[off++] = 0x48; hook[off++] = 0x83; hook[off++] = 0xC4; hook[off++] = 0x28;

    /* 15. pop registers to restore state */
    hook[off++] = 0x41; hook[off++] = 0x5B; // pop r11
    hook[off++] = 0x41; hook[off++] = 0x5A; // pop r10
    hook[off++] = 0x41; hook[off++] = 0x59; // pop r9
    hook[off++] = 0x41; hook[off++] = 0x58; // pop r8
    hook[off++] = 0x59; // pop rcx
    hook[off++] = 0x5A; // pop rdx
    hook[off++] = 0x5E; // pop rsi
    hook[off++] = 0x5F; // pop rdi
    hook[off++] = 0x5D; // pop rbp
    hook[off++] = 0x5B; // pop rbx

    /* 16. ret */
    hook[off++] = 0xC3;

    /* Setup trampoline at offset 104 */
    size_t tramp_off = 104;
    hook[jne_instr_off + 1] = (uint8_t)(tramp_off - (jne_instr_off + 2));

    /* Trampoline: original 5 bytes */
    memcpy(&hook[tramp_off], g_orig_setpriv, 5);

    /* Trampoline: mov rax, fn_setpriv + 5 */
    hook[tramp_off + 5] = 0x48; hook[tramp_off + 6] = 0xB8;
    intptr_t ret_addr = fn_setpriv + 5;
    memcpy(&hook[tramp_off + 7], &ret_addr, 8);

    /* Trampoline: jmp rax */
    hook[tramp_off + 15] = 0xFF; hook[tramp_off + 16] = 0xE0;

    /* Write hook block to scePadVirtualDeviceInsertData in SceShellCore */
    if (mdbg_copyin(target, hook, fn_vdi, 128) != 0) {
        klog_printf("[Ghostpad] hook_sp: failed to write hook block to SceShellCore\n");
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    /* Write relative jump in scePadSetProcessPrivilege */
    uint8_t detour[5];
    detour[0] = 0xE9;
    int32_t jmp_rel32 = (int32_t)(fn_vdi - (fn_setpriv + 5));
    memcpy(&detour[1], &jmp_rel32, 4);

    if (mdbg_copyin(target, detour, fn_setpriv, 5) != 0) {
        klog_printf("[Ghostpad] hook_sp: failed to write detour jump to SceShellCore\n");
        if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
        return -1;
    }

    klog_printf("[Ghostpad] hook_sp: scePadSetProcessPrivilege HOOKED successfully (detour -> %p)\n", (void *)fn_vdi);

    if (saved_authid) kernel_set_ucred_authid(mypid, saved_authid);
    return 0;
#endif /* GHOSTPAD_ALLOW_UNSAFE_SETPRIV_HOOK */
}

/*
 * =====================================================================================
 *            UNPATCH SYSTEM PROCESS HOOKS & TERMINATE STUB
 * =====================================================================================
 */

/* Safe memory write with VM protection escalation/restoration */
static void safe_mdbg_restore(pid_t target, void *orig_bytes, intptr_t addr, size_t len) {
    intptr_t page1 = addr & ~(intptr_t)0xfff;
    intptr_t page2 = (addr + len - 1) & ~(intptr_t)0xfff;
    int protect1_ok = (kernel_set_vmem_protection(target, page1, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
    int protect2_ok = 0;
    if (page2 != page1) {
        protect2_ok = (kernel_set_vmem_protection(target, page2, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
    }
    
    mdbg_copyin(target, orig_bytes, addr, len);
    
    if (protect1_ok) {
        kernel_set_vmem_protection(target, page1, 0x1000, PROT_READ | PROT_EXEC);
    }
    if (protect2_ok) {
        kernel_set_vmem_protection(target, page2, 0x1000, PROT_READ | PROT_EXEC);
    }
}

int
shellui_pad_unpatch(void)
{
    pid_t mypid = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(mypid);
    uint8_t privcaps[16];
    memset(privcaps, 0xff, sizeof(privcaps));
    uint8_t saved_caps[16];
    int have_saved_caps = 0;

    if (saved_authid && kernel_get_ucred_caps(mypid, saved_caps) == 0) {
        have_saved_caps = 1;
        kernel_set_ucred_authid(mypid, 0x4800000000010003l);
        kernel_set_ucred_caps(mypid, privcaps);
    }

    klog_printf("[Ghostpad] shellui_pad_unpatch: starting unpatching sequence...\n");

    /* 1. Stop SceShellUI/SceShellCore stub thread if it is running */
    if (g_relaunch_pid > 0 && g_relaunch_args_kaddr != 0) {
        klog_printf("[Ghostpad] shellui_pad_unpatch: stopping target stub thread in pid %d...\n", g_relaunch_pid);
        int32_t stop_val = 1;
        if (mdbg_copyin(g_relaunch_pid, &stop_val, g_relaunch_args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, stop), 4) == 0) {
            /* Wait up to 1 second for the thread to exit and clean up */
            for (int i = 0; i < 50; i++) {
                usleep(20000);
                int32_t ready_val = (int32_t)mdbg_getint(g_relaunch_pid, g_relaunch_args_kaddr + (intptr_t)offsetof(ShellUiPadArgs, ready));
                if (ready_val == 0 || ready_val == -1) {
                    klog_printf("[Ghostpad] shellui_pad_unpatch: stub thread exited successfully.\n");
                    break;
                }
            }
        } else {
            klog_printf("[Ghostpad] shellui_pad_unpatch: failed to write stop flag to target stub.\n");
        }
    }

    /* Find SceShellCore PID */
    pid_t core_pid = -1;
    {
        pid_t pids[8];
        if (find_pids("SceShellCore", pids, 8) > 0) {
            core_pid = pids[0];
        }
    }

    if (core_pid > 0) {
        uint32_t libpad_h = 0;
        get_lib(core_pid, "libScePad", &libpad_h);
        if (libpad_h) {
            intptr_t fn_gethandle = resolve_sym(core_pid, libpad_h, "scePadGetHandle");
            intptr_t fn_vdi = resolve_sym(core_pid, libpad_h, "scePadVirtualDeviceInsertData");
            intptr_t fn_setpriv = resolve_sym(core_pid, libpad_h, "scePadSetProcessPrivilege");
            intptr_t fn_vda = resolve_sym(core_pid, libpad_h, "scePadVirtualDeviceAddDevice");

            /* Restore scePadGetHandle hook */
            if (g_gethandle_hooked && fn_gethandle) {
                klog_printf("[Ghostpad] shellui_pad_unpatch: restoring scePadGetHandle (5 bytes)\n");
                safe_mdbg_restore(core_pid, g_orig_gethandle, fn_gethandle, 5);
                g_gethandle_hooked = 0;
            }

            /* Restore scePadSetProcessPrivilege hook */
            if (g_setpriv_hooked && fn_setpriv) {
                klog_printf("[Ghostpad] shellui_pad_unpatch: restoring scePadSetProcessPrivilege (5 bytes)\n");
                safe_mdbg_restore(core_pid, g_orig_setpriv, fn_setpriv, 5);
                g_setpriv_hooked = 0;
            }

            /* Restore scePadVirtualDeviceInsertData (128 bytes) */
            if (g_vdi_hooked && fn_vdi) {
                klog_printf("[Ghostpad] shellui_pad_unpatch: restoring scePadVirtualDeviceInsertData (128 bytes)\n");
                safe_mdbg_restore(core_pid, g_orig_vdi_128, fn_vdi, 128);
                g_vdi_hooked = 0;
            }

            /* Restore SceShellCore VDA patch */
            if (g_vda_patched && fn_vda && g_vda_patched_pid == core_pid) {
                klog_printf("[Ghostpad] shellui_pad_unpatch: restoring SceShellCore VDA patch\n");
                safe_mdbg_restore(core_pid, g_orig_vda_call, fn_vda + (intptr_t)GHOSTPAD_VDA_PS4_CALL_OFF, 5);
                safe_mdbg_restore(core_pid, g_orig_vda_cave, fn_vda + (intptr_t)GHOSTPAD_VDA_PS4_CAVE_OFF, 8);
                g_vda_patched = 0;
            }
        }
    }

    /* Restore self VDA patch */
    if (g_self_vda_patched) {
        uint32_t self_libpad_h = 0;
        if (get_lib(mypid, "libScePad", &self_libpad_h) == 0) {
            intptr_t self_fn_vda = resolve_sym(mypid, self_libpad_h, "scePadVirtualDeviceAddDevice");
            if (self_fn_vda) {
                klog_printf("[Ghostpad] shellui_pad_unpatch: restoring self VDA patch\n");
                safe_mdbg_restore(mypid, g_orig_self_vda_call, self_fn_vda + (intptr_t)GHOSTPAD_VDA_PS4_CALL_OFF, 5);
                safe_mdbg_restore(mypid, g_orig_self_vda_cave, self_fn_vda + (intptr_t)GHOSTPAD_VDA_PS4_CAVE_OFF, 8);
                g_self_vda_patched = 0;
            }
        }
    }

    klog_printf("[Ghostpad] shellui_pad_unpatch: unpatching sequence complete.\n");

    if (have_saved_caps) {
        kernel_set_ucred_authid(mypid, saved_authid);
        kernel_set_ucred_caps(mypid, saved_caps);
    }
    return 0;
}


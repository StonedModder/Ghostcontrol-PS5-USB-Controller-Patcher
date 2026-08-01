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

#include <machine/reg.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
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

    /* Skip x86-64 red zone (128 bytes below RSP), align to 16 bytes. */
    intptr_t new_rsp = (regs.r_rsp - 256) & ~(intptr_t)0xf;

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

    ret_rsp = (regs.r_rsp - 256) & ~(intptr_t)0xf;
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


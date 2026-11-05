/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_remote.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <machine/reg.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include <ps5/nid.h>

#define GC_REMOTE_AUTHID UINT64_C(0x4800000000010003)
#define GC_REMOTE_SYSCALL_OP UINT64_C(0x500000027)
#define GC_REMOTE_CHECK_OP UINT64_C(0xffffffff00000027)
#define GC_REMOTE_PAGE_SIZE UINT64_C(0x4000)
#define GC_REMOTE_CALL_TIMEOUT_MS 5000u

extern void ghostpad_status_log(const char *format, ...);
#define remote_log(...) ghostpad_status_log("[GC DS4 remote] " __VA_ARGS__)

static void *g_kekcall_target __attribute__((used));
__asm__(
    ".text\n"
    ".global gc_kekcall_bridge\n"
    ".type gc_kekcall_bridge,@function\n"
    "gc_kekcall_bridge:\n"
    "mov 8(%rsp), %rax\n"
    "jmp *g_kekcall_target(%rip)\n");
extern uint64_t gc_kekcall_bridge(
    uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t);

typedef struct {
    pid_t pid;
    uint64_t authid;
    uint8_t caps[16];
    int active;
} GcCredentialGuard;

static int
credentials_begin(GcCredentialGuard *guard)
{
    static const uint8_t privileged_caps[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    memset(guard, 0, sizeof(*guard));
    guard->pid = getpid();
    guard->authid = kernel_get_ucred_authid(guard->pid);
    if (!guard->authid || kernel_get_ucred_caps(guard->pid, guard->caps) != 0)
        return -1;
    if (kernel_set_ucred_authid(guard->pid, GC_REMOTE_AUTHID) != 0)
        return -1;
    if (kernel_set_ucred_caps(guard->pid, privileged_caps) != 0) {
        int saved_errno = errno;
        (void)kernel_set_ucred_authid(guard->pid, guard->authid);
        errno = saved_errno;
        return -1;
    }
    guard->active = 1;
    return 0;
}

static int
credentials_end(GcCredentialGuard *guard)
{
    if (!guard || !guard->active)
        return -1;
    int caps_result = kernel_set_ucred_caps(guard->pid, guard->caps);
    int caps_errno = errno;
    int auth_result = kernel_set_ucred_authid(guard->pid, guard->authid);
    int auth_errno = errno;
    guard->active = 0;
    if (caps_result != 0) {
        errno = caps_errno;
        return -1;
    }
    if (auth_result != 0) {
        errno = auth_errno;
        return -1;
    }
    return 0;
}

static int
privileged_ptrace(int request, pid_t pid, caddr_t address, int data)
{
    GcCredentialGuard guard;
    if (credentials_begin(&guard) != 0)
        return -1;
    int result = (int)__syscall(SYS_ptrace, request, pid, address, data);
    int saved_errno = errno;
    if (credentials_end(&guard) != 0 && result == 0)
        return -1;
    errno = saved_errno;
    return result;
}

static int
privileged_kill(pid_t pid, int signal_number)
{
    GcCredentialGuard guard;
    if (credentials_begin(&guard) != 0)
        return -1;
    int result = (int)__syscall(SYS_kill, pid, signal_number);
    int saved_errno = errno;
    if (credentials_end(&guard) != 0 && result == 0)
        return -1;
    errno = saved_errno;
    return result;
}

static int64_t
remote_syscall(pid_t pid, uint64_t number, const uint64_t arguments[6])
{
    return (int64_t)gc_kekcall_bridge(
        (uint64_t)(uint32_t)pid, number,
        (uint64_t)(uintptr_t)arguments, 0, 0, 0,
        GC_REMOTE_SYSCALL_OP);
}

int
gc_remote_available(void)
{
    g_kekcall_target = (uint8_t *)(uintptr_t)getpid + 7;
    return gc_kekcall_bridge(0, 0, 0, 0, 0, 0, GC_REMOTE_CHECK_OP) == 0;
}

size_t
gc_remote_find_pids(const char *name, pid_t *pids, size_t capacity)
{
    int mib[4] = {1, 14, 8, 0};
    size_t buffer_size = 0;
    size_t count = 0;
    pid_t self = getpid();

    if (!name || !pids || capacity == 0 ||
        sysctl(mib, 4, NULL, &buffer_size, NULL, 0) != 0 ||
        buffer_size == 0)
        return 0;
    uint8_t *buffer = malloc(buffer_size);
    if (!buffer)
        return 0;
    if (sysctl(mib, 4, buffer, &buffer_size, NULL, 0) != 0) {
        free(buffer);
        return 0;
    }

    uint8_t *cursor = buffer;
    const uint8_t *end = buffer + buffer_size;
    while (cursor < end) {
        int record_size = 0;
        if ((size_t)(end - cursor) < sizeof(record_size))
            break;
        memcpy(&record_size, cursor, sizeof(record_size));
        if (record_size < 448 || (size_t)record_size > (size_t)(end - cursor))
            break;

        pid_t pid = 0;
        memcpy(&pid, cursor + 72, sizeof(pid));
        const char *thread_name = (const char *)cursor + 447;
        size_t name_capacity = (size_t)record_size - 447u;
        size_t name_length = 0;
        while (name_length < name_capacity && thread_name[name_length] != '\0')
            ++name_length;
        cursor += (size_t)record_size;
        if (name_length == name_capacity || strcmp(name, thread_name) != 0 ||
            pid == self)
            continue;

        int duplicate = 0;
        for (size_t slot = 0; slot < count; ++slot)
            duplicate |= pids[slot] == pid;
        if (!duplicate && count < capacity)
            pids[count++] = pid;
    }
    free(buffer);

    for (size_t slot = 1; slot < count; ++slot) {
        pid_t value = pids[slot];
        size_t position = slot;
        while (position > 0 && pids[position - 1] > value) {
            pids[position] = pids[position - 1];
            --position;
        }
        pids[position] = value;
    }
    return count;
}

int
gc_remote_get_library(pid_t pid, const char *name, uint32_t *handle)
{
    if (pid <= 0 || !name || !handle)
        return -1;
    *handle = 0;
    int result = kernel_dynlib_handle(pid, name, handle);
    if (result != 0 || *handle == 0) {
        char sprx[64];
        int length = snprintf(sprx, sizeof(sprx), "%s.sprx", name);
        if (length <= 0 || (size_t)length >= sizeof(sprx))
            return -1;
        result = kernel_dynlib_handle(pid, sprx, handle);
    }
    return result == 0 && *handle != 0 ? 0 : -1;
}

intptr_t
gc_remote_library_base(pid_t pid, const char *name)
{
    uint32_t handle = 0;
    if (gc_remote_get_library(pid, name, &handle) != 0)
        return 0;
    intptr_t base = kernel_dynlib_init_addr(pid, handle);
    return base > 0 ? base : 0;
}

intptr_t
gc_remote_resolve(pid_t pid, uint32_t handle, const char *symbol)
{
    if (pid <= 0 || handle == 0 || !symbol)
        return 0;
    intptr_t address = kernel_dynlib_dlsym(pid, handle, symbol);
    if (!address) {
        char nid[12];
        nid_encode(symbol, nid);
        address = kernel_dynlib_resolve(pid, handle, nid);
    }
    return address;
}

int
gc_remote_read(pid_t pid, intptr_t address, void *output, size_t length)
{
    if (pid <= 0 || address <= 0 || !output || length == 0 ||
        length > (uintptr_t)INTPTR_MAX - (uintptr_t)address)
        return -1;
    GcCredentialGuard guard;
    if (credentials_begin(&guard) != 0)
        return -1;
    int result = mdbg_copyout(pid, address, output, length);
    int saved_errno = errno;
    if (credentials_end(&guard) != 0 && result == 0)
        return -1;
    errno = saved_errno;
    return result;
}

int
gc_remote_write(pid_t pid, intptr_t address, const void *input, size_t length)
{
    if (pid <= 0 || address <= 0 || !input || length == 0 ||
        length > (uintptr_t)INTPTR_MAX - (uintptr_t)address)
        return -1;
    GcCredentialGuard guard;
    if (credentials_begin(&guard) != 0)
        return -1;
    int result = mdbg_copyin(pid, input, address, length);
    int saved_errno = errno;
    if (credentials_end(&guard) != 0 && result == 0)
        return -1;
    errno = saved_errno;
    return result;
}

int
gc_remote_map(pid_t pid, size_t length, intptr_t *address)
{
    if (pid <= 0 || length == 0 || !address)
        return -1;
    uint64_t arguments[6] = {
        0, (uint64_t)length, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0
    };
    int64_t mapped = remote_syscall(pid, SYS_mmap, arguments);
    if (mapped <= 0 || (uint64_t)mapped > (uint64_t)INTPTR_MAX)
        return -1;
    uint64_t lock_arguments[6] = {
        (uint64_t)mapped, (uint64_t)length, 0, 0, 0, 0
    };
    if (remote_syscall(pid, SYS_mlock, lock_arguments) != 0) {
        uint64_t unmap_arguments[6] = {
            (uint64_t)mapped, (uint64_t)length, 0, 0, 0, 0
        };
        if (remote_syscall(pid, SYS_munmap, unmap_arguments)) *address = (intptr_t)mapped;
        return -1;
    }
    *address = (intptr_t)mapped;
    return 0;
}

static const char *record_path(int role)
{
    if (role == GC_REMOTE_RECORD_SOURCE) return "/data/ghostcontrol/runtime/ds4-reader.bin";
    if (role == GC_REMOTE_RECORD_GAME) return "/data/ghostcontrol/runtime/ds4-game.bin";
    return NULL;
}

int gc_remote_record_load(int role, GcRemoteRecord *record)
{
    const char *path = record_path(role);
    if (!path || !record) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    GcRemoteRecord candidate;
    int result = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof(candidate) &&
        read(fd, &candidate, sizeof(candidate)) == sizeof(candidate) &&
        candidate.magic == UINT32_C(0x47524344) && candidate.version == 1 &&
        candidate.role == role && candidate.pid > 0 && candidate.process_identity &&
        candidate.mapping > 0 && !(candidate.mapping & 0x3fff) &&
        candidate.mapping_size == 0x8000) { *record = candidate; result = 0; }
    close(fd);
    return result;
}

int gc_remote_record_save(int role, pid_t pid, intptr_t identity, intptr_t mapping, size_t size)
{
    const char *path = record_path(role);
    if (!path || pid <= 0 || !identity || mapping <= 0 || size != 0x8000) return -1;
    char temporary[128];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    GcRemoteRecord record = {UINT32_C(0x47524344), 1, pid, role, identity, mapping, size};
    size_t done = 0;
    while (done < sizeof(record)) {
        ssize_t wrote = write(fd, (const uint8_t *)&record + done, sizeof(record) - done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) break;
        done += (size_t)wrote;
    }
    int synced = done == sizeof(record) && !fsync(fd);
    int closed = close(fd);
    if (!synced || closed || rename(temporary, path)) { unlink(temporary); return -1; }
    return 0;
}

void gc_remote_record_clear(int role)
{
    const char *path = record_path(role);
    if (path) (void)unlink(path);
}

int
gc_remote_protect(pid_t pid, intptr_t address, size_t length, int protection)
{
    if (pid <= 0 || address <= 0 || length == 0)
        return -1;
    uint64_t arguments[6] = {
        (uint64_t)address, (uint64_t)length,
        (uint64_t)(uint32_t)protection, 0, 0, 0
    };
    return remote_syscall(pid, SYS_mprotect, arguments) == 0 ? 0 : -1;
}

int
gc_remote_unmap(pid_t pid, intptr_t address, size_t length)
{
    if (pid <= 0 || address <= 0 || length == 0)
        return -1;
    uint64_t arguments[6] = {
        (uint64_t)address, (uint64_t)length, 0, 0, 0, 0
    };
    return remote_syscall(pid, SYS_munmap, arguments) == 0 ? 0 : -1;
}

static int
ptrace_write(pid_t pid, intptr_t address, const void *input, size_t length)
{
    struct ptrace_io_desc request = {
        .piod_op = PIOD_WRITE_D,
        .piod_offs = (void *)address,
        .piod_addr = (void *)input,
        .piod_len = length
    };
    return privileged_ptrace(PT_IO, pid, (caddr_t)&request, 0);
}

static GcRemoteTraceState
recover_tracee(pid_t pid)
{
    if (privileged_kill(pid, SIGSTOP) != 0 && errno == ESRCH)
        return GC_REMOTE_EXITED;
    for (unsigned elapsed = 0; elapsed < GC_REMOTE_CALL_TIMEOUT_MS; ++elapsed) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            if (WIFSTOPPED(status))
                return GC_REMOTE_STOPPED;
            if (WIFEXITED(status) || WIFSIGNALED(status))
                return GC_REMOTE_EXITED;
        } else if (waited < 0 && (errno == ESRCH || errno == ECHILD)) {
            return GC_REMOTE_EXITED;
        }
        usleep(1000);
    }
    return GC_REMOTE_RUNNING_UNKNOWN;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int restore_registers(GcRemoteSession *session)
{
    if (!session->needs_restore) return 0;
    if (session->state != GC_REMOTE_STOPPED ||
        privileged_ptrace(PT_SETREGS, session->pid,
                          (caddr_t)session->saved_registers, 0)) return -1;
    session->needs_restore = 0;
    return 0;
}

int
gc_remote_session_open(GcRemoteSession *session, pid_t pid)
{
    if (!session || pid <= 0)
        return -1;
    memset(session, 0, sizeof(*session));
    session->pid = pid;
    session->trap_size = (size_t)GC_REMOTE_PAGE_SIZE;
    if (gc_remote_map(pid, session->trap_size, &session->trap_address) != 0)
        goto fail;
    uint8_t trap = 0xcc;
    if (gc_remote_write(pid, session->trap_address, &trap, sizeof(trap)) != 0 ||
        gc_remote_protect(pid, session->trap_address, session->trap_size,
                          PROT_READ | PROT_EXEC) != 0)
        goto fail;
    if (privileged_ptrace(PT_ATTACH, pid, NULL, 0) != 0)
        goto fail;
    session->state = GC_REMOTE_RUNNING_UNKNOWN;
    uint64_t deadline = monotonic_ms() + GC_REMOTE_CALL_TIMEOUT_MS;
    while (monotonic_ms() < deadline) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            session->state = WIFSTOPPED(status) ? GC_REMOTE_STOPPED : GC_REMOTE_EXITED;
            return session->state == GC_REMOTE_STOPPED ? 0 : -1;
        }
        if (waited < 0 && errno != EINTR) break;
        usleep(1000);
    }
    (void)gc_remote_session_close(session);
    return -1;

fail:
    (void)gc_remote_session_close(session);
    return -1;
}

int
gc_remote_session_call(GcRemoteSession *session, intptr_t function,
                       const uint64_t arguments[6], int64_t *result)
{
    if (!session || session->pid <= 0 || function <= 0 || !arguments ||
        !result || session->state != GC_REMOTE_STOPPED || session->needs_restore)
        return -1;

    struct reg registers = {0};
    struct reg saved = {0};
    if (privileged_ptrace(PT_GETREGS, session->pid,
                          (caddr_t)&registers, 0) != 0)
        return -1;
    saved = registers;
    _Static_assert(sizeof(saved) <= sizeof(session->saved_registers), "register storage too small");
    memcpy(session->saved_registers, &saved, sizeof(saved));
    intptr_t stack = ((registers.r_rsp - 256) & ~(intptr_t)0xf) - 8;
    if (ptrace_write(session->pid, stack, &session->trap_address,
                     sizeof(session->trap_address)) != 0)
        return -1;

    registers.r_rsp = stack;
    registers.r_rip = function;
    registers.r_rdi = arguments[0];
    registers.r_rsi = arguments[1];
    registers.r_rdx = arguments[2];
    registers.r_rcx = arguments[3];
    registers.r_r8 = arguments[4];
    registers.r_r9 = arguments[5];
    session->needs_restore = 1;
    if (privileged_ptrace(PT_SETREGS, session->pid,
                          (caddr_t)&registers, 0) != 0)
        return -1;
    if (privileged_ptrace(PT_CONTINUE, session->pid, (caddr_t)1, 0) != 0) {
        (void)restore_registers(session);
        return -1;
    }
    session->state = GC_REMOTE_RUNNING_UNKNOWN;
    uint64_t deadline = monotonic_ms() + GC_REMOTE_CALL_TIMEOUT_MS;
    while (monotonic_ms() < deadline) {
        int status = 0;
        pid_t waited = waitpid(session->pid, &status, WNOHANG);
        if (waited < 0) {
            if (errno == EINTR) continue;
            session->state = errno == ESRCH ? GC_REMOTE_EXITED
                                            : GC_REMOTE_RUNNING_UNKNOWN;
            break;
        }
        if (waited == 0) {
            usleep(1000);
            continue;
        }
        if (!WIFSTOPPED(status)) {
            session->state = GC_REMOTE_EXITED;
            break;
        }
        session->state = GC_REMOTE_STOPPED;
        int signal_number = WSTOPSIG(status);
        if (signal_number == SIGTRAP) {
            if (privileged_ptrace(PT_GETREGS, session->pid,
                                  (caddr_t)&registers, 0) != 0)
                break;
            if ((intptr_t)registers.r_rip == session->trap_address + 1) {
                *result = (int64_t)registers.r_rax;
                if (restore_registers(session) != 0)
                    return -1;
                return 0;
            }
        }
        if (signal_number == SIGBUS || signal_number == SIGSEGV) {
            (void)restore_registers(session);
            return -2;
        }
        int forward = signal_number == SIGCHLD ? 0 : signal_number;
        if (privileged_ptrace(PT_CONTINUE, session->pid,
                              (caddr_t)1, forward) != 0) {
            (void)restore_registers(session);
            return -1;
        }
        session->state = GC_REMOTE_RUNNING_UNKNOWN;
    }

    if (session->state == GC_REMOTE_RUNNING_UNKNOWN)
        session->state = recover_tracee(session->pid);
    if (session->state == GC_REMOTE_STOPPED)
        (void)restore_registers(session);
    remote_log("call failed pid=%d fn=0x%lx state=%d\n",
               session->pid, (unsigned long)function, session->state);
    return -1;
}

int
gc_remote_session_close(GcRemoteSession *session)
{
    if (!session) return -1;
    if (session->pid <= 0) return 0;
    if (session->state == GC_REMOTE_RUNNING_UNKNOWN)
        session->state = recover_tracee(session->pid);
    if (session->state == GC_REMOTE_RUNNING_UNKNOWN ||
        (session->state != GC_REMOTE_EXITED && restore_registers(session))) return -1;
    if (session->state == GC_REMOTE_STOPPED &&
        privileged_ptrace(PT_DETACH, session->pid, (caddr_t)1, 0) != 0)
        return -1;
    if (session->state != GC_REMOTE_EXITED) {
        session->state = GC_REMOTE_DETACHED;
        if (session->trap_address > 0 && gc_remote_unmap(
                session->pid, session->trap_address, session->trap_size)) return -1;
    }
    memset(session, 0, sizeof(*session));
    return 0;
}

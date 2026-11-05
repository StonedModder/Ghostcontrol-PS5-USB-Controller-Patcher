/* SPDX-License-Identifier: GPL-3.0-or-later
 * Single-instance takeover: see gc_singleton.h for the contract. Only POSIX
 * calls are used so the host test suite can exercise the whole sequence. */
#define _DEFAULT_SOURCE
#include "gc_singleton.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* SIGTERM window: a clean teardown of USB endpoints and VDA devices takes
 * several seconds and the old instance drops the lock only at the very end,
 * so wait generously before escalating. */
#define GC_TAKEOVER_TERM_POLL_MS    100
#define GC_TAKEOVER_TERM_TIMEOUT_MS 12000
/* SIGKILL window: a wedged instance cannot run teardown, so the kernel
 * reclaims the descriptors (and the lock) as soon as it is reaped. */
#define GC_TAKEOVER_KILL_POLL_MS    100
#define GC_TAKEOVER_KILL_TIMEOUT_MS 5000
/* Final retry window: the lock usually frees moments after the pid stops
 * being visible; 250 ms steps for a few seconds cover a slow reaper. */
#define GC_TAKEOVER_LOCK_RETRY_MS    250
#define GC_TAKEOVER_LOCK_RETRY_STEPS 20

static void log_line(gc_singleton_log_fn log_fn, void *context,
                     const char *format, ...)
{
    if (!log_fn)
        return;
    char buffer[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_fn(context, buffer);
}

/* A pid of 1 or below is never a real previous instance; our own pid must
 * never be signalled. EPERM means the process exists but belongs to another
 * user, which still counts as alive. */
static int process_is_alive(pid_t pid)
{
    if (pid <= 1 || pid == getpid())
        return 0;
    if (kill(pid, 0) == 0)
        return 1;
    return errno == EPERM;
}

static int try_lock(int fd)
{
    return flock(fd, LOCK_EX | LOCK_NB) == 0;
}

/* Reads the previous instance's pid from the open fd. Empty, garbage and
 * stale contents are all tolerated: an unusable value returns -1. */
static pid_t read_previous_pid(int fd)
{
    char buffer[32];
    if (lseek(fd, 0, SEEK_SET) < 0)
        return (pid_t)-1;
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    if (count <= 0)
        return (pid_t)-1;
    buffer[count] = '\0';
    long value = strtol(buffer, NULL, 10);
    if (value <= 1 || value == (long)getpid())
        return (pid_t)-1;
    return (pid_t)value;
}

/* Poll until the lock is free or the process disappears, whichever first.
 * Acquiring the lock here is fine: the caller retries flock on the same fd,
 * which succeeds immediately once we hold it. */
/* Returns 0 when the previous instance is gone (or was already), 1 when it
 * survived an accepted SIGKILL (wedged in an unkillable kernel call), and -1
 * when it could not be signalled at all. */
static int evict_previous(int pid_fd, pid_t previous,
                          gc_singleton_log_fn log_fn, void *log_context)
{
    if (!process_is_alive(previous)) {
        log_line(log_fn, log_context,
                 "startup: previous pid=%d is not alive", (int)previous);
        return 0;
    }

    log_line(log_fn, log_context,
             "startup: requesting previous Ghostcontrol pid=%d shutdown",
             (int)previous);
    if (kill(previous, SIGTERM) != 0)
        log_line(log_fn, log_context,
                 "startup: SIGTERM previous pid=%d failed errno=%d",
                 (int)previous, errno);

    for (int i = 0; i < GC_TAKEOVER_TERM_TIMEOUT_MS / GC_TAKEOVER_TERM_POLL_MS; i++) {
        usleep(GC_TAKEOVER_TERM_POLL_MS * 1000);
        if (try_lock(pid_fd)) {
            log_line(log_fn, log_context,
                     "startup: previous pid=%d exited after SIGTERM", (int)previous);
            return 0;
        }
        if (!process_is_alive(previous)) {
            log_line(log_fn, log_context,
                     "startup: previous pid=%d exited after SIGTERM", (int)previous);
            return 0;
        }
    }

    log_line(log_fn, log_context,
             "startup: previous pid=%d still alive after SIGTERM; forcing SIGKILL",
             (int)previous);
    if (kill(previous, SIGKILL) != 0) {
        log_line(log_fn, log_context,
                 "startup: SIGKILL previous pid=%d failed errno=%d",
                 (int)previous, errno);
        return -1;
    }

    for (int i = 0; i < GC_TAKEOVER_KILL_TIMEOUT_MS / GC_TAKEOVER_KILL_POLL_MS; i++) {
        usleep(GC_TAKEOVER_KILL_POLL_MS * 1000);
        if (try_lock(pid_fd)) {
            log_line(log_fn, log_context,
                     "startup: previous pid=%d killed", (int)previous);
            return 0;
        }
        if (!process_is_alive(previous)) {
            log_line(log_fn, log_context,
                     "startup: previous pid=%d killed", (int)previous);
            return 0;
        }
    }

    log_line(log_fn, log_context,
             "startup: WARNING previous pid=%d still appears alive after SIGKILL",
             (int)previous);
    return 1;
}

/* Secondary lock used when a predecessor is wedged beyond SIGKILL: it can
 * never run again, so its primary lock is abandoned rather than blocking
 * every future deploy until a console reboot. */
static int g_hold_fd = -1;

void gc_singleton_release_secondary(void)
{
    if (g_hold_fd >= 0) {
        flock(g_hold_fd, LOCK_UN);
        close(g_hold_fd);
        g_hold_fd = -1;
    }
}

static int acquire_lock_with_retries(int pid_fd,
                                     gc_singleton_log_fn log_fn, void *log_context)
{
    for (int i = 0; i < GC_TAKEOVER_LOCK_RETRY_STEPS; i++) {
        if (try_lock(pid_fd))
            return 0;
        if (i == 0)
            log_line(log_fn, log_context,
                     "startup: waiting for singleton lock release (retrying every %d ms)",
                     GC_TAKEOVER_LOCK_RETRY_MS);
        usleep(GC_TAKEOVER_LOCK_RETRY_MS * 1000);
    }
    if (try_lock(pid_fd))
        return 0;
    log_line(log_fn, log_context,
             "startup: could not acquire singleton lock; refusing to start a second instance");
    return -1;
}

int gc_singleton_takeover(int pid_fd, const char *pid_path, unsigned settle_us,
                          gc_singleton_log_fn log_fn, void *log_context)
{
    if (pid_fd < 0 || !pid_path)
        return -1;

    /* If the lock is already free there is no live owner to evict. */
    if (!try_lock(pid_fd)) {
        pid_t previous = read_previous_pid(pid_fd);
        int evict = -2;
        if (previous > 1) {
            evict = evict_previous(pid_fd, previous, log_fn, log_context);
        } else {
            log_line(log_fn, log_context,
                     "startup: singleton lock busy but pid file has no usable pid; waiting for release");
        }
        if (acquire_lock_with_retries(pid_fd, log_fn, log_context) != 0) {
            /* The primary lock is still held. If the recorded owner is dead
             * (evict 0) or wedged beyond an accepted SIGKILL (evict 1), the
             * holder is a ghost that will never touch shared state again and
             * a secondary lock is safe. Only an owner we could not signal at
             * all (evict -1) must still force a refusal. */
            if (evict != 0 && evict != 1) {
                log_line(log_fn, log_context,
                         "startup: cannot verify predecessor pid=%d is gone; refusing",
                         (int)previous);
                return -1;
            }
            char hold_path[256];
            snprintf(hold_path, sizeof(hold_path), "%s.hold", pid_path);
            int hold_fd = open(hold_path, O_RDWR | O_CREAT, 0600);
            if (hold_fd < 0 || !try_lock(hold_fd)) {
                if (hold_fd >= 0)
                    close(hold_fd);
                log_line(log_fn, log_context,
                         "startup: secondary lock %s busy; refusing", hold_path);
                return -1;
            }
            g_hold_fd = hold_fd; /* held for the process lifetime */
            log_line(log_fn, log_context,
                     "startup: predecessor pid=%d is a wedged ghost; claimed secondary lock %s",
                     (int)previous, hold_path);
        }
    }

    if (ftruncate(pid_fd, 0) != 0)
        log_line(log_fn, log_context,
                 "startup: failed to truncate singleton pid file errno=%d", errno);
    if (lseek(pid_fd, 0, SEEK_SET) < 0)
        log_line(log_fn, log_context,
                 "startup: failed to seek singleton pid file errno=%d", errno);
    char text[32];
    int length = snprintf(text, sizeof(text), "%d\n", (int)getpid());
    if (length > 0) {
        ssize_t written = write(pid_fd, text, (size_t)length);
        if (written != (ssize_t)length)
            log_line(log_fn, log_context,
                     "startup: failed to write singleton pid errno=%d", errno);
    }
    (void)fsync(pid_fd);
    log_line(log_fn, log_context,
             "startup: claimed singleton pid=%d at %s", (int)getpid(), pid_path);

    /* Let the USB stack release endpoints after a killed or crashed prior
     * instance before this process claims device slots. */
    if (settle_us > 0) {
        usleep(settle_us);
        log_line(log_fn, log_context, "startup: USB settle after singleton handoff");
    }
    return 0;
}

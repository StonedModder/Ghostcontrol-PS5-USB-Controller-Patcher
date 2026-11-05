/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GC_SINGLETON_H
#define GC_SINGLETON_H

/* Single-instance takeover for the resident core.
 *
 * A redeploy loads the core ELF a second time while the first instance still
 * owns the controllers. The new process must evict the old one instead of
 * silently doing nothing (which is what forced a console reboot before). The
 * pid file doubles as an advisory lock: whoever holds LOCK_EX owns the
 * controllers. Release of that lock is the primary "previous instance is
 * gone" signal; process liveness is only the secondary one, because a clean
 * shutdown needs several seconds to tear down USB and VDA first. */

/* USB stack settle after a handoff, before device slots are claimed. */
#define GC_SINGLETON_SETTLE_US 1000000u

/* Log sink: the payload passes gp_log, which keeps this module free of PS5
 * headers so the host test suite can compile it. message has no newline. */
typedef void (*gc_singleton_log_fn)(void *context, const char *message);

/* Takes over the singleton behind pid_fd, which must already be open O_RDWR
 * on the pid file. Any previous live instance is asked to exit (SIGTERM, then
 * SIGKILL) and the exclusive lock is then acquired. On success the pid file is
 * rewritten with this process's pid, settle_us microseconds are spent letting
 * the USB stack settle, and 0 is returned. On failure -1 is returned and the
 * caller must not start a second instance. pid_path is used only for logging. */
int gc_singleton_takeover(int pid_fd, const char *pid_path, unsigned settle_us,
                          gc_singleton_log_fn log_fn, void *log_context);

/* Releases the secondary lock if this instance claimed one. Called from the
 * shutdown path next to the primary lock release. */
void gc_singleton_release_secondary(void);

#endif

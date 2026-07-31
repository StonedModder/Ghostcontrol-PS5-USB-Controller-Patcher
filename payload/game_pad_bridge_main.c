/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wireless DS4 -> native PS5 game ScePadData bridge.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/klog.h>
#include <ps5/payload.h>

#include "gc_types.h"
#include "shellui_pad.h"

#define GAME_BRIDGE_STOP_FILE "/data/ds4tod5/stop-game-pad-bridge"
#define GAME_BRIDGE_LOG_FILE  "/data/ds4tod5/game-pad-bridge.log"
#define GAME_BRIDGE_LOCK_FILE "/data/ds4tod5/game-pad-bridge-supervisor.lock"
#define GAME_BRIDGE_STATE_FILE "/data/ds4tod5/game-pad-bridge-supervisor.txt"
#define GAME_BRIDGE_LOCK_FRESH_SECONDS 20

#ifndef GC_WIRELESS_DS4_AUTO_WATCH
#define GC_WIRELESS_DS4_AUTO_WATCH 0
#endif

static time_t g_last_supervisor_write;

static int
process_alive(pid_t pid)
{
    errno = 0;
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static void
write_supervisor_state(const char *state, pid_t game_pid,
                       unsigned sessions,
                       unsigned long long output_frames, int force)
{
    time_t now = time(NULL);
    if (!force && g_last_supervisor_write != 0 &&
        now - g_last_supervisor_write < 5)
        return;

    char report[384];
    int length = snprintf(
        report, sizeof(report),
        "pid=%d\nheartbeat_epoch=%lld\nauto_watch=%d\nstate=%s\n"
        "game_pid=%d\nsessions=%u\noutput_frames=%llu\n",
        getpid(), (long long)now, GC_WIRELESS_DS4_AUTO_WATCH,
        state ? state : "unknown", game_pid, sessions, output_frames);
    if (length <= 0)
        return;
    size_t write_length = (size_t)length;
    if (write_length >= sizeof(report))
        write_length = sizeof(report) - 1u;

    int state_fd = open(
        GAME_BRIDGE_STATE_FILE,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (state_fd >= 0) {
        (void)write(state_fd, report, write_length);
        close(state_fd);
    }

    char lock[128];
    int lock_length = snprintf(
        lock, sizeof(lock), "pid=%d\nheartbeat_epoch=%lld\n",
        getpid(), (long long)now);
    int lock_fd = open(
        GAME_BRIDGE_LOCK_FILE,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (lock_fd >= 0) {
        if (lock_length > 0)
            (void)write(lock_fd, lock, (size_t)lock_length);
        close(lock_fd);
    }
    g_last_supervisor_write = now;
}

static int
acquire_supervisor_lock(void)
{
    char lock[128];
    pid_t existing_pid = -1;
    long long heartbeat = 0;
    int fd = open(GAME_BRIDGE_LOCK_FILE, O_RDONLY);
    if (fd >= 0) {
        ssize_t length = read(fd, lock, sizeof(lock) - 1u);
        close(fd);
        if (length > 0) {
            lock[length] = '\0';
            (void)sscanf(
                lock, "pid=%d\nheartbeat_epoch=%lld",
                &existing_pid, &heartbeat);
        }
    }

    time_t now = time(NULL);
    if (existing_pid > 0 && heartbeat > 0 &&
        now >= (time_t)heartbeat &&
        now - (time_t)heartbeat <= GAME_BRIDGE_LOCK_FRESH_SECONDS &&
        process_alive(existing_pid))
        return 0;

    fd = open(
        GAME_BRIDGE_LOCK_FILE,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    int length = snprintf(
        lock, sizeof(lock), "pid=%d\nheartbeat_epoch=%lld\n",
        getpid(), (long long)now);
    int write_result = length > 0
        ? (int)write(fd, lock, (size_t)length)
        : -1;
    close(fd);
    return write_result == length ? 1 : -1;
}

extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *out_user_id);
extern int32_t sceUserServiceGetForegroundUser(int32_t *out_user_id);
extern int32_t sceKernelSendNotificationRequest(
    int unk0, void *request, size_t size, int unk1);

typedef struct {
    char unknown[45];
    char message[3075];
} GameBridgeNotifyRequest;

void
ghostpad_status_log_reset(void)
{
    mkdir("/data/ds4tod5", 0755);
    int fd = open(
        GAME_BRIDGE_LOG_FILE,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
        close(fd);
}

void
ghostpad_status_log(const char *format, ...)
{
    char buffer[768];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    size_t write_length = (size_t)length;
    if (write_length >= sizeof(buffer))
        write_length = sizeof(buffer) - 1;
    klog_printf("%s", buffer);
    int fd = open(
        GAME_BRIDGE_LOG_FILE,
        O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        (void)write(fd, buffer, write_length);
        close(fd);
    }
}

static void
game_bridge_notify(const char *message)
{
    GameBridgeNotifyRequest request;
    memset(&request, 0, sizeof(request));
    if (message)
        snprintf(request.message, sizeof(request.message), "%s", message);
    int32_t result = sceKernelSendNotificationRequest(
        0, &request, sizeof(request), 0);
    ghostpad_status_log(
        "[DS4toDS5] notification result=0x%08x message=%s\n",
        (uint32_t)result, message ? message : "");
}

typedef enum {
    SESSION_END_STOP_REQUESTED = 0,
    SESSION_END_GAME_EXITED = 1,
    SESSION_END_READER_FAILED = 2,
    SESSION_END_WRITER_FAILED = 3,
    SESSION_END_RECEIVER_FAILED = 4
} GameSessionEndReason;

static unsigned
run_game_session(pid_t reader_pid, intptr_t reader_args,
                 pid_t game_pid, intptr_t bridge_args,
                 unsigned session,
                 unsigned long long previous_output_frames,
                 GameSessionEndReason *out_end_reason)
{
    unsigned input_frames = 0;
    unsigned output_frames = 0;
    unsigned stale_frames = 0;
    unsigned read_failures = 0;
    unsigned write_failures = 0;
    uint32_t last_seq = 0;
    unsigned consecutive_read_failures = 0;
    unsigned consecutive_write_failures = 0;
    unsigned consecutive_receiver_health_failures = 0;
    unsigned loop_count = 0;
    unsigned last_health_frame = 0;
    int game_alive = 1;
    GameSessionEndReason end_reason = SESSION_END_STOP_REQUESTED;
    write_supervisor_state(
        "active", game_pid, session, previous_output_frames, 1);
    while (access(GAME_BRIDGE_STOP_FILE, F_OK) != 0) {
        ScePadData pad;
        uint32_t seq = 0;
        memset(&pad, 0, sizeof(pad));
        if (shellui_pad_remote_reader_read(
                reader_pid, reader_args, &pad, sizeof(pad), &seq) == 0) {
            consecutive_read_failures = 0;
            if (seq != last_seq) {
                input_frames++;
                if (shellui_pad_game_bridge_update(
                        game_pid, bridge_args,
                        &pad, sizeof(pad)) == 0) {
                    output_frames++;
                    consecutive_write_failures = 0;
                } else {
                    write_failures++;
                    consecutive_write_failures++;
                }
                last_seq = seq;
            } else {
                stale_frames++;
            }
        } else {
            read_failures++;
            consecutive_read_failures++;
        }

        if (input_frames != 0 && (input_frames % 600u) == 0 &&
            input_frames != last_health_frame) {
            ghostpad_status_log(
                "[DS4toDS5] BRIDGE HEALTH in=%u out=%u seq=%u conn=%u "
                "buttons=0x%08x stale=%u read_fail=%u write_fail=%u "
                "receiver_health_fail=%u\n",
                input_frames, output_frames, last_seq, pad.connected,
                pad.buttons, stale_frames, read_failures, write_failures,
                consecutive_receiver_health_failures);
            write_supervisor_state(
                "active", game_pid, session,
                previous_output_frames + output_frames, 0);
            last_health_frame = input_frames;
        }
        loop_count++;
        if ((loop_count % 120u) == 0) {
            errno = 0;
            game_alive = kill(game_pid, 0) == 0 || errno == EPERM;
            Ds4tod5GameBridgeStatus receiver_status;
            memset(&receiver_status, 0, sizeof(receiver_status));
            if (shellui_pad_game_bridge_status(
                    game_pid, bridge_args, &receiver_status) == 0 &&
                receiver_status.receiver_ready == 1) {
                consecutive_receiver_health_failures = 0;
            } else {
                consecutive_receiver_health_failures++;
            }
        }
        if (consecutive_read_failures >= 240u ||
            consecutive_write_failures >= 1200u ||
            consecutive_receiver_health_failures >= 5u ||
            !game_alive) {
            ghostpad_status_log(
                "[DS4toDS5] bridge stopping read_streak=%u "
                "write_streak=%u receiver_health_streak=%u "
                "game_alive=%d game_pid=%d\n",
                consecutive_read_failures,
                consecutive_write_failures,
                consecutive_receiver_health_failures,
                game_alive, game_pid);
            if (!game_alive)
                end_reason = SESSION_END_GAME_EXITED;
            else if (consecutive_read_failures >= 240u)
                end_reason = SESSION_END_READER_FAILED;
            else if (consecutive_receiver_health_failures >= 5u)
                end_reason = SESSION_END_RECEIVER_FAILED;
            else
                end_reason = SESSION_END_WRITER_FAILED;
            break;
        }
        usleep(8333);
    }
    int remove_result =
        shellui_pad_game_bridge_remove(game_pid, bridge_args);
    ghostpad_status_log(
        "[DS4toDS5] game bridge remove=%d pid=%d\n",
        remove_result, game_pid);
    ghostpad_status_log(
        "[DS4toDS5] game session end pid=%d reason=%d in=%u out=%u\n",
        game_pid, end_reason, input_frames, output_frames);
    if (out_end_reason)
        *out_end_reason = end_reason;
    return output_frames;
}

static int
wait_for_game_receiver(pid_t game_pid, intptr_t bridge_args)
{
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        Ds4tod5GameBridgeStatus status;
        memset(&status, 0, sizeof(status));
        if (shellui_pad_game_bridge_status(
                game_pid, bridge_args, &status) == 0) {
            if (status.receiver_ready == 1)
                return 0;
            if (status.receiver_ready == 2)
                return -1;
        }
        usleep(10000);
    }
    return -1;
}

static void
wait_for_game_exit_or_stop(pid_t game_pid, const char *state,
                           unsigned sessions,
                           unsigned long long output_frames)
{
    while (access(GAME_BRIDGE_STOP_FILE, F_OK) != 0 &&
           process_alive(game_pid)) {
        write_supervisor_state(
            state, game_pid, sessions, output_frames, 0);
        usleep(500000);
    }
}

static int
start_wireless_reader(int32_t user_id, pid_t *reader_pid,
                      intptr_t *reader_args, unsigned sessions,
                      unsigned long long output_frames)
{
    for (;;) {
        if (shellui_pad_remote_reader_start(
                user_id, reader_pid, reader_args) == 0)
            return 0;
        if (!GC_WIRELESS_DS4_AUTO_WATCH ||
            access(GAME_BRIDGE_STOP_FILE, F_OK) == 0)
            return -1;
        ghostpad_status_log(
            "[DS4toDS5] wireless reader unavailable; retrying\n");
        write_supervisor_state(
            "waiting_for_wireless_controller", -1, sessions,
            output_frames, 1);
        usleep(5000000);
    }
}

int
main(void)
{
    _Static_assert(sizeof(ScePadData) == 120,
                   "firmware-11.60 ScePadData must be 120 bytes");
    int32_t initial_user = -1;
    int32_t foreground_user = -1;
    pid_t reader_pid = -1;
    intptr_t reader_args = 0;
    unsigned sessions = 0;
    unsigned long long total_output_frames = 0;
    pid_t retry_pid = -1;
    unsigned install_retries = 0;
    int exit_code = 1;
    /* The singleton lock lives below this directory.  Create it before the
     * first lock attempt so a clean console can produce both logs and a lock;
     * previously a missing directory made Start exit before observability was
     * initialized. */
    (void)mkdir("/data/ds4tod5", 0755);
    int lock_result = acquire_supervisor_lock();
    if (lock_result == 0) {
        game_bridge_notify("Ghostcontrol: wireless DS4 bridge is already running");
        payload_exit(0);
        return 0;
    }
    if (lock_result < 0) {
        game_bridge_notify("Ghostcontrol: bridge could not acquire its lock");
        payload_exit(1);
        return 0;
    }

    ghostpad_status_log_reset();
    (void)unlink(GAME_BRIDGE_STOP_FILE);
    write_supervisor_state("starting", -1, 0, 0, 1);
    (void)sceUserServiceInitialize(NULL);
    (void)sceUserServiceGetInitialUser(&initial_user);
    (void)sceUserServiceGetForegroundUser(&foreground_user);
    int32_t user_id =
        foreground_user >= 0 ? foreground_user : initial_user;
    ghostpad_status_log(
        "[DS4toDS5] game bridge start user=0x%08x auto_watch=%d\n",
        (uint32_t)user_id, GC_WIRELESS_DS4_AUTO_WATCH);
    game_bridge_notify(
        "Ghostcontrol: wireless DS4 bridge injected; starting reader");
    if (user_id < 0)
        goto cleanup;

    if (start_wireless_reader(
            user_id, &reader_pid, &reader_args,
            sessions, total_output_frames) != 0) {
        ghostpad_status_log("[DS4toDS5] wireless reader start failed\n");
        goto cleanup;
    }
    write_supervisor_state("waiting_for_game", -1, 0, 0, 1);
    game_bridge_notify(
        "Ghostcontrol: wireless DS4 ready; waiting for a PS5 game");

    for (;;) {
        if (access(GAME_BRIDGE_STOP_FILE, F_OK) == 0)
            break;

        pid_t game_pid = -1;
        intptr_t bridge_args = 0;
        int install_result = shellui_pad_game_bridge_install(
            user_id, &game_pid, &bridge_args);
        if ((install_result == -2 || install_result == -3) &&
            GC_WIRELESS_DS4_AUTO_WATCH) {
            retry_pid = -1;
            install_retries = 0;
            write_supervisor_state(
                install_result == -2
                    ? "waiting_for_game"
                    : "waiting_for_unique_game",
                -1, sessions, total_output_frames, 0);
            usleep(1000000);
            continue;
        }
        if (install_result != 1) {
            if (GC_WIRELESS_DS4_AUTO_WATCH && game_pid > 0) {
                if (retry_pid != game_pid) {
                    retry_pid = game_pid;
                    install_retries = 0;
                }
                install_retries++;
                unsigned retry_limit = install_result == -4 ? 120u : 15u;
                if (install_result != 0 && install_retries <= retry_limit) {
                    ghostpad_status_log(
                        "[DS4toDS5] game not ready pid=%d result=%d "
                        "retry=%u/%u\n",
                        game_pid, install_result, install_retries,
                        retry_limit);
                    write_supervisor_state(
                        "waiting_game_ready", game_pid, sessions,
                        total_output_frames, 0);
                    usleep(1000000);
                    continue;
                }
                ghostpad_status_log(
                    "[DS4toDS5] game skipped pid=%d result=%d retries=%u; "
                    "waiting for it to exit\n",
                    game_pid, install_result, install_retries);
                wait_for_game_exit_or_stop(
                    game_pid, "game_skipped", sessions,
                    total_output_frames);
                retry_pid = -1;
                install_retries = 0;
                continue;
            }
            ghostpad_status_log(
                "[DS4toDS5] game bridge install failed result=%d\n",
                install_result);
            break;
        }
        if (wait_for_game_receiver(game_pid, bridge_args) != 0) {
            ghostpad_status_log(
                "[DS4toDS5] game receiver did not become ready pid=%d\n",
                game_pid);
            (void)shellui_pad_game_bridge_remove(game_pid, bridge_args);
            if (GC_WIRELESS_DS4_AUTO_WATCH &&
                process_alive(game_pid)) {
                if (retry_pid != game_pid) {
                    retry_pid = game_pid;
                    install_retries = 0;
                }
                install_retries++;
                if (install_retries <= 5) {
                    ghostpad_status_log(
                        "[DS4toDS5] receiver retry pid=%d retry=%u/5\n",
                        game_pid, install_retries);
                    write_supervisor_state(
                        "waiting_receiver_ready", game_pid, sessions,
                        total_output_frames, 0);
                    usleep(1000000);
                    continue;
                }
                ghostpad_status_log(
                    "[DS4toDS5] receiver failed repeatedly pid=%d; "
                    "waiting for it to exit\n",
                    game_pid);
                wait_for_game_exit_or_stop(
                    game_pid, "receiver_failed", sessions,
                    total_output_frames);
                retry_pid = -1;
                install_retries = 0;
                continue;
            }
            break;
        }
        retry_pid = -1;
        install_retries = 0;

        sessions++;
        ghostpad_status_log(
            "[DS4toDS5] game bridge installed reader_pid=%d "
            "reader_args=0x%lx game_pid=%d bridge_args=0x%lx "
            "session=%u\n",
            reader_pid, (unsigned long)reader_args,
            game_pid, (unsigned long)bridge_args, sessions);
        game_bridge_notify(
            "Ghostcontrol: wireless DS4 active in the PS5 game");
        GameSessionEndReason end_reason = SESSION_END_STOP_REQUESTED;
        unsigned session_output_frames = run_game_session(
            reader_pid, reader_args, game_pid, bridge_args,
            sessions, total_output_frames, &end_reason);
        total_output_frames += session_output_frames;
        if (total_output_frames > 0)
            exit_code = 0;

        if (!GC_WIRELESS_DS4_AUTO_WATCH ||
            access(GAME_BRIDGE_STOP_FILE, F_OK) == 0)
            break;
        if (end_reason == SESSION_END_READER_FAILED &&
            process_alive(game_pid)) {
            int reader_stop = process_alive(reader_pid)
                ? shellui_pad_remote_reader_stop(reader_pid, reader_args)
                : 0;
            ghostpad_status_log(
                "[DS4toDS5] wireless reader recovery stop=%d\n",
                reader_stop);
            if (reader_stop != 0)
                break;
            reader_pid = -1;
            reader_args = 0;
            if (start_wireless_reader(
                    user_id, &reader_pid, &reader_args,
                    sessions, total_output_frames) != 0)
                break;
            ghostpad_status_log(
                "[DS4toDS5] wireless reader recovered; "
                "reinstalling current game\n");
        }
        ghostpad_status_log(
            "[DS4toDS5] waiting for next game after session=%u\n",
            sessions);
        write_supervisor_state(
            "waiting_for_next_game", -1, sessions,
            total_output_frames, 1);
        usleep(1000000);
    }

cleanup:
    if (reader_pid >= 0 && reader_args != 0) {
        int reader_stop =
            shellui_pad_remote_reader_stop(reader_pid, reader_args);
        ghostpad_status_log(
            "[DS4toDS5] wireless reader stop=%d\n", reader_stop);
    }
    (void)unlink(GAME_BRIDGE_STOP_FILE);
    write_supervisor_state(
        "stopped", -1, sessions, total_output_frames, 1);
    game_bridge_notify("Ghostcontrol: wireless DS4 bridge stopped");
    (void)unlink(GAME_BRIDGE_LOCK_FILE);
    ghostpad_status_log(
        "[DS4toDS5] game bridge exit=%d sessions=%u total_out=%llu\n",
        exit_code, sessions, total_output_frames);
    payload_exit(exit_code);
    return 0;
}

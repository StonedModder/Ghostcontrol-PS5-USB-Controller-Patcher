/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only live status snapshot for the wireless game pad bridge.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/klog.h>
#include <ps5/payload.h>

#include "shellui_pad.h"

#define BRIDGE_INSTALL_REPORT "/data/ds4tod5/game-pad-bridge-last.txt"
#define BRIDGE_STATUS_REPORT  "/data/ds4tod5/game-pad-bridge-status.txt"

void
ghostpad_status_log_reset(void)
{
}

void
ghostpad_status_log(const char *format, ...)
{
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    klog_printf("%s", buffer);
}

static int
read_install_identity(pid_t *out_pid, intptr_t *out_args)
{
    char buffer[4096];
    int fd = open(BRIDGE_INSTALL_REPORT, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t length = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (length <= 0)
        return -1;
    buffer[length] = '\0';

    long pid_value = -1;
    unsigned long args_value = 0;
    char *line = buffer;
    while (line && *line) {
        if (!strncmp(line, "pid=", 4))
            pid_value = strtol(line + 4, NULL, 0);
        else if (!strncmp(line, "args_address=", 13))
            args_value = strtoul(line + 13, NULL, 0);
        char *next = strchr(line, '\n');
        line = next ? next + 1 : NULL;
    }
    if (pid_value <= 0 || args_value == 0)
        return -1;
    *out_pid = (pid_t)pid_value;
    *out_args = (intptr_t)args_value;
    return 0;
}

int
main(void)
{
    pid_t game_pid = -1;
    intptr_t args_address = 0;
    Ds4tod5GameBridgeStatus status;
    memset(&status, 0, sizeof(status));

    int identity_result =
        read_install_identity(&game_pid, &args_address);
    int snapshot_result = identity_result == 0
        ? shellui_pad_game_bridge_status(
              game_pid, args_address, &status)
        : -1;

    int fd = open(
        BRIDGE_STATUS_REPORT,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        char report[1536];
        int report_length = snprintf(
            report, sizeof(report),
            "mode=read-only-no-attach-no-write\n"
                "identity_result=%d\n"
                "pid=%d\n"
                "args_address=0x%lx\n"
                "snapshot_result=%d\n"
                "active=%u\n"
                "seq=%u\n"
                "pad_handle=0x%08x\n"
                "receiver_ready=%d\n"
                "receiver_last_result=%d\n"
                "receiver_port=%u\n"
                "receiver_packets=%llu\n"
                "read_state_calls=%llu\n"
                "read_state_ext_calls=%llu\n"
                "read_calls=%llu\n"
                "read_ext_calls=%llu\n"
                "data_internal_calls=%llu\n"
                "controller_info_calls=%llu\n"
                "controller_info_spoofs=%llu\n"
                "buttons=0x%08x\n"
                "connected=%u\n",
                identity_result, game_pid,
                (unsigned long)args_address, snapshot_result,
                status.active, status.seq,
                (uint32_t)status.pad_handle,
                status.receiver_ready,
                status.receiver_last_result,
                status.receiver_port,
                (unsigned long long)status.receiver_packets,
                (unsigned long long)status.read_state_calls,
                (unsigned long long)status.read_state_ext_calls,
                (unsigned long long)status.read_calls,
                (unsigned long long)status.read_ext_calls,
                (unsigned long long)status.data_internal_calls,
                (unsigned long long)status.controller_info_calls,
                (unsigned long long)status.controller_info_spoofs,
                status.buttons, status.connected);
        if (report_length > 0) {
            size_t write_length = (size_t)report_length;
            if (write_length >= sizeof(report))
                write_length = sizeof(report) - 1u;
            (void)write(fd, report, write_length);
        }
        close(fd);
    }
    payload_exit(snapshot_result == 0 && fd >= 0 ? 0 : 1);
    return 0;
}

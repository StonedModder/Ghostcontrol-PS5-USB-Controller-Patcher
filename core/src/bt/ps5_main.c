/* SPDX-License-Identifier: GPL-3.0-or-later
 * GhostTooth userland Bluetooth HID host for PS5.
 */
#include "hci_usb.h"
#include "host.h"
#include "lock.h"
#include "log.h"
#include "ps5_power.h"
#include "ps5_sysinfo.h"
#include "ps5_vpad.h"
#include "util.h"
#include "version.h"

#include <sys/stat.h>
#include <sys/syscall.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STATE_DIR    "/data/ghosttooth"
#define LOG_PATH     STATE_DIR "/ghosttooth.log"
#define DB_PATH      STATE_DIR "/controllers.db"
#define LOCK_PATH    STATE_DIR "/runtime.lock"
#define STATUS_PATH  STATE_DIR "/status.json"
#define PAIR_FLAG    STATE_DIR "/pair"
#define RETRY_FLAG   STATE_DIR "/retry"
#define FORGET_FLAG  STATE_DIR "/forget"
#define STOP_FLAG    STATE_DIR "/stop"
#define PAIR_SECONDS 60
#define OPEN_TRIES   5
#define RAISED_TRIES 2

enum bt_state { BT_TRYING, BT_READY, BT_FAILED, BT_STOPPED };

static volatile sig_atomic_t g_stop;
static host_t *g_host;
static int g_vpad_ok;
static int g_bt_attempt;
static long g_bt_next;
static int g_bt_plain_creds;
static enum bt_state g_bt_state = BT_TRYING;
static char g_bt_reason[200];

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

static void on_connect(void *unused, int slot, const pad_info *info)
{
    (void)unused;
    if(vpad_add(slot))
        notify("GhostTooth: %s connected. Choose the new controller user, then tap PS.",
               info->profile);
    else
        notify("GhostTooth: controller connected, but its virtual pad failed. See the log.");
}

static void on_state(void *unused, int slot, const pad_state *state)
{
    (void)unused;
    vpad_update(slot, state);
}

static void on_disconnect(void *unused, int slot)
{
    (void)unused;
    vpad_remove(slot);
    notify("GhostTooth: Bluetooth controller %d disconnected", slot + 1);
}

static void on_idle(void *unused)
{
    (void)unused;
}

static void json_text(FILE *file, const char *text)
{
    fputc('"', file);
    for(; text && *text; ++text) {
        unsigned char value = (unsigned char)*text;
        if(value < 0x20)
            continue;
        if(value == '"' || value == '\\')
            fputc('\\', file);
        fputc(value, file);
    }
    fputc('"', file);
}

static void format_addr(char output[18], const unsigned char address[6])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3], address[2], address[1],
             address[0]);
}

static void write_status(void)
{
    static const char *const names[] = {"trying", "ready", "failed", "stopped"};
    char temporary[320];
    snprintf(temporary, sizeof(temporary), "%s.tmp.%d", STATUS_PATH,
             (int)getpid());
    FILE *file = fopen(temporary, "w");
    if(!file)
        return;
    fprintf(file,
            "{\"ok\":true,\"version\":\"%s\",\"bluetooth\":\"%s\",\"reason\":",
            GHOSTTOOTH_VERSION, names[g_bt_state]);
    json_text(file, g_bt_reason);
    fprintf(file, ",\"pairing\":%d,\"paired\":[",
            g_host ? host_pairing_left(g_host) : 0);
    int first = 1;
    for(int index = 0; g_host && index < host_paired_count(g_host); ++index) {
        host_paired_info info;
        char address[18];
        if(!host_paired_get(g_host, index, &info))
            break;
        format_addr(address, info.addr);
        fprintf(file,
                "%s{\"address\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"le\":%s,\"connected\":%s}",
                first ? "" : ",", address, info.vid, info.pid,
                info.le ? "true" : "false",
                info.connected ? "true" : "false");
        first = 0;
    }
    fputs("],\"controllers\":[", file);
    first = 1;
    for(int slot = 0; g_host && slot < HOST_MAX_PADS; ++slot) {
        pad_info info;
        pad_state state;
        char address[18];
        if(!host_pad_get(g_host, slot, &info, &state))
            continue;
        format_addr(address, info.addr);
        fprintf(file,
                "%s{\"slot\":%d,\"profile\":", first ? "" : ",",
                slot + 1);
        json_text(file, info.profile);
        fprintf(file,
                ",\"address\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"virtualPad\":%s}",
                address, info.vid, info.pid,
                vpad_live(slot) ? "true" : "false");
        first = 0;
    }
    fputs("]}\n", file);
    if(fclose(file) == 0)
        (void)rename(temporary, STATUS_PATH);
    else
        (void)unlink(temporary);
}

static int parse_addr(const char *text, unsigned char address[6])
{
    unsigned values[6];
    if(sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x", &values[5],
              &values[4], &values[3], &values[2], &values[1], &values[0]) != 6)
        return 0;
    for(int index = 0; index < 6; ++index)
        address[index] = (unsigned char)values[index];
    return 1;
}

static void read_forget_request(void)
{
    FILE *file = fopen(FORGET_FLAG, "r");
    if(!file)
        return;
    char text[64] = "";
    (void)fgets(text, sizeof(text), file);
    fclose(file);
    (void)unlink(FORGET_FLAG);
    text[strcspn(text, "\r\n ")] = '\0';
    unsigned char address[6];
    if(!g_host || !parse_addr(text, address) || !host_forget(g_host, address))
        log_line("control: forget request rejected: %s", text);
    else
        log_line("control: forgot controller %s", text);
}

static void check_flags(long now)
{
    static long last_second = -1;
    if(now / 1000 == last_second)
        return;
    last_second = now / 1000;
    if(access(STOP_FLAG, F_OK) == 0) {
        (void)unlink(STOP_FLAG);
        g_stop = 1;
    }
    if(access(RETRY_FLAG, F_OK) == 0) {
        (void)unlink(RETRY_FLAG);
        if(g_vpad_ok && g_bt_state == BT_FAILED) {
            g_bt_attempt = 0;
            g_bt_next = 0;
            g_bt_reason[0] = '\0';
            g_bt_state = BT_TRYING;
            if(g_vpad_ok)
                (void)vpad_creds_raise();
            log_line("control: retry requested");
        }
    }
    if(g_host && access(PAIR_FLAG, F_OK) == 0) {
        (void)unlink(PAIR_FLAG);
        (void)host_pair(g_host, PAIR_SECONDS);
        notify("GhostTooth: pairing for 60 seconds. Put the controller in pairing mode.");
    }
    /* A saved record belongs to the live host. Keep the request queued while
     * the radio is down so Retry cannot turn a successful UI response into a
     * silently lost Forget. */
    if(g_host && access(FORGET_FLAG, F_OK) == 0)
        read_forget_request();
}

static void bt_try(long now)
{
    host_events events = {on_connect, on_state, on_disconnect, NULL, on_idle, NULL};
    hci_t hci;
    int attempt = ++g_bt_attempt;
    if(attempt == RAISED_TRIES + 1 && g_vpad_ok) {
        log_line("Bluetooth silent with raised credentials; trying original credentials");
        if(vpad_creds_restore())
            g_bt_plain_creds = 1;
    }
    if(hci_usb_open(&hci)) {
        g_host = host_open(hci, DB_PATH, &events);
        if(g_host) {
            g_bt_state = BT_READY;
            g_bt_reason[0] = '\0';
            log_line("Bluetooth ready after %d attempt(s)%s", attempt,
                     g_bt_plain_creds ? " with original credentials" : "");
            notify("GhostTooth: Bluetooth ready. Pair controllers from the Toolbox.");
            return;
        }
        hci.ops->close(hci.ctx);
    }
    log_line("Bluetooth not ready (attempt %d of %d)", attempt, OPEN_TRIES);
    if(attempt >= OPEN_TRIES) {
        g_bt_state = BT_FAILED;
        snprintf(g_bt_reason, sizeof(g_bt_reason),
                 "The Bluetooth chip did not answer after %d attempts. Use Retry in the Toolbox.",
                 attempt);
        notify("GhostTooth: Bluetooth unavailable. Open the Toolbox for status and Retry.");
    } else {
        g_bt_next = now + 3000;
    }
}

static void bt_lost(void)
{
    log_line("Bluetooth transport lost");
    host_close(g_host);
    g_host = NULL;
    g_bt_state = BT_FAILED;
    snprintf(g_bt_reason, sizeof(g_bt_reason),
             "The Bluetooth transport stopped responding. Use Retry in the Toolbox.");
    notify("GhostTooth: Bluetooth link lost. Use Retry in the Toolbox.");
}

int main(void)
{
    const char *stop_reason = "stop requested";
    long last_power = 0;
    long last_status = 0;
    (void)mkdir(STATE_DIR, 0755);
    (void)mkdir(STATE_DIR "/maps", 0755);
    (void)log_open(LOG_PATH);
    log_line("GhostTooth %s", GHOSTTOOTH_VERSION);
    if(!lock_take(LOCK_PATH)) {
        log_line("another instance is running");
        notify("GhostTooth is already running");
        log_close();
        return 1;
    }
    (void)unlink(STOP_FLAG);
    (void)syscall(SYS_thr_set_name, -1, "ghosttooth");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
    sysinfo_log();

    g_vpad_ok = vpad_init();
    if(!g_vpad_ok) {
        /* vpad_init raises credentials before loading MBus/libScePad. A later
         * startup failure must not leave the long-running process elevated. */
        (void)vpad_creds_restore();
        g_bt_state = BT_FAILED;
        snprintf(g_bt_reason, sizeof(g_bt_reason),
                 "Virtual controllers are unavailable; Bluetooth was left untouched.");
        notify("GhostTooth: virtual controller startup failed. See the Toolbox log.");
    }

    while(!g_stop) {
        if(g_host)
            host_poll(g_host, 4);
        else
            usleep(4000);
        long now = now_ms();
        if(g_vpad_ok)
            vpad_poll(now);
        if(g_vpad_ok && g_bt_state == BT_TRYING && now >= g_bt_next)
            bt_try(now);
        if(g_host && host_transport_lost(g_host))
            bt_lost();
        check_flags(now);
        if(now - last_status >= 250) {
            write_status();
            last_status = now;
        }
        if(now - last_power >= 200) {
            int state = power_state();
            last_power = now;
            if(state == POWER_GOING_TO_REST || state == POWER_IN_REST) {
                stop_reason = "rest mode";
                break;
            }
        }
    }

    log_line("stopping: %s", stop_reason);
    host_close(g_host);
    g_host = NULL;
    vpad_shutdown();
    (void)vpad_creds_restore();
    g_bt_state = BT_STOPPED;
    snprintf(g_bt_reason, sizeof(g_bt_reason), "%s", stop_reason);
    write_status();
    lock_release();
    log_line("stopped");
    log_close();
    return 0;
}

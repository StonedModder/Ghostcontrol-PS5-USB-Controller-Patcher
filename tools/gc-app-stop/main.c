/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gc-app-stop: stop the app-native GhosttoothAPI companion from inside the console.
 *
 * Runs ON the console (sent to the same ELF loader as the service). It:
 *   1. reads /data/ghostcontrol-app-service/runtime/ipc.token,
 *   2. sends "GC1 <token> STOP" to the service's loopback port 39073,
 *   3. reads the pid from runtime-instance.json and reaps it (SIGTERM, SIGKILL),
 *   4. writes everything it did to /data/ghostcontrol-app-service/stop-app.txt.
 *
 * Needed because the app-service can exit its main loop but block in the audio
 * provider release while still holding the singleton flock; the next push then
 * answers "ownership unavailable; refusing without takeover" and only a kill
 * (or reboot) clears it. Touches no kernel memory and no other process.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define IPC_PORT 39073
#define ROOT "/data/ghostcontrol-app-service"
#define TOKEN_PATH ROOT "/runtime/ipc.token"
#define INSTANCE_PATH ROOT "/runtime-instance.json"
#define OUT_PATH ROOT "/stop-app.txt"

static FILE *g_out;

static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_out) {
        fputs(buf, g_out);
        fflush(g_out);
    }
    fputs(buf, stdout);
}

static void read_token(char *out, size_t cap)
{
    out[0] = '\0';
    FILE *f = fopen(TOKEN_PATH, "r");
    if (!f)
        return;
    if (fgets(out, (int)cap, f)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
    }
    fclose(f);
}

static int instance_pid(void)
{
    FILE *f = fopen(INSTANCE_PATH, "r");
    if (!f) {
        say("runtime-instance.json unreadable: %s\n", strerror(errno));
        return 0;
    }
    char body[1024];
    size_t n = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    body[n] = '\0';
    const char *key = strstr(body, "\"pid\"");
    if (!key)
        return 0;
    const char *colon = strchr(key, ':');
    if (!colon)
        return 0;
    int pid = atoi(colon + 1);
    return pid > 1 ? pid : 0;
}

static int ipc_send(const char *line, char *reply, size_t cap, size_t *used)
{
    *used = 0;
    reply[0] = '\0';
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(reply, cap, "<socket: %s>", strerror(errno));
        return -1;
    }
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(IPC_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(reply, cap, "<connect: %s>", strerror(errno));
        close(fd);
        return -1;
    }
    char request[320];
    int want = snprintf(request, sizeof(request), "GC1 %s\n", line);
    if (want <= 0 || (size_t)want >= sizeof(request)) {
        snprintf(reply, cap, "<request too long>");
        close(fd);
        return -1;
    }
    size_t sent = 0;
    while (sent < (size_t)want) {
        ssize_t w = send(fd, request + sent, (size_t)want - sent, 0);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        snprintf(reply, cap, "<send: %s>", strerror(errno));
        close(fd);
        return -1;
    }
    while (*used + 1 < cap) {
        ssize_t r = recv(fd, reply + *used, cap - *used - 1, 0);
        if (r > 0) {
            *used += (size_t)r;
            reply[*used] = '\0';
            if (memchr(reply, '\n', *used))
                break;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    return *used ? 0 : -1;
}

int main(void)
{
    g_out = fopen(OUT_PATH, "a");
    say("== gc-app-stop wall=%lld mypid=%d ==\n", (long long)time(NULL), (int)getpid());

    char token[96];
    read_token(token, sizeof(token));
    say("token: %s\n", token[0] ? "present" : "absent");

    int pid = instance_pid();
    say("instance pid: %d\n", pid);

    char line[256];
    if (token[0])
        snprintf(line, sizeof(line), "%s STOP", token);
    else
        snprintf(line, sizeof(line), "STOP");

    char reply[4096];
    size_t used = 0;
    int rc = ipc_send(line, reply, sizeof(reply), &used);
    say("STOP rc=%d reply=%.400s\n", rc, reply[0] ? reply : "(no answer)");

    usleep(400 * 1000);

    if (pid <= 1 || pid == (int)getpid()) {
        say("no usable pid\n\n");
        if (g_out) fclose(g_out);
        return 0;
    }
    if (kill(pid, 0) != 0) {
        say("pid %d already gone\n\n", pid);
        if (g_out) fclose(g_out);
        return 0;
    }
    if (kill(pid, SIGTERM) == 0)
        say("pid %d SIGTERM sent\n", pid);
    else
        say("pid %d SIGTERM: %s\n", pid, strerror(errno));
    usleep(400 * 1000);
    if (kill(pid, 0) == 0) {
        if (kill(pid, SIGKILL) == 0)
            say("pid %d SIGKILL sent\n", pid);
        else
            say("pid %d SIGKILL: %s\n", pid, strerror(errno));
        usleep(400 * 1000);
        say("pid %d alive after SIGKILL: %s\n\n", pid,
            kill(pid, 0) == 0 ? "yes" : "no");
    } else {
        say("pid %d reaped after SIGTERM\n\n", pid);
    }
    if (g_out) fclose(g_out);
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gc-stop: stop the Ghostcontrol companion service from inside the console.
 *
 * Runs ON the console (sent to the same ELF loader as the service). It:
 *   1. reads /data/ghostcontrol/runtime/ipc.token (no token = tokenless build),
 *   2. sends "GC1 <token> STOP" to the service's loopback IPC port,
 *   3. scans /proc for a surviving ghostcontrol-core process and reaps it
 *      (SIGTERM, then SIGKILL),
 *   4. writes everything it did to /data/ghostcontrol/stop.txt for the PC.
 *
 * Needed because a wedged instance ignores the singleton handoff: the new
 * process starts, fails to bind the IPC port, and both fight over the USB
 * device. The service's own STOP is the graceful path; the kill is the
 * backstop. Touches no kernel memory and no other process.
 */
#include <arpa/inet.h>
#include <dirent.h>
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

#define IPC_PORT 39071
#define ROOT "/data/ghostcontrol"
#define TOKEN_PATH ROOT "/runtime/ipc.token"
#define PID_PATH ROOT "/runtime/core.pid"
#define RUNS_DIR ROOT "/logs/runs"
#define OUT_PATH ROOT "/stop.txt"

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

/* One request line in, one reply line out. Returns 0 when the service answered. */
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
        ssize_t n = send(fd, request + sent, (size_t)want - sent, 0);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        snprintf(reply, cap, "<send: %s>", strerror(errno));
        close(fd);
        return -1;
    }
    while (*used + 1 < cap) {
        ssize_t n = recv(fd, reply + *used, cap - *used - 1, 0);
        if (n > 0) {
            *used += (size_t)n;
            reply[*used] = '\0';
            if (memchr(reply, '\n', *used))
                break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    return *used ? 0 : -1;
}

int main(void)
{
    g_out = fopen(OUT_PATH, "a");
    say("== gc-stop wall=%lld mypid=%d ==\n", (long long)time(NULL), (int)getpid());

    char token[96];
    read_token(token, sizeof(token));
    say("token: %s\n", token[0] ? "present" : "absent");

    char line[256];
    if (token[0])
        snprintf(line, sizeof(line), "%s STOP", token);
    else
        snprintf(line, sizeof(line), "STOP");

    char reply[4096];
    size_t used = 0;
    int rc = ipc_send(line, reply, sizeof(reply), &used);
    say("STOP rc=%d reply=%.400s\n", rc, reply[0] ? reply : "(no answer)");

    /* A wedged instance holds its sockets but services nothing, so a graceful
     * STOP can be useless. Collect the pids it could be from the files it
     * owns: the current pid file, and every run log ever written. */
    int pids[64];
    int pid_count = 0;

    FILE *pf = fopen(PID_PATH, "r");
    if (pf) {
        int pid = 0;
        if (fscanf(pf, "%d", &pid) == 1 && pid > 1 && pid != (int)getpid()) {
            say("core.pid: %d\n", pid);
            pids[pid_count++] = pid;
        }
        fclose(pf);
    } else {
        say("core.pid unreadable: %s\n", strerror(errno));
    }

    DIR *dir = opendir(RUNS_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            const char *mark = strstr(ent->d_name, "-pid");
            if (!mark)
                continue;
            int pid = atoi(mark + 4);
            if (pid <= 1 || pid == (int)getpid())
                continue;
            int seen = 0;
            for (int i = 0; i < pid_count; ++i)
                if (pids[i] == pid) { seen = 1; break; }
            if (!seen && pid_count < (int)(sizeof(pids) / sizeof(pids[0]))) {
                say("run log: %s -> pid %d\n", ent->d_name, pid);
                pids[pid_count++] = pid;
            }
        }
        closedir(dir);
    } else {
        say("runs dir unreadable: %s\n", strerror(errno));
    }

    usleep(300 * 1000);

    int killed = 0;
    for (int i = 0; i < pid_count; ++i) {
        int pid = pids[i];
        if (kill(pid, 0) != 0) {
            say("pid %d already gone\n", pid);
            continue;
        }
        if (kill(pid, SIGTERM) == 0)
            say("pid %d SIGTERM sent\n", pid);
        else
            say("pid %d SIGTERM: %s\n", pid, strerror(errno));
        usleep(250 * 1000);
        if (kill(pid, 0) == 0) {
            if (kill(pid, SIGKILL) == 0)
                say("pid %d SIGKILL sent\n", pid);
            else
                say("pid %d SIGKILL: %s\n", pid, strerror(errno));
            usleep(300 * 1000);
            say("pid %d alive after SIGKILL: %s\n", pid, kill(pid, 0) == 0 ? "yes" : "no");
        } else {
            say("pid %d reaped after SIGTERM\n", pid);
        }
        killed++;
    }
    say("done: candidates=%d acted=%d\n\n", pid_count, killed);
    if (g_out) fclose(g_out);
    return 0;
}

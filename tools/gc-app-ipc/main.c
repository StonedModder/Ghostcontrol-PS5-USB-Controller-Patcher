/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gc-app-ipc: query the app-native GhosttoothAPI companion from inside the
 * console and leave the replies where FTP can read them.
 *
 * Sends GC1 <token> STATUS and GC1 <token> DEVICES to 127.0.0.1:39073 and
 * writes each reply to /data/ghostcontrol-app-service/ipc-<command>.json.
 * Read-only: it issues no state-changing verb.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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

/* One request line in, the whole reply out. Returns 0 when the service answered. */
static int ipc_send(const char *line, char *reply, size_t cap, size_t *used)
{
    *used = 0;
    reply[0] = '\0';
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(IPC_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    char request[320];
    int want = snprintf(request, sizeof(request), "GC1 %s\n", line);
    if (want <= 0 || (size_t)want >= sizeof(request)) {
        close(fd);
        return -1;
    }
    size_t sent = 0;
    while (sent < (size_t)want) {
        ssize_t w = send(fd, request + sent, (size_t)want - sent, 0);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
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

static void dump(const char *command, const char *body, size_t len)
{
    char path[256];
    snprintf(path, sizeof(path), ROOT "/ipc-%s.json", command);
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("write %s failed: %s\n", path, strerror(errno));
        return;
    }
    fwrite(body, 1, len, f);
    fclose(f);
    printf("wrote %s (%zu bytes)\n", path, len);
}

int main(void)
{
    char token[96];
    read_token(token, sizeof(token));
    printf("== gc-app-ipc mypid=%d token=%s\n", (int)getpid(),
           token[0] ? "present" : "absent");

    static char reply[65536];
    /* Read-only verbs only. SETUP_STATUS carries the active draft, its path and
     * the requested target, which is what proves a guided arm took effect. */
    const char *commands[] = {"STATUS", "DEVICES", "SETUP_STATUS"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char line[160];
        if (token[0])
            snprintf(line, sizeof(line), "%s %s", token, commands[i]);
        else
            snprintf(line, sizeof(line), "%s", commands[i]);
        size_t used = 0;
        int rc = ipc_send(line, reply, sizeof(reply), &used);
        printf("%s rc=%d bytes=%zu\n", commands[i], rc, used);
        if (rc == 0)
            dump(commands[i], reply, used);
    }
    return 0;
}

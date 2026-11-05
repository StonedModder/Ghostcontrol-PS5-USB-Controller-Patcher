/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gc-app-save: ask the app-native GhosttoothAPI companion to commit the armed
 * setup draft, and leave the raw reply where FTP can read it.
 *
 * Sends GC1 <token> SETUP_SAVE to 127.0.0.1:39073 and writes the reply to
 * /data/ghostcontrol-app-service/ipc-SETUP_SAVE.json. The point is the error
 * text: the app only shows "save rejected", the service reply carries why.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

static int ipc_send(const char *line, char *reply, size_t cap, size_t *used)
{
    *used = 0;
    reply[0] = '\0';
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {20, 0};
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

int main(void)
{
    char token[96];
    read_token(token, sizeof(token));
    printf("== gc-app-save mypid=%d token=%s\n", (int)getpid(),
           token[0] ? "present" : "absent");

    static char reply[65536];
    char line[160];
    if (token[0])
        snprintf(line, sizeof(line), "%s SETUP_SAVE", token);
    else
        snprintf(line, sizeof(line), "SETUP_SAVE");
    size_t used = 0;
    int rc = ipc_send(line, reply, sizeof(reply), &used);
    printf("SETUP_SAVE rc=%d bytes=%zu\n", rc, used);
    if (rc == 0) {
        FILE *f = fopen(ROOT "/ipc-SETUP_SAVE.json", "w");
        if (f) {
            fwrite(reply, 1, used, f);
            fclose(f);
            printf("wrote " ROOT "/ipc-SETUP_SAVE.json\n");
        }
    }
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gc-app-cmd: send one IPC command to the app-native GhosttoothAPI companion
 * and leave the raw reply where FTP can read it.
 *
 * The command is read from /data/ghostcontrol-app-service/cmd.txt (first line:
 * "VERB [argument]"), so a test can change verbs over FTP without rebuilding
 * or redeploying this payload. Reply goes to ipc-<VERB>.json.
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
#define CMD_PATH ROOT "/cmd.txt"

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
    struct timeval tv = {25, 0};
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
    char request[1024];
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
    char token[96], command[700], verb[64];
    read_token(token, sizeof(token));
    command[0] = verb[0] = '\0';
    FILE *f = fopen(CMD_PATH, "r");
    if (!f) {
        printf("== gc-app-cmd mypid=%d: no " CMD_PATH "\n", (int)getpid());
        return 1;
    }
    if (fgets(command, sizeof(command), f)) {
        size_t n = strlen(command);
        while (n && (command[n - 1] == '\n' || command[n - 1] == '\r'))
            command[--n] = '\0';
    }
    fclose(f);
    if (!command[0]) {
        printf("== gc-app-cmd: empty command file\n");
        return 1;
    }
    sscanf(command, "%63s", verb);
    printf("== gc-app-cmd mypid=%d token=%s verb=%s\n", (int)getpid(),
           token[0] ? "present" : "absent", verb);

    static char reply[262144];
    char line[800];
    if (token[0])
        snprintf(line, sizeof(line), "%s %s", token, command);
    else
        snprintf(line, sizeof(line), "%s", command);
    size_t used = 0;
    int rc = ipc_send(line, reply, sizeof(reply), &used);
    printf("%s rc=%d bytes=%zu\n", verb, rc, used);
    if (rc == 0) {
        char path[256];
        snprintf(path, sizeof(path), ROOT "/ipc-%s.json", verb);
        FILE *out = fopen(path, "w");
        if (out) {
            fwrite(reply, 1, used, out);
            fclose(out);
            printf("wrote %s\n", path);
        }
    }
    return rc ? 1 : 0;
}

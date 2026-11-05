#include "gc_fx_probe.h"

#ifdef __PROSPERO__
#include "../gc_remote.h"
#include "../gc_dualsense_fx_remote.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void ghostpad_status_log(const char *format, ...);

static void hex_line(FILE *fp, intptr_t addr, const uint8_t *data, size_t n)
{
    size_t i;
    fprintf(fp, "%lx:", (unsigned long)addr);
    for (i = 0; i < n; ++i)
        fprintf(fp, " %02x", data[i]);
    fputc('\n', fp);
}

void gc_fx_probe_shellui(pid_t pid, intptr_t libpad, intptr_t motion,
                         const int32_t *users, unsigned user_count)
{
    static int ran;
    FILE *fp;
    uint8_t dump[256];
    uint32_t pad = 0;
    intptr_t get_handle = 0, open_pad = 0;
    GcRemoteSession session;
    int32_t type;
    unsigned u;
    int32_t extra[6];
    unsigned n = 0;
    if (ran)
        return;
    ran = 1;
    if (users) {
        for (u = 0; u < user_count && n < 4; ++u)
            if (users[u] >= 0)
                extra[n++] = users[u];
    }
    extra[n++] = 0;
    extra[n++] = (int32_t)0xffffffff;
    fp = fopen("/data/ghostcontrol/logs/fx-probe.log", "w");
    ghostpad_status_log("[GC FX probe] start pid=%d libpad=0x%lx motion=0x%lx\n",
                        (int)pid, (unsigned long)libpad, (unsigned long)motion);
    if (fp) {
        fprintf(fp, "pid=%d libpad=0x%lx motion=0x%lx\n",
                (int)pid, (unsigned long)libpad, (unsigned long)motion);
        if (motion && gc_remote_read(pid, motion - 64, dump, sizeof(dump)) == 0) {
            size_t off;
            for (off = 0; off < sizeof(dump); off += 16)
                hex_line(fp, motion - 64 + (intptr_t)off, dump + off, 16);
        }
    }
    if (gc_remote_get_library(pid, "libScePad", &pad) != 0)
        goto done;
    get_handle = gc_remote_resolve(pid, pad, "scePadGetHandle");
    open_pad = gc_remote_resolve(pid, pad, "scePadOpen");
    if (fp)
        fprintf(fp, "GetHandle=0x%lx Open=0x%lx\n",
                (unsigned long)get_handle, (unsigned long)open_pad);
    if (!get_handle || gc_remote_session_open(&session, pid) != 0)
        goto done;
    for (u = 0; u < n; ++u) {
        for (type = 0; type <= 3; ++type) {
            uint64_t args[6] = {
                (uint64_t)(uint32_t)extra[u], (uint64_t)(uint32_t)type, 0, 0, 0, 0
            };
            int64_t handle = -1;
            if (gc_remote_session_call(&session, get_handle, args, &handle) != 0) {
                if (fp)
                    fprintf(fp, "GetHandle call fail user=0x%x type=%d\n",
                            extra[u], type);
                goto close_session;
            }
            if (fp)
                fprintf(fp, "GetHandle user=0x%x type=%d -> %lld (i32=%d 0x%x)\n",
                        extra[u], type, (long long)handle, (int32_t)handle,
                        (unsigned)(int32_t)handle);
            ghostpad_status_log("[GC FX probe] GetHandle user=0x%x type=%d i32=%d (0x%x)\n",
                                extra[u], type, (int32_t)handle,
                                (unsigned)(int32_t)handle);
            if (open_pad) {
                int64_t opened = -1;
                uint64_t oargs[6] = {
                    (uint64_t)(uint32_t)extra[u], (uint64_t)(uint32_t)type, 0, 0, 0, 0
                };
                if (gc_remote_session_call(&session, open_pad, oargs, &opened) == 0) {
                    if (fp)
                        fprintf(fp, "Open user=0x%x type=%d -> %lld (i32=%d 0x%x)\n",
                                extra[u], type, (long long)opened, (int32_t)opened,
                                (unsigned)(int32_t)opened);
                    ghostpad_status_log("[GC FX probe] Open user=0x%x type=%d i32=%d (0x%x)\n",
                                        extra[u], type, (int32_t)opened,
                                        (unsigned)(int32_t)opened);
                }
            }
        }
    }
close_session:
    (void)gc_remote_session_close(&session);
done:
    if (fp) {
        fputs("probe done\n", fp);
        fclose(fp);
    }
    ghostpad_status_log("[GC FX probe] done\n");
}
#else
void gc_fx_probe_shellui(pid_t pid, intptr_t libpad, intptr_t motion,
                         const int32_t *users, unsigned user_count)
{
    (void)pid;
    (void)libpad;
    (void)motion;
    (void)users;
    (void)user_count;
}
#endif

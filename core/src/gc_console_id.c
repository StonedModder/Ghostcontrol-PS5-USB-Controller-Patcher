/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_console_id.h"
#include <errno.h>
#include <string.h>
#ifdef __PROSPERO__
#include <sys/sysctl.h>
#endif

extern int sceKernelGetHwModelName(char *);
extern int sceKernelGetHwSerialNumber(char *);
extern int sceKernelGetOpenPsId(void *);
extern int sceKernelGetOpenPsIdForSystem(void *);

static int copy_cstr(char *dst, size_t size, const char *src)
{
    if (!dst || !size || !src || !src[0]) return 0;
    size_t length = strnlen(src, size);
    if (length >= size) return 0;
    memcpy(dst, src, length + 1);
    return 1;
}

static int sysctl_openpsid(const char *name, uint8_t out[16])
{
#ifdef __PROSPERO__
    size_t length = 16;
    memset(out, 0, 16);
    return sysctlbyname(name, out, &length, NULL, 0) == 0 && length == 16;
#else
    (void)name;
    (void)out;
    return 0;
#endif
}

int gc_console_id_read(GcConsoleId *out)
{
    char text[GC_CONSOLE_ID_MODEL_MAX] = "";
    uint8_t id[16] = {0};
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (sceKernelGetHwSerialNumber(text) == 0 &&
        copy_cstr(out->serial, sizeof(out->serial), text))
        out->flags |= GC_CONSOLE_ID_HAS_SERIAL;
    memset(text, 0, sizeof(text));
    if (sceKernelGetHwModelName(text) == 0 &&
        copy_cstr(out->model, sizeof(out->model), text))
        out->flags |= GC_CONSOLE_ID_HAS_MODEL;
    if (sceKernelGetOpenPsId(id) == 0) {
        memcpy(out->openpsid, id, 16);
        out->flags |= GC_CONSOLE_ID_HAS_OPENPSID;
    } else if (sysctl_openpsid("machdep.openpsid", out->openpsid)) {
        out->flags |= GC_CONSOLE_ID_HAS_OPENPSID;
    }
    memset(id, 0, sizeof(id));
    if (sceKernelGetOpenPsIdForSystem(id) == 0) {
        memcpy(out->openpsid_sys, id, 16);
        out->flags |= GC_CONSOLE_ID_HAS_OPENPSID_SYS;
    } else if (sysctl_openpsid("machdep.openpsid_for_sys", out->openpsid_sys)) {
        out->flags |= GC_CONSOLE_ID_HAS_OPENPSID_SYS;
    }
    if (out->flags) return 0;
    errno = ENOENT;
    return -1;
}

void gc_console_id_hex(const uint8_t id[16], char out[33])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = digits[id[i] >> 4];
        out[i * 2 + 1] = digits[id[i] & 15];
    }
    out[32] = '\0';
}

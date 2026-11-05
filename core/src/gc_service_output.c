/* SPDX-License-Identifier: GPL-3.0-or-later
 * Resident lightbar for GhosttoothAPI. Not linked into host tests.
 * Uses the payload pad library only. No SystemService, no process attach.
 */
#include "gc_types.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int32_t sceUserServiceInitialize(void *params);
int32_t sceUserServiceGetInitialUser(int32_t *user);
int32_t scePadInit(void);
int32_t scePadClose(int32_t handle);
int32_t scePadGetHandle(int32_t user, int32_t type, int32_t index);
int32_t scePadOpen(int32_t user, int32_t type, int32_t index, const void *param);
int32_t scePadSetLightBar(int32_t handle, const void *color);
int32_t scePadReadState(int32_t handle, void *data);

typedef struct { uint8_t r, g, b, reserved; } GcPadColor;

static int ready;
static int opened_here;
static int close_uncertain;
static int32_t handle = -1;
static uint64_t retry_at;
static unsigned char last[3] = {255, 255, 255};

static uint64_t pad_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int pad_open(void)
{
    int32_t user = 0;
    int32_t rc;
    uint64_t now;
    if (close_uncertain) return 0;
    if (ready && handle >= 0) return 1;
    now = pad_now();
    if (now && now < retry_at) return 0;
    retry_at = now + 5000ull;
    rc = sceUserServiceInitialize(0);
    if (rc && (uint32_t)rc != 0x80960003u) {
        fprintf(stderr, "resident pad: user service %#x\n", (unsigned)rc);
        return 0;
    }
    rc = sceUserServiceGetInitialUser(&user);
    if (rc) {
        fprintf(stderr, "resident pad: initial user %#x\n", (unsigned)rc);
        return 0;
    }
    rc = scePadInit();
    if (rc) {
        fprintf(stderr, "resident pad: scePadInit %#x\n", (unsigned)rc);
        return 0;
    }
    handle = scePadGetHandle(user, 0, 0);
    opened_here = 0;
    if (handle < 0) {
        handle = scePadOpen(user, 0, 0, NULL);
        if (handle >= 0) opened_here = 1;
    }
    if (handle < 0) {
        fprintf(stderr, "resident pad: no handle %#x\n", (unsigned)handle);
        return 0;
    }
    fprintf(stderr, "resident pad: handle ready%s\n", opened_here ? " via open" : "");
    ready = 1;
    retry_at = 0;
    return 1;
}

int gc_service_pad_read(ScePadData *data)
{
    int32_t rc;
    if (!data || !pad_open()) return 0;
    memset(data, 0, sizeof(*data));
    rc = scePadReadState(handle, data);
    if (rc || !data->connected) return 0;
    return 1;
}

void gc_service_output_apply(const unsigned char rgb[3])
{
    GcPadColor color;
    int32_t rc;

    if (!rgb || !pad_open()) return;
    if (rgb[0] == last[0] && rgb[1] == last[1] && rgb[2] == last[2])
        return;
    color.r = rgb[0];
    color.g = rgb[1];
    color.b = rgb[2];
    color.reserved = 0;
    rc = scePadSetLightBar(handle, &color);
    if (rc) {
        fprintf(stderr, "resident pad: set light %#x\n", (unsigned)rc);
        return;
    }
    last[0] = rgb[0];
    last[1] = rgb[1];
    last[2] = rgb[2];
}

/* Admission and ticks are stopped by the resident owner before this call.
 * Borrowed scePadGetHandle handles are never closed by this owner. */
const char *gc_service_output_release_blocker(void)
{
    return close_uncertain || (opened_here && handle < 0) ? "native_output_handle_uncertain" : NULL;
}
int gc_service_output_close_checked(void)
{
    if (gc_service_output_release_blocker()) return -1;
    if (opened_here && scePadClose(handle) != 0) {
        /* An error does not prove nonconsumption. Keep the ledger, but never
         * use or close this numeric handle again: it may already be reused. */
        close_uncertain = 1;
        return -1;
    }
    opened_here = ready = 0;
    handle = -1;
    retry_at = 0;
    memset(last, 255, sizeof last);
    return 0;
}

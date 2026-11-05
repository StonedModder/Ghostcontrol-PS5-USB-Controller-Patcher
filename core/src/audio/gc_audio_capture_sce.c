/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-07 capture adapter, console backend. See gc_audio_capture_sce.h.
 *
 * This file carries out the platform facts documented in
 * docs/AUDIO-07-CONTRACTS.md section 1, now with every ABI value established
 * from the two implementations that section cites as sources. No third-party
 * code is imported or vendored here: what is used is the published ABI surface
 * only -- the export NIDs, the parameter-block field offsets, the read flags
 * and the status codes -- each cited below at the commit and line it was read
 * from. The console's own copies of these libraries are encrypted SELF, so they
 * cannot be read statically; those two sources are the evidence for the ABI,
 * and the values they agree on are the ones this file uses.
 *
 * SOURCES (read, not imported):
 *   H = X-F1REBALL-X/HearBridge-PS5 @ 4599b05b8f3aeeea6887887f1444db520a8e3059
 *       src/avcap2.c: 21-29 export NIDs; 31-35 status codes; 37 OPEN_PARAM_SIZE
 *       0x38; 39-40 META_SIZE 0x10 and READ_BYTES 0x2000; 48-55 export shapes;
 *       120-127 the parameter block's field writes; 363 read_flag = 1.
 *   F = FGGstore/FGG-PlayPods @ d726e1c32568048aae66f3218c0540adc357e408
 *       src/capture.c: 17-20 KIND_AUDIO 0, status codes, READ_NOWAIT 1; 26-45
 *       the 0x38 parameter block field by field with its size asserted;
 *       47-52 export shapes (open takes a kind, read takes a 16-byte info
 *       record and 32-bit flags); 145,149 two of the NIDs; 158-186 the read
 *       call, its 16-byte info buffer, READ_NOWAIT, and the empty/overrun/
 *       ended handling. src/capture.h: 48 kHz stereo interleaved float32,
 *       records of 1024 frames (8192 bytes).
 *
 * THE FACTS ENCODED HERE (both sources agree unless noted):
 *
 *   1. Load order: /system/common/lib/libSceIpmi.sprx is loaded BEFORE
 *      /system/common/lib/libSceAvcap2.sprx. libSceAvcap2 imports libSceIpmi,
 *      a payload host process does not have it, and left unresolved the first
 *      IPC call kills the process outright (section 1). load_modules() does
 *      the two loads in that order and stops at the first failure.
 *   2. Symbol lookup: the exports are resolved BY NID through
 *      kernel_dynlib_resolve(pid, handle, nid), never by name (H:285-335,
 *      F:69-73,145,149). F uses getpid(), H uses -1; both mean this process.
 *   3. Open: sceAvcap2OpenAudio(out, param, kind) with kind = 0 (audio,
 *      H:37-41, F:17) and a 0x38-byte parameter block (asserted below).
 *   4. Block fields: u64 0x38 at +0x00, u64 0x18 at +0x08, source = 2 (Remote
 *      Play mix) at +0x14, mode = 0 (raw 48 kHz stereo float32) at +0x18,
 *      0xffffffff at +0x1c, the rest zero (H:120-127, F:26-45).
 *   5. Read: sceAvcap2ReadAudio(handle, buf, size, info16, flags) with the
 *      no-wait flag = 1 and a 16-byte info record, asked for one 8192-byte
 *      record = 1024 stereo float32 frames (H:39-40,55,363; F:47-52,160-163).
 *   6. Statuses: 0x81950002 nothing ready yet (not an error), 0x81950004
 *      overrun (the writer lapped us and already resynced: counted as dropped
 *      audio, never fatal), 0x81950008 the service ended the session, and
 *      0x81950001 a sticky invalid state (H restarts the session on it, F
 *      treats it as an error; this backend reports the source gone and leaves
 *      it to the adapter's own restart path) -- H:31-35, F:19-20,175-185.
 *   7. Lifecycle: Initialize -> OpenAudio -> Start -> Read* -> Stop -> Close,
 *      with Terminate available for cleanup when an initialize, open or start
 *      fails (H:169-215), and the restart path in gc_audio_capture.c for "the
 *      service has nothing to capture yet" (F:60-62,188-190).
 *
 * Every value the adapter needs is now established, so
 * gc_audio_capture_sce_backend() returns a usable backend. The refusal path
 * stays for a future value that is not established: an unset ABI field or an
 * empty required NID makes the backend report no service rather than guess.
 */
#include "gc_audio_capture_sce.h"

#ifdef __PROSPERO__

#include <ps5/kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The two modules, in the order section 1 requires. */
#define GC_SCE_IPMI_PATH "/system/common/lib/libSceIpmi.sprx"
#define GC_SCE_AVCAP2_PATH "/system/common/lib/libSceAvcap2.sprx"
#define GC_SCE_AVCAP2_LIBRARY "libSceAvcap2"

/* The parameter block's size IS established (0x38 bytes). */
#define GC_SCE_PARAM_BYTES 0x38u
_Static_assert(GC_SCE_PARAM_BYTES == 0x38u, "section 1: the parameter block is 0x38 bytes");

/* kind = 0 is audio (H:37-41, F:17). */
#define GC_SCE_KIND_AUDIO 0u

/* The service's status codes (H:31-35, F:19-20,179). */
#define GC_SCE_STATUS_INVALID ((int)0x81950001) /* sticky; the session is unusable */
#define GC_SCE_STATUS_NOTHING_YET ((int)0x81950002)
#define GC_SCE_STATUS_OVERRUN ((int)0x81950004)
#define GC_SCE_STATUS_ENDED ((int)0x81950008)

/* "Not established" sentinel, for the fields that could still be unknown (the
 * two offsets are the only ones a future revision could move). The fixed
 * contents are compared by their own meaning, not by this marker: +0x1c really
 * is 0xffffffff, so using it as "unset" would refuse a correct table. */
#define GC_SCE_UNSET 0xffffffffu

/* The read's info record is 16 bytes (H:39, F:160). */
#define GC_SCE_INFO_MAX 0x10u

/* sceKernelLoadStartModule is exported by the SDK's libkernel (checked on this
 * host with nm -D against the ps5-payload-sdk's libkernel_sys.so); the payload
 * SDK does not declare it, so the prototype lives here (H:48, F:23-24). */
extern int sceKernelLoadStartModule(const char *name, size_t argc, const void *argv, uint32_t flags, void *opt,
                                    int *result);

/* The Avcap2 exports, resolved by NID. Shapes: H:48-55 and F:47-52 agree; the
 * handle is 64-bit (H uses int64_t, F uses void *). */
typedef int (*GcSceAvcap2InitializeFn)(void);
typedef int (*GcSceAvcap2TerminateFn)(void);
typedef int (*GcSceAvcap2OpenAudioFn)(void **out, const void *param, uint32_t kind);
typedef int (*GcSceAvcap2HandleFn)(void *handle);
typedef int (*GcSceAvcap2ReadAudioFn)(void *handle, void *buffer, size_t bytes, void *info, uint32_t flags);

typedef struct {
    const char *what;   /* the entry, for logs */
    const char *nid;    /* resolved by NID, never by name */
    intptr_t address;   /* filled by resolve_exports() */
    int required;       /* Terminate is best-effort cleanup only */
} GcSceExport;

/* The ABI values the adapter must have before it may call the service. Each is
 * established and cited; the sentinel stays for anything a future platform
 * revision makes unknown again. */
typedef struct {
    uint32_t read_flag_no_wait;        /* 1: H:363, F:20 */
    uint32_t read_info_bytes;          /* 0x10: H:39, F:160 */
    uint32_t param_size_offset;        /* 0x00: H:123, F:33 */
    uint32_t param_field08_offset;     /* 0x08: H:124, F:34 */
    uint32_t param_source_offset;      /* 0x14: H:125, F:36 */
    uint32_t param_mode_offset;        /* 0x18: F:37 */
    uint32_t param_field1c_offset;     /* 0x1c: H:126, F:38 */
    uint32_t param_size_value;         /* 0x38 */
    uint32_t param_field08_value;      /* 0x18 */
    uint32_t param_source_remote_play; /* 2: Remote Play mix, never muted */
    uint32_t param_mode_raw_float32;   /* 0: raw 48 kHz stereo float32 */
    uint32_t param_field1c_value;      /* 0xffffffff */
} GcSceAvcap2Abi;

/* One capture session: the service handle plus the local read scratch. */
typedef struct {
    void *handle;
    _Alignas(16) uint8_t info[GC_SCE_INFO_MAX];
} GcSceSession;

typedef struct {
    int loaded;
    uint32_t library;   /* libSceAvcap2's dynlib handle in this process */
    uint32_t overruns;  /* overruns this backend itself observed */
    GcSceAvcap2Abi abi;
    GcSceExport exports[7];
    GcSceAvcap2InitializeFn initialize_audio;
    GcSceAvcap2TerminateFn terminate_audio;
    GcSceAvcap2OpenAudioFn open_audio;
    GcSceAvcap2HandleFn start_audio;
    GcSceAvcap2ReadAudioFn read_audio;
    GcSceAvcap2HandleFn stop_audio;
    GcSceAvcap2HandleFn close_audio;
} GcSceAvcap2;

/* The one table: ABI values and NIDs, each cited to the source it was read
 * from. */
static GcSceAvcap2 g_sce = {
    .abi =
        {
            .read_flag_no_wait = 1u,  /* READ_NOWAIT: H:363, F:20 */
            .read_info_bytes = 0x10u, /* H:39, F:160 */
            .param_size_offset = 0x00u,
            .param_field08_offset = 0x08u,
            .param_source_offset = 0x14u,
            .param_mode_offset = 0x18u,
            .param_field1c_offset = 0x1cu,
            .param_size_value = GC_SCE_PARAM_BYTES,
            .param_field08_value = 0x18u,
            .param_source_remote_play = 2u,
            .param_mode_raw_float32 = 0u,
            .param_field1c_value = 0xffffffffu,
        },
    .exports =
        {
            {"sceAvcap2Initialize", "svzPXluOz8U", 0, 1},
            {"sceAvcap2OpenAudio", "3nHpE7Dp5SE", 0, 1},
            {"sceAvcap2Start", "8CGNCwBsItI", 0, 1},
            {"sceAvcap2ReadAudio", "WhsHggqdwcg", 0, 1},
            {"sceAvcap2Stop", "Z5dKK0xnQ8g", 0, 1},
            {"sceAvcap2Close", "tR7gPe1i8hw", 0, 1},
            {"sceAvcap2Terminate", "gojqghDU+1Y", 0, 0}, /* cleanup only (H:24) */
        },
};

#define GC_SCE_EXPORT_COUNT (sizeof(g_sce.exports) / sizeof(g_sce.exports[0]))

static int abi_complete(void)
{
    /* The offsets and the read geometry are what a revision could move; the
     * block's contents are constants (+0x1c is 0xffffffff, so it can never be
     * compared against the sentinel). */
    return g_sce.abi.read_flag_no_wait != GC_SCE_UNSET && g_sce.abi.read_info_bytes != GC_SCE_UNSET &&
           g_sce.abi.read_info_bytes <= GC_SCE_INFO_MAX && g_sce.abi.param_size_offset != GC_SCE_UNSET &&
           g_sce.abi.param_field08_offset != GC_SCE_UNSET && g_sce.abi.param_source_offset != GC_SCE_UNSET &&
           g_sce.abi.param_mode_offset != GC_SCE_UNSET && g_sce.abi.param_field1c_offset != GC_SCE_UNSET &&
           g_sce.abi.param_source_remote_play != GC_SCE_UNSET;
}

static int required_nids_complete(void)
{
    for (size_t i = 0; i < GC_SCE_EXPORT_COUNT; ++i)
        if (g_sce.exports[i].required && (!g_sce.exports[i].nid || !g_sce.exports[i].nid[0])) return 0;
    return 1;
}

/* Writes a 32-bit little-endian value into the parameter block. */
static int put_u32(uint8_t *block, uint32_t offset, uint32_t value)
{
    if ((size_t)offset + 4 > GC_SCE_PARAM_BYTES) return -1;
    for (unsigned i = 0; i < 4; ++i) block[offset + i] = (uint8_t)(value >> (8u * i));
    return 0;
}

/* Writes a 64-bit little-endian value into the parameter block. */
static int put_u64(uint8_t *block, uint32_t offset, uint64_t value)
{
    if ((size_t)offset + 8 > GC_SCE_PARAM_BYTES) return -1;
    for (unsigned i = 0; i < 8; ++i) block[offset + i] = (uint8_t)(value >> (8u * i));
    return 0;
}

/* The 0x38-byte block, "laid out as Remote Play fills it": the five fields the
 * sources pin, everything else zero (H:120-127, F:26-45). */
static int build_param_block(uint8_t block[GC_SCE_PARAM_BYTES])
{
    memset(block, 0, GC_SCE_PARAM_BYTES);
    if (put_u64(block, g_sce.abi.param_size_offset, g_sce.abi.param_size_value)) return -1;
    if (put_u64(block, g_sce.abi.param_field08_offset, g_sce.abi.param_field08_value)) return -1;
    if (put_u32(block, g_sce.abi.param_source_offset, g_sce.abi.param_source_remote_play)) return -1;
    if (put_u32(block, g_sce.abi.param_mode_offset, g_sce.abi.param_mode_raw_float32)) return -1;
    if (put_u32(block, g_sce.abi.param_field1c_offset, g_sce.abi.param_field1c_value)) return -1;
    return 0;
}

/* Load order is the fact section 1 leads with: libSceIpmi first, and if it did
 * not load nothing else happens -- the first Avcap2 IPC call would otherwise
 * terminate this process with no signal. */
static int load_modules(void)
{
    int result = 0;
    const int ipmi = sceKernelLoadStartModule(GC_SCE_IPMI_PATH, 0, NULL, 0, NULL, &result);
    if (ipmi < 0) return -1;
    const int avcap2 = sceKernelLoadStartModule(GC_SCE_AVCAP2_PATH, 0, NULL, 0, NULL, &result);
    if (avcap2 < 0) return -1;
    return 0;
}

static void resolve_exports(void)
{
    const pid_t pid = getpid();
    uint32_t library = 0;
    if (kernel_dynlib_handle(pid, GC_SCE_AVCAP2_LIBRARY, &library) != 0) {
        char sprx[64];
        snprintf(sprx, sizeof(sprx), "%s.sprx", GC_SCE_AVCAP2_LIBRARY);
        if (kernel_dynlib_handle(pid, sprx, &library) != 0) library = 0;
    }
    g_sce.library = library;
    if (!library) return;
    for (size_t i = 0; i < GC_SCE_EXPORT_COUNT; ++i) {
        g_sce.exports[i].address = 0;
        if (!g_sce.exports[i].nid[0]) continue; /* not established: never guessed */
        g_sce.exports[i].address = kernel_dynlib_resolve(pid, library, g_sce.exports[i].nid);
    }
    g_sce.initialize_audio = (GcSceAvcap2InitializeFn)g_sce.exports[0].address;
    g_sce.terminate_audio = (GcSceAvcap2TerminateFn)g_sce.exports[6].address;
    g_sce.open_audio = (GcSceAvcap2OpenAudioFn)g_sce.exports[1].address;
    g_sce.start_audio = (GcSceAvcap2HandleFn)g_sce.exports[2].address;
    g_sce.read_audio = (GcSceAvcap2ReadAudioFn)g_sce.exports[3].address;
    g_sce.stop_audio = (GcSceAvcap2HandleFn)g_sce.exports[4].address;
    g_sce.close_audio = (GcSceAvcap2HandleFn)g_sce.exports[5].address;
}

static int required_exports_resolved(void)
{
    for (size_t i = 0; i < GC_SCE_EXPORT_COUNT; ++i)
        if (g_sce.exports[i].required && !g_sce.exports[i].address) return 0;
    return 1;
}

/* H clears a failed Initialize/Open/Start with Terminate (H:169-215); it is
 * best effort, so a missing or failing one changes nothing. */
static void terminate_if_possible(void)
{
    if (g_sce.terminate_audio) g_sce.terminate_audio();
}

static int sce_initialize(void *ctx)
{
    GcSceAvcap2 *av = ctx;
    if (av->loaded) return 0;
    if (!abi_complete() || !required_nids_complete()) return -1; /* unestablished: refuse */
    if (load_modules() != 0) return -1;
    resolve_exports();
    if (!required_exports_resolved()) return -1; /* an unresolved import is fatal, per section 1 */
    if (av->initialize_audio && av->initialize_audio() != 0) {
        terminate_if_possible();
        if (av->initialize_audio() != 0) return -1; /* one retry, as H:169-186 does */
    }
    av->loaded = 1;
    return 0;
}

static int sce_open(void *ctx, void **session)
{
    GcSceAvcap2 *av = ctx;
    uint8_t param[GC_SCE_PARAM_BYTES];
    if (!av->loaded || !av->open_audio || build_param_block(param)) return -1;
    GcSceSession *fresh = calloc(1, sizeof(*fresh));
    if (!fresh) return -1;
    void *handle = NULL;
    const int status = av->open_audio(&handle, param, GC_SCE_KIND_AUDIO);
    if (status != 0 || !handle) {
        free(fresh);
        terminate_if_possible(); /* H clears the failed open the same way */
        return -1;
    }
    fresh->handle = handle;
    *session = fresh;
    return 0;
}

static int sce_start(void *ctx, void *session)
{
    GcSceAvcap2 *av = ctx;
    GcSceSession *s = session;
    if (!av->loaded || !av->start_audio || !s) return -1;
    if (av->start_audio(s->handle) != 0) {
        if (av->close_audio) av->close_audio(s->handle);
        s->handle = NULL;
        terminate_if_possible();
        return -1;
    }
    return 0;
}

/* The no-wait read (H:363, F:160-190): 0x81950002 is "nothing ready yet" (0 to
 * the adapter), 0x81950004 is an overrun (the adapter counts it as dropped
 * audio), 0x81950008 means the service ended the session, and 0x81950001 is the
 * sticky invalid state -- like F, both are reported as the source being gone. */
static long sce_read(void *ctx, void *session, void *buffer, size_t bytes)
{
    GcSceAvcap2 *av = ctx;
    GcSceSession *s = session;
    if (!av->loaded || !av->read_audio || !s || !buffer || !s->handle) return GC_AUDIO_CAPTURE_READ_FAILED;
    const int status = av->read_audio(s->handle, buffer, bytes, s->info, av->abi.read_flag_no_wait);
    if (status == GC_SCE_STATUS_NOTHING_YET) return 0;
    if (status == GC_SCE_STATUS_OVERRUN) {
        ++av->overruns;
        return GC_AUDIO_CAPTURE_READ_OVERRUN;
    }
    if (status == GC_SCE_STATUS_ENDED || status == GC_SCE_STATUS_INVALID) return GC_AUDIO_CAPTURE_READ_FAILED;
    if (status < 0) return GC_AUDIO_CAPTURE_READ_FAILED;
    return status; /* bytes copied */
}

static int sce_stop(void *ctx, void *session)
{
    GcSceAvcap2 *av = ctx;
    GcSceSession *s = session;
    if (!av->loaded || !av->stop_audio || !s || !s->handle) return -1;
    void *handle = s->handle;
    s->handle = NULL;
    return av->stop_audio(handle) == 0 ? 0 : -1;
}

static void sce_close(void *ctx, void *session)
{
    GcSceAvcap2 *av = ctx;
    GcSceSession *s = session;
    if (!s) return;
    if (av->loaded && av->close_audio && s->handle) av->close_audio(s->handle); /* once: F:196-206 */
    free(s);
}

/* This backend's own tally. Neither source publishes a counter of its own to
 * read here, so the adapter's cross-check reports no unseen overruns for the
 * console backend. */
static uint64_t sce_overruns(void *ctx)
{
    return ((GcSceAvcap2 *)ctx)->overruns;
}

static const GcAudioCaptureBackend g_sce_backend = {
    &g_sce, "sce-avcap2", sce_initialize, sce_open, sce_start, sce_read, sce_stop, sce_close, sce_overruns,
};

const GcAudioCaptureBackend *gc_audio_capture_sce_backend(void)
{
    if (!abi_complete() || !required_nids_complete()) return NULL; /* unestablished: no service, never a guess */
    return &g_sce_backend;
}

#endif /* __PROSPERO__ */

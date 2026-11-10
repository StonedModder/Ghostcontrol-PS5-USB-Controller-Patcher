/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AUDIO-07 capture adapter: a GcAudioProvider filled over the console's own
 * capture service (libSceAvcap2), the way Remote Play uses it.
 *
 * This is the adapter, not the service. Every fact about the service lives
 * behind one small injectable backend table (load/open/start/read/stop/
 * close/overruns), so a HOST test can fake the service completely and the
 * host build contains no console-only code. The console implementation of
 * that table is a separate file, gc_audio_capture_sce.c, compiled only for
 * the target.
 *
 * Facts encoded here (docs/AUDIO-07-CONTRACTS.md section 1):
 *
 *   - The stream is raw 48 kHz stereo interleaved float32 in records of
 *     8192 bytes = 1024 stereo frames (32 ms).
 *   - The service's no-wait read answers "nothing ready yet" and, when the
 *     writer lapped us, an overrun. An overrun is DATA LOSS: it is counted
 *     and surfaced (stats.overruns / stats.dropped_frames), never hidden and
 *     never fatal.
 *   - The provider is an explicitly authorized external source:
 *     scope = GC_AUDIO_SCOPE_AUTHORIZED_EXTERNAL, consent_required = 1.
 *   - A missing or refusing service is source_unavailable. There is no
 *     fallback capture, no process attach and no game detection here; the
 *     adapter never opens anything the caller did not ask it to open.
 *
 * Seam obligations (docs/AUDIO-CONTRACT.md) this adapter satisfies:
 *
 *   - read runs without the caller's lock, and the adapter's own lock is
 *     never held across a backend call. One read at a time per handle (a
 *     second concurrent read on the same handle is refused, not raced).
 *   - cancel may run concurrently with a read and makes it return: the
 *     adapter's read only ever waits on a bounded, cancellable condition
 *     wait between no-wait service polls.
 *   - close always follows cancel and never overlaps a read; a close while a
 *     read is in flight is refused and counted (it would close the service
 *     session under the reader).
 *   - every handle is distinct from every handle not yet closed (a slot is
 *     never reissued before close).
 *   - every callback is bounded: at most GC_AUDIO_CAPTURE_POLLS_PER_READ
 *     service reads per provider read, each preceded by a wait of at most
 *     GC_AUDIO_CAPTURE_POLL_US.
 *
 * What is host-proven and what is not is stated in
 * docs/AUDIO-07-CAPTURE-ADAPTER.md. The ABI facts of the platform are used
 * as published; no sibling project's source is imported.
 */
#ifndef GC_AUDIO_CAPTURE_H
#define GC_AUDIO_CAPTURE_H

#include "gc_audio.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The provider's self-description. `[a-z0-9_-]{1,31}`, per the module. */
#define GC_AUDIO_CAPTURE_ID "sce-avcap2"

#define GC_AUDIO_CAPTURE_RATE 48000u
#define GC_AUDIO_CAPTURE_CHANNELS 2u

/* One service record: 8192 bytes = 1024 stereo float32 frames. */
#define GC_AUDIO_CAPTURE_RECORD_BYTES 8192u
#define GC_AUDIO_CAPTURE_FRAME_BYTES 8u
#define GC_AUDIO_CAPTURE_RECORD_FRAMES (GC_AUDIO_CAPTURE_RECORD_BYTES / GC_AUDIO_CAPTURE_FRAME_BYTES)

/* Bounded-callback policy. The service read is no-wait, so the adapter may
 * poll; it never polls more than this per provider read, and waits at most
 * this long between polls (well under one record period, 32 ms). */
#define GC_AUDIO_CAPTURE_POLLS_PER_READ 4u
#define GC_AUDIO_CAPTURE_POLL_US 2000u

/* Consecutive "nothing ready yet" polls before the restart path (section 1
 * lifecycle) tears the session down and opens a fresh one, and how many
 * times one handle may do that. */
#define GC_AUDIO_CAPTURE_SILENT_POLLS 8u
#define GC_AUDIO_CAPTURE_MAX_RESTARTS 3u

/* Backend read outcomes. Anything >= 0 is a byte count; anything below is an
 * outcome the adapter maps onto the seam. */
enum {
    GC_AUDIO_CAPTURE_READ_FAILED = -1,  /* service refused / its session is gone */
    GC_AUDIO_CAPTURE_READ_OVERRUN = -2  /* the writer lapped us (data loss, counted) */
};

/* The injectable service. A backend with any callback missing is treated as
 * ABSENT, never half-used: describe() then says unavailable and open()
 * refuses. `ctx` is the backend's own object, passed back verbatim.
 *
 *   initialize  load/resolve/open the service side (console: load
 *               /system/common/lib/libSceIpmi.sprx BEFORE
 *               /system/common/lib/libSceAvcap2.sprx and resolve the Avcap2
 *               exports BY NID). Called once, by gc_audio_capture_init().
 *               Returns 0 on success.
 *   open        open one capture session (console: sceAvcap2OpenAudio with
 *               kind = 0 and the 0x38-byte parameter block, source = 2
 *               "Remote Play mix", mode = 0 raw 48 kHz stereo float32) and
 *               hand back its session pointer in *session. Returns 0 on
 *               success; a failure hands out no session.
 *   start       start that session's stream. Returns 0 on success.
 *   read        the no-wait record read: fills at most `bytes` (which is
 *               exactly GC_AUDIO_CAPTURE_RECORD_BYTES) and returns the byte
 *               count (a whole number of stereo frames, never more than
 *               `bytes`), 0 for "nothing ready yet", or one of the two
 *               outcomes above. It never blocks.
 *   stop        stop the session's stream; called at most once per session.
 *   close       close the session and free its resources; called at most
 *               once per session, after stop.
 *   overruns    the service's own overrun counter, for cross-checking what
 *               the no-wait read reported, or NULL. Not an error source.
 */
typedef struct {
    void *ctx;                   /* the backend's own object, passed back verbatim */
    const char *name;
    int (*initialize)(void *ctx);
    int (*open)(void *ctx, void **session);
    int (*start)(void *ctx, void *session);
    long (*read)(void *ctx, void *session, void *buffer, size_t bytes);
    int (*stop)(void *ctx, void *session);
    void (*close)(void *ctx, void *session);
    uint64_t (*overruns)(void *ctx);
} GcAudioCaptureBackend;

/* Evidence. Counts are the adapter's own tally of what it asked the service
 * for and what the service answered; `service_overruns`/`unseen_overruns`
 * are the cross-check against the service's own counter. Packets/records
 * counted here are NOT a measurement of fidelity or latency. */
typedef struct {
    uint64_t init_calls;
    uint64_t init_failures;
    uint64_t opens;
    uint64_t starts;
    uint64_t stops;
    uint64_t service_closes;     /* service sessions closed */
    uint64_t refused_opens;      /* no backend, or no free slot */
    uint64_t failed_opens;       /* the service refused to open */
    uint64_t failed_starts;
    uint64_t closes;
    uint64_t stale_closes;       /* close of a handle that is not open */
    uint64_t closes_during_read; /* refused: would close under the reader */
    uint64_t closes_without_cancel;
    uint64_t cancels;
    uint64_t stale_cancels;      /* cancel of a handle that is not open */
    uint64_t cancelled_reads;    /* reads that returned because they were cancelled */
    uint64_t stale_reads;        /* read of a handle that is not open */
    uint64_t overlapping_reads;  /* a second concurrent read on the same handle */
    uint64_t reads;              /* no-wait service reads issued */
    uint64_t records;            /* well-formed records received */
    uint64_t frames;             /* stereo frames handed to the consumer */
    uint64_t overruns;           /* overruns THIS adapter saw (data loss) */
    uint64_t dropped_frames;     /* >= overruns * RECORD_FRAMES */
    uint64_t service_overruns;   /* the service's own counter, last sampled */
    uint64_t unseen_overruns;    /* service counter above what we saw */
    uint64_t rejected_records;   /* short/long/non-frame service records */
    uint64_t restarts;           /* restart path: session reopened */
    uint64_t discarded_frames;   /* staged frames dropped unread at close */
    GcAudioError last_error;
    int available;               /* backend present and initialized */
} GcAudioCaptureStats;

/* One open provider handle: its own service session, its own staging.
 * Private; a handle is a pointer to one of these, and a slot is never
 * reissued before its close. `stage` is a union so the float view is
 * properly aligned for the backend's byte view. */
typedef struct {
    int in_use;
    int closing;                 
    int reading;
    int cancelled;
    int started;
    void *service;
    uint64_t generation;
    uint64_t next_sample;
    unsigned silent_polls;
    unsigned restarts;
    size_t staged;
    union {
        float f[GC_AUDIO_CAPTURE_RECORD_FRAMES * GC_AUDIO_CAPTURE_CHANNELS];
        unsigned char bytes[GC_AUDIO_CAPTURE_RECORD_BYTES];
    } stage;
} GcAudioCaptureSlot;

/* Private state, like GcAudio's: read it through gc_audio_capture_stats(). */
typedef struct {
    const GcAudioCaptureBackend *backend;
    int available;
    pthread_mutex_t mutex;
    pthread_cond_t ready;        /* broadcast by cancel to wake a waiting read */
    GcAudioCaptureStats stats;
    GcAudioCaptureSlot slots[4];
} GcAudioCapture;

/* Calls backend->initialize() once (counted). A NULL or incomplete backend,
 * or one that refuses to initialize, leaves the adapter reporting
 * source_unavailable: it never substitutes another source. Returns 0 when
 * the adapter is usable, -1 otherwise (the object is still valid either way,
 * so a caller can hand it to the module and let the module report the seam). */
int gc_audio_capture_init(GcAudioCapture *capture, const GcAudioCaptureBackend *backend);
/* After every call inside this object has left (the module guarantees it
 * does that at gc_audio_shutdown); frees the lock. Does not tear a session
 * down: the module closes every handle it opened. */
void gc_audio_capture_destroy(GcAudioCapture *capture);

/* The seam. All five callbacks are always non-NULL, so the module keeps this
 * provider and reports what describe() says (unavailable when there is no
 * service). */
GcAudioProvider gc_audio_capture_provider(GcAudioCapture *capture);

/* Evidence readback; samples the service's overrun counter when the backend
 * offers one. Never opens, reads or starts anything. */
void gc_audio_capture_stats(GcAudioCapture *capture, GcAudioCaptureStats *out);

#ifdef __cplusplus
}
#endif

#endif

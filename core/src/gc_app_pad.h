/* App-owned pad contract v1. No platform APIs in the core. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GC_APP_PAD_H
#define GC_APP_PAD_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_APP_PAD_POLL "APP_PAD_POLL"
#define GC_APP_PAD_ACK "APP_PAD_ACK"
#define GC_APP_PAD_WIRE_MAX 512
#define GC_APP_PAD_LEASE_MS 1500u
#define GC_APP_PAD_TTL_MS 5000u
#define GC_APP_PAD_CAP_LIGHT 1u
#define GC_APP_PAD_CAP_RUMBLE 2u
#define GC_APP_PAD_CAP_INPUT 4u
#define GC_APP_PAD_NOT_CALLED (-2147483647)
enum { GC_APP_PAD_IDLE, GC_APP_PAD_PENDING, GC_APP_PAD_STARTED, GC_APP_PAD_COMPLETE,
       GC_APP_PAD_FAILED, GC_APP_PAD_CANCELLED, GC_APP_PAD_EXPIRED };
/* Poll args: session|connection_generation|connected|caps|read_rc|handle_rc.
 * session/gen >0; connected 0/1; caps mask 0..7. No handle crosses IPC. */
typedef struct { uint64_t session, generation; unsigned connected, caps; int read_rc, handle_rc; } GcAppPadPoll;
/* ACK args: session|generation|boot|revision|light_rc|event_id|phase|mode_rc|start_rc|stop_rc|reset_rc.
 * phase 0=light only, 1=started, 2=complete, 3=failed, 4=cancelled.
 * An rc not actually called MUST be GC_APP_PAD_NOT_CALLED, not zero. */
typedef struct { uint64_t session,generation,boot,revision; int light_rc;
 uint64_t event_id; unsigned phase; int mode_rc,start_rc,stop_rc,reset_rc; } GcAppPadAck;
/* Reply data (20 decimal fields): boot|revision|enabled|effect|period_ms|step_ms|r|g|b|
 * lease_ms|event_id|event_state|ttl_remaining_ms|large|small|on_ms|off_ms|repeats|stop|generation.
 * enabled is lightbar-only. effect: 0 Solid, 1 Breathe, 2 Rainbow, 3 Police, 4 Strobe.
 * Event delivered only to connected rumble-capable app; rumble ignores enabled.
 * TTL is acceptance time, not pulse duration. lease must renew during playback. */
typedef struct { uint64_t boot,revision; unsigned enabled,effect,period_ms,step_ms,r,g,b,lease_ms;
 uint64_t event_id; unsigned event_state,ttl_remaining_ms,large,small,on_ms,off_ms,repeats,stop;
 uint64_t generation; } GcAppPadReply;
typedef struct {
 uint64_t boot,revision,session,generation,last_poll_ms,next_id,event_id,event_created_ms,event_started_ms;
 unsigned enabled,effect,period_ms,step_ms,r,g,b,connected,caps,event_state,delivered;
 unsigned large,small,on_ms,off_ms,repeats;
 uint64_t light_ack_revision;
 int read_rc,handle_rc,light_rc,mode_rc,start_rc,stop_rc,reset_rc;
} GcAppPad;
/* Single-owner, no internal locks: parent serializes all calls with one mutex.
 * All now values are core monotonic ms, nondecreasing. boot nonzero/unique per core lifetime. */
void gc_app_pad_init(GcAppPad *, uint64_t boot);
int gc_app_pad_configure(GcAppPad *, unsigned enabled,unsigned effect,unsigned period_ms,
                        unsigned step_ms,unsigned r,unsigned g,unsigned b);
/* Parent gates service-running state and explicitly stops rumble on service STOP.
 * Lightbar enabled never authorizes or denies rumble.
 * Reject busy rather than silently replace an unacknowledged event. Motors 0..255,
 * on 1..2000ms, off 0..2000ms, repeats 1..16, total duration <=10000ms. */
int gc_app_pad_schedule(GcAppPad *,uint64_t now,unsigned large,unsigned small,
                       unsigned on_ms,unsigned off_ms,unsigned repeats);
void gc_app_pad_stop(GcAppPad *);
void gc_app_pad_status(GcAppPad *,uint64_t now,GcAppPad *out);
int gc_app_pad_poll(GcAppPad *,uint64_t now,const GcAppPadPoll *,GcAppPadReply *);
int gc_app_pad_ack(GcAppPad *,uint64_t now,const GcAppPadAck *);
/* Standalone bounded handler. Returns 0 success, -1 invalid/unsupported/stale/busy.
 * Reply is one JSON line: {"ok":true,"data":"<reply fields>"}\n for POLL,
 * {"ok":true}\n for ACK; error {"ok":false,"error":"app_pad_rejected"}\n.
 * Input must be NUL-terminated within GC_APP_PAD_WIRE_MAX bytes. Output cap >=512.
 * On invalid input no state mutation (except time-driven expiry on valid requests). */
int gc_app_pad_command(GcAppPad *,uint64_t now,const char *command,const char *argument,
                       char *output,size_t capacity);
int gc_app_pad_parse_reply(const char *data,GcAppPadReply *);
int gc_app_pad_format_poll(const GcAppPadPoll *,char *,size_t);
int gc_app_pad_format_ack(const GcAppPadAck *,char *,size_t);
#ifdef __cplusplus
}
#endif
#endif

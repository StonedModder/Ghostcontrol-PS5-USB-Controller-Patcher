/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gc_app_pad.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

static int live(const GcAppPad *s) { return s->event_state==GC_APP_PAD_PENDING || s->event_state==GC_APP_PAD_STARTED; }
static void expire(GcAppPad *s,uint64_t now) {
 if(s->session && now-s->last_poll_ms>=GC_APP_PAD_LEASE_MS) {
  s->connected=0;
  if(live(s) && s->delivered) s->event_state=GC_APP_PAD_CANCELLED;
 }
 if(s->event_state==GC_APP_PAD_PENDING && now-s->event_created_ms>=GC_APP_PAD_TTL_MS)
  s->event_state=GC_APP_PAD_EXPIRED;
 if(s->event_state==GC_APP_PAD_STARTED && now-s->event_started_ms>=
    (uint64_t)s->on_ms*s->repeats+(uint64_t)s->off_ms*(s->repeats-1)+GC_APP_PAD_LEASE_MS)
  s->event_state=GC_APP_PAD_EXPIRED;
}
void gc_app_pad_init(GcAppPad *s,uint64_t boot) {
 memset(s,0,sizeof(*s)); s->boot=boot; s->revision=1; s->enabled=1;
 s->period_ms=2000; s->step_ms=32;
 s->read_rc=s->handle_rc=s->light_rc=s->mode_rc=s->start_rc=s->stop_rc=s->reset_rc=GC_APP_PAD_NOT_CALLED;
}
void gc_app_pad_stop(GcAppPad *s) { if(live(s)) s->event_state=GC_APP_PAD_CANCELLED; }
int gc_app_pad_configure(GcAppPad *s,unsigned enabled,unsigned effect,unsigned period,unsigned step,unsigned r,unsigned g,unsigned b) {
 if(enabled>1 || effect>4 || period<100 || period>60000 || step<16 || step>1000 || r>255 || g>255 || b>255 || s->revision==UINT64_MAX) return -1;
 s->enabled=enabled; s->effect=effect; s->period_ms=period; s->step_ms=step; s->r=r; s->g=g; s->b=b;
 ++s->revision; s->light_ack_revision=0; s->light_rc=GC_APP_PAD_NOT_CALLED;
 return 0;
}
int gc_app_pad_schedule(GcAppPad *s,uint64_t now,unsigned large,unsigned small,unsigned on,unsigned off,unsigned repeats) {
 if(large>255 || small>255 || (!large&&!small) || !on || on>2000 || off>2000 || !repeats || repeats>16 ||
    (uint64_t)on*repeats+(uint64_t)off*(repeats-1)>10000 || s->next_id==UINT64_MAX) return -1;
 expire(s,now); if(live(s)) return -1;
 s->event_id=++s->next_id; s->event_created_ms=now; s->event_state=GC_APP_PAD_PENDING;
 s->delivered=0; s->large=large; s->small=small; s->on_ms=on; s->off_ms=off; s->repeats=repeats;
 s->mode_rc=s->start_rc=s->stop_rc=GC_APP_PAD_NOT_CALLED; return 0;
}
void gc_app_pad_status(GcAppPad *s,uint64_t now,GcAppPad *out) { expire(s,now); *out=*s; }
int gc_app_pad_poll(GcAppPad *s,uint64_t now,const GcAppPadPoll *p,GcAppPadReply *r) {
 if(!s->boot || !p->session || !p->generation || p->connected>1 || p->caps>7) return -1;
 if(s->session && p->session!=s->session && now-s->last_poll_ms<GC_APP_PAD_LEASE_MS) return -1;
 if(p->session==s->session && p->generation<s->generation) return -1;
 expire(s,now);
 if((s->session!=p->session || s->generation!=p->generation || !p->connected) && live(s) && s->delivered)
  s->event_state=GC_APP_PAD_CANCELLED;
 if(s->session!=p->session || s->generation!=p->generation) { s->light_ack_revision=0; s->light_rc=GC_APP_PAD_NOT_CALLED; }
 s->session=p->session; s->generation=p->generation; s->last_poll_ms=now;
 s->connected=p->connected; s->caps=p->caps; s->read_rc=p->read_rc; s->handle_rc=p->handle_rc;
 memset(r,0,sizeof(*r));
 r->boot=s->boot; r->revision=s->revision; r->enabled=s->enabled; r->effect=s->effect;
 r->period_ms=s->period_ms; r->step_ms=s->step_ms; r->r=s->r; r->g=s->g; r->b=s->b;
 r->lease_ms=GC_APP_PAD_LEASE_MS; r->generation=p->generation;
 r->event_state=s->event_state; r->stop=!live(s);
 if(s->event_id && p->connected && (p->caps&GC_APP_PAD_CAP_RUMBLE)) {
  r->event_id=s->event_id; r->large=s->large; r->small=s->small; r->on_ms=s->on_ms; r->off_ms=s->off_ms; r->repeats=s->repeats;
  if(s->event_state==GC_APP_PAD_PENDING) { r->ttl_remaining_ms=(unsigned)(GC_APP_PAD_TTL_MS-(now-s->event_created_ms)); s->delivered=1; }
 }
 return 0;
}
int gc_app_pad_ack(GcAppPad *s,uint64_t now,const GcAppPadAck *a) {
 if(a->session!=s->session || a->generation!=s->generation || a->boot!=s->boot || a->revision!=s->revision || a->phase>4 ||
    !s->session || now-s->last_poll_ms>=GC_APP_PAD_LEASE_MS ||
    (a->phase && (a->event_id!=s->event_id || !s->delivered))) return -1;
 if(a->phase==1 && !live(s)) return -1;
 if(s->event_state==GC_APP_PAD_COMPLETE && a->phase && a->phase!=2) return -1;
 if(a->phase==1 && (a->mode_rc!=0 || a->start_rc!=0)) return -1;
 if(a->phase==2 && (a->mode_rc!=0 || a->start_rc!=0 || a->stop_rc!=0)) return -1;
 /* phase3 can have zero latest rc after a successful cleanup: pattern still failed. */
 s->light_ack_revision=a->revision; s->light_rc=a->light_rc; s->reset_rc=a->reset_rc;
 if(a->phase) {
  if(a->phase==1 && s->event_state==GC_APP_PAD_PENDING) s->event_started_ms=now;
  s->mode_rc=a->mode_rc; s->start_rc=a->start_rc; s->stop_rc=a->stop_rc;
  if(live(s)) s->event_state=a->phase==1?GC_APP_PAD_STARTED:a->phase==2?GC_APP_PAD_COMPLETE:a->phase==3?GC_APP_PAD_FAILED:GC_APP_PAD_CANCELLED;
 }
 return 0;
}
/* Strict decimal parser, bounded scanning and checked accumulation. */
static int fields(const char *s,uint64_t *v,size_t n,uint32_t signed_mask) {
 size_t pos=0;
 if(!s) return -1;
 for(size_t i=0;i<n;i++) {
  int neg=0; uint64_t x=0;
  if(pos>=GC_APP_PAD_WIRE_MAX) return -1;
  if(s[pos]=='-' && (signed_mask&(1u<<i))) { neg=1; ++pos; }
  if(pos>=GC_APP_PAD_WIRE_MAX || s[pos]<'0' || s[pos]>'9') return -1;
  while(pos<GC_APP_PAD_WIRE_MAX && s[pos]>='0' && s[pos]<='9') {
   unsigned d=(unsigned)(s[pos++]-'0'); if(x>(UINT64_MAX-d)/10) return -1; x=x*10+d;
  }
  if(pos>=GC_APP_PAD_WIRE_MAX) return -1;
  if(signed_mask&(1u<<i)) { if(x>(neg?(uint64_t)INT_MAX+1:INT_MAX)) return -1; v[i]=neg?(uint64_t)(-(int64_t)x):x; }
  else v[i]=x;
  if(i+1<n) { if(s[pos++]!='|') return -1; } else if(s[pos]!=0) return -1;
 }
 return 0;
}
static int fits(const uint64_t *v,unsigned begin,unsigned end) {
 for(unsigned i=begin;i<=end;i++) if(v[i]>UINT_MAX) return 0;
 return 1;
}
int gc_app_pad_parse_reply(const char *data,GcAppPadReply *r) {
 uint64_t v[20]; if(fields(data,v,20,0) || !fits(v,2,9) || !fits(v,11,18)) return -1;
 GcAppPadReply t={v[0],v[1],(unsigned)v[2],(unsigned)v[3],(unsigned)v[4],(unsigned)v[5],(unsigned)v[6],(unsigned)v[7],(unsigned)v[8],(unsigned)v[9],v[10],(unsigned)v[11],(unsigned)v[12],(unsigned)v[13],(unsigned)v[14],(unsigned)v[15],(unsigned)v[16],(unsigned)v[17],(unsigned)v[18],v[19]};
 if(!t.boot || !t.revision || !t.generation || t.enabled>1 || t.effect>4 || t.period_ms<100 || t.period_ms>60000 || t.step_ms<16 || t.step_ms>1000 ||
    t.r>255 || t.g>255 || t.b>255 || t.lease_ms!=GC_APP_PAD_LEASE_MS || t.event_state>6 || t.ttl_remaining_ms>GC_APP_PAD_TTL_MS || t.stop>1 ||
    t.large>255 || t.small>255 || (t.event_id && (!t.on_ms || t.on_ms>2000 || t.off_ms>2000 || !t.repeats || t.repeats>16 ||
    (uint64_t)t.on_ms*t.repeats+(uint64_t)t.off_ms*(t.repeats-1)>10000))) return -1;
 *r=t; return 0;
}
#define DONE(n,c) ((n)<0 || (size_t)(n)>=(c)?-1:0)
int gc_app_pad_format_poll(const GcAppPadPoll *p,char *out,size_t cap) {
 int n=snprintf(out,cap,"%" PRIu64 "|%" PRIu64 "|%u|%u|%d|%d",p->session,p->generation,p->connected,p->caps,p->read_rc,p->handle_rc); return DONE(n,cap);
}
int gc_app_pad_format_ack(const GcAppPadAck *a,char *out,size_t cap) {
 int n=snprintf(out,cap,"%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%d|%" PRIu64 "|%u|%d|%d|%d|%d",a->session,a->generation,a->boot,a->revision,a->light_rc,a->event_id,a->phase,a->mode_rc,a->start_rc,a->stop_rc,a->reset_rc); return DONE(n,cap);
}
int gc_app_pad_command(GcAppPad *s,uint64_t now,const char *cmd,const char *arg,char *out,size_t cap) {
 uint64_t v[20]; int rc=-1; GcAppPadReply r;
 if(!out || cap<GC_APP_PAD_WIRE_MAX) return -1;
 if(cmd && !strcmp(cmd,GC_APP_PAD_POLL) && !fields(arg,v,6,(1u<<4)|(1u<<5)) && fits(v,2,3)) {
  GcAppPadPoll p={v[0],v[1],(unsigned)v[2],(unsigned)v[3],(int)(int64_t)v[4],(int)(int64_t)v[5]};
  rc=gc_app_pad_poll(s,now,&p,&r);
  if(!rc) {
   int n=snprintf(out,cap,"{\"ok\":true,\"data\":\"%" PRIu64 "|%" PRIu64 "|%u|%u|%u|%u|%u|%u|%u|%u|%" PRIu64 "|%u|%u|%u|%u|%u|%u|%u|%u|%" PRIu64 "\"}\n",r.boot,r.revision,r.enabled,r.effect,r.period_ms,r.step_ms,r.r,r.g,r.b,r.lease_ms,r.event_id,r.event_state,r.ttl_remaining_ms,r.large,r.small,r.on_ms,r.off_ms,r.repeats,r.stop,r.generation);
   return DONE(n,cap);
  }
 } else if(cmd && !strcmp(cmd,GC_APP_PAD_ACK) && !fields(arg,v,11,(1u<<4)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<10)) && v[6]<=4) {
  GcAppPadAck a={v[0],v[1],v[2],v[3],(int)(int64_t)v[4],v[5],(unsigned)v[6],(int)(int64_t)v[7],(int)(int64_t)v[8],(int)(int64_t)v[9],(int)(int64_t)v[10]};
  rc=gc_app_pad_ack(s,now,&a);
 }
 snprintf(out,cap,rc?"{\"ok\":false,\"error\":\"app_pad_rejected\"}\n":"{\"ok\":true}\n"); return rc;
}

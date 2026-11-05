// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_pad_bridge.hpp"
#include <cstddef>
#include <cstring>
namespace {
struct RawPadSample {
 std::uint32_t buttons; std::uint8_t left_x,left_y,right_x,right_y,l2,r2,reserved0[66];
 std::int32_t connected; std::uint64_t timestamp_us; std::uint8_t extension[16],connected_count,reserved1[15];
};
static_assert(sizeof(RawPadSample)==120);
static_assert(offsetof(RawPadSample,connected)==0x4c);
static_assert(offsetof(RawPadSample,timestamp_us)==0x50);
static_assert(offsetof(RawPadSample,connected_count)==0x68);
}
#ifdef GC_NATIVE_PAD_PS5
extern "C" {
 int sceUserServiceGetInitialUser(int *);
 int scePadGetHandle(int,int,int);
 int scePadRead(int,RawPadSample *,int);
 int scePadSetLightBar(int,const ghostcontrol::PadColor *);
 int scePadResetLightBar(int);
 int scePadSetVibrationMode(int,int);
 int scePadSetVibration(int,const ghostcontrol::PadVibration *);
}
#endif
namespace ghostcontrol {
int NativePadBackend::acquire() {
#ifdef GC_NATIVE_PAD_PS5
 int user=-1; int rc=sceUserServiceGetInitialUser(&user);
 if(rc!=0) {handle_=-1;return rc<0?rc:-1;}
 handle_=scePadGetHandle(user,0,0); return handle_;
#else
 return -1;
#endif
}
int NativePadBackend::light(PadColor c) {
#ifdef GC_NATIVE_PAD_PS5
 return handle_<0?-1:scePadSetLightBar(handle_,&c);
#else
 (void)c; return -1;
#endif
}
int NativePadBackend::reset_light() {
#ifdef GC_NATIVE_PAD_PS5
 return handle_<0?-1:scePadResetLightBar(handle_);
#else
 return -1;
#endif
}
int NativePadBackend::mode() {
#ifdef GC_NATIVE_PAD_PS5
 return handle_<0?-1:scePadSetVibrationMode(handle_,2);
#else
 return -1;
#endif
}
int NativePadBackend::vibration(PadVibration v) {
#ifdef GC_NATIVE_PAD_PS5
 return handle_<0?-1:scePadSetVibration(handle_,&v);
#else
 (void)v; return -1;
#endif
}
void NativePadBackend::release() {handle_=-1;}
int NativePadBackend::read(PadInput *samples,std::size_t capacity) {
 if(!samples || !capacity || capacity>64) return -1;
#ifdef GC_NATIVE_PAD_PS5
 if(handle_<0) return -1;
 RawPadSample raw[64]{}; int count=scePadRead(handle_,raw,static_cast<int>(capacity));
 if(count<=0) return count;
 if(count>static_cast<int>(capacity)) return -1;
 for(int n=0;n<count;++n) {
  const auto &r=raw[n]; PadInput s{}; s.valid=true; s.connected=r.connected!=0;
  s.connected_count=r.connected_count; s.buttons=r.buttons;
  s.left_x=r.left_x;s.left_y=r.left_y;s.right_x=r.right_x;s.right_y=r.right_y;s.l2=r.l2;s.r2=r.r2;
  s.timestamp_us=r.timestamp_us; s.read_rc=count; samples[n]=s;
 }
 return count;
#else
 return -1;
#endif
}
NativePadBridge::NativePadBridge(PadBackend &b,std::uint64_t session):backend_(b),session_(session) {
 ack_.session=session; ack_.generation=generation_;
 ack_.light_rc=ack_.mode_rc=ack_.start_rc=ack_.stop_rc=ack_.reset_rc=GC_APP_PAD_NOT_CALLED;
}
NativePadBridge::~NativePadBridge() {shutdown(last_input_);}
GcAppPadPoll NativePadBridge::poll() const {
 return {session_,generation_,connected_?1u:0u,
  have_handle_?(GC_APP_PAD_CAP_LIGHT|GC_APP_PAD_CAP_RUMBLE|GC_APP_PAD_CAP_INPUT):GC_APP_PAD_CAP_INPUT,
  input_.read_rc,handle_rc_};
}
void NativePadBridge::cleanup(std::uint64_t now,unsigned phase) {
 if(!cleanup_pending_) {cleanup_pending_=true; stop_tries_=reset_tries_=0; next_cleanup_=now;}
 running_=false;
 if(ack_.event_id && ack_.phase!=2 && ack_.phase!=3) ack_.phase=phase;
 if(now<next_cleanup_) return;
 next_cleanup_=now+50;
 if(have_handle_ && stop_tries_<3 && (stop_tries_==0 || ack_.stop_rc!=0)) {
  ack_.stop_rc=backend_.vibration({0,0}); ++stop_tries_; if(ack_.stop_rc==0)motor_on_=false;
 }
 if(have_handle_ && light_owned_ && reset_tries_<3) {
  int rc=backend_.reset_light(); ++reset_tries_; ack_.reset_rc=rc;
  if(rc==0)light_owned_=false;
 }
 if(!have_handle_ || ((ack_.stop_rc==0 || stop_tries_>=3) && (!light_owned_ || reset_tries_>=3))) {
  backend_.release(); have_handle_=false; cleanup_pending_=false;
  light_owned_=false; motor_on_=false; last_color_=0xffffffffu;
 }
}
void NativePadBridge::shutdown(std::uint64_t now) {
 if(have_handle_) {
  // Bounded immediate best-effort stop/reset on teardown, recording every last rc.
  for(unsigned i=0;i<3 && have_handle_;++i) cleanup(now+i*50,4);
 }
 backend_.release(); have_handle_=false; running_=false; have_reply_=false;
}
bool NativePadBridge::accept(const GcAppPadReply &r,std::uint64_t sent,std::uint64_t now) {
 if(!session_ || r.generation!=generation_ || !r.boot || r.lease_ms!=GC_APP_PAD_LEASE_MS ||
    !r.revision || sent>now || now-sent>=r.lease_ms || sent<last_sent_ ||
    (r.boot==boot_ && r.revision<desired_.revision)) return false;
 // Validate even typed mailbox data using the same wire limits.
 if(r.enabled>1 || r.effect>4 || r.period_ms<100 || r.period_ms>60000 || r.step_ms<16 || r.step_ms>1000 ||
    r.r>255 || r.g>255 || r.b>255 || r.event_state>6 || r.stop>1 || r.ttl_remaining_ms>GC_APP_PAD_TTL_MS ||
    r.large>255 || r.small>255 || (r.event_id && (!r.on_ms || r.on_ms>2000 || r.off_ms>2000 || !r.repeats || r.repeats>16 ||
    static_cast<std::uint64_t>(r.on_ms)*r.repeats+static_cast<std::uint64_t>(r.off_ms)*(r.repeats-1)>10000)))return false;
 if(boot_!=r.boot) { if(have_reply_ && have_handle_)cleanup(now,4); boot_=r.boot; last_id_=0; }
 bool changed=!have_reply_ || desired_.revision!=r.revision;
 desired_=r; have_reply_=true; last_sent_=sent; lease_until_=sent+r.lease_ms;
 ack_.boot=r.boot; ack_.revision=r.revision; ack_.generation=generation_;
 if(changed) {reset_tries_=0;ack_.reset_rc=GC_APP_PAD_NOT_CALLED;light_tries_=0;next_light_=now;last_color_=0xffffffffu;ack_.light_rc=GC_APP_PAD_NOT_CALLED;}
 if(r.event_id>last_id_ && r.event_state==GC_APP_PAD_PENDING) {
  if(running_) {
   // Replacement after an explicit core stop must neutralize the old pattern first.
   ack_.stop_rc=backend_.vibration({0,0}); running_=false;
   if(ack_.stop_rc!=0)cleanup(now,3);else motor_on_=false;
  }
  // Mark consumed BEFORE trying any output: a failed native start is never replayed.
  last_id_=r.event_id; ack_.event_id=r.event_id; ack_.phase=0;
  ack_.mode_rc=ack_.start_rc=ack_.stop_rc=GC_APP_PAD_NOT_CALLED;
  if(r.stop || !connected_ || !have_handle_ || cleanup_pending_ || !r.ttl_remaining_ms || now-sent>=r.ttl_remaining_ms) {
   ack_.phase=4; return true;
  }
  ack_.mode_rc=backend_.mode();
  if(ack_.mode_rc==0)ack_.start_rc=backend_.vibration({static_cast<std::uint8_t>(r.large),static_cast<std::uint8_t>(r.small)});
  if(ack_.mode_rc!=0 || ack_.start_rc!=0) {ack_.phase=3; cleanup(now,3);}
  else {ack_.phase=1;started_=now;running_=true;motor_on_=true;}
 }
 if(!r.enabled && have_handle_)reset_light_tick(now);
 if(r.stop && running_ && have_handle_) {
  ack_.stop_rc=backend_.vibration({0,0}); running_=false;
  if(ack_.stop_rc!=0) {ack_.phase=3;cleanup(now,3);}
  else {motor_on_=false;ack_.phase=4;}
 }
 return true;
}
void NativePadBridge::reset_light_tick(std::uint64_t now) {
 if(!light_owned_ || reset_tries_>=3 || now<next_light_)return;
 next_light_=now+50; ++reset_tries_;
 ack_.reset_rc=backend_.reset_light();
 if(ack_.reset_rc==0) {light_owned_=false;last_color_=0xffffffffu;}
}
void NativePadBridge::light_tick(std::uint64_t now) {
 if(now<next_light_ || light_tries_>=3)return;
 next_light_=now+desired_.step_ms;
 unsigned r=desired_.r,g=desired_.g,b=desired_.b;
 // Same ordinary pure dualsense_fx math; production IDs differ from DS_FX_*.
 // Keep arithmetic wide and step quantization modulo a full period (no clock wrap).
 unsigned phase=static_cast<unsigned>((now-now%desired_.step_ms)%desired_.period_ms);
 if(desired_.effect==1) { // Breathe: smoothstep, then gamma squared.
  unsigned half=desired_.period_ms/2;
  unsigned tri=phase<half?phase:desired_.period_ms-phase;
  std::uint64_t x=static_cast<std::uint64_t>(tri)*1000/half;
  unsigned sm=static_cast<unsigned>(x*x*(3000-2*x)/1000000);
  sm=sm*sm/1000;
  r=r*sm/1000;g=g*sm/1000;b=b*sm/1000;
 } else if(desired_.effect==2) {
  unsigned hue=phase*1536/desired_.period_ms, x=hue%256;
  switch(hue/256) {case 0:r=255;g=x;b=0;break;case 1:r=255-x;g=255;b=0;break;
   case 2:r=0;g=255;b=x;break;case 3:r=0;g=255-x;b=255;break;
   case 4:r=x;g=0;b=255;break;default:r=255;g=0;b=255-x;break;}
 } else if(desired_.effect==3) { // Police: two red flashes, then two blue.
  bool red=phase<desired_.period_ms/2;
  unsigned u=red?phase:phase-desired_.period_ms/2;
  bool on=u<55 || (u>=110 && u<165);
  r=on&&red?255:0;g=0;b=on&&!red?255:0;
 } else if(desired_.effect==4 && phase>=desired_.period_ms/8) {r=g=b=0;}
 std::uint32_t packed=(r<<16)|(g<<8)|b;
 if(packed==last_color_)return;
 light_owned_=true; // Even a failed call may have changed device state: reset on release.
 // Merge light telemetry into the durable event ACK; never replace phase/motor results.
 ack_.light_rc=backend_.light({static_cast<std::uint8_t>(r),static_cast<std::uint8_t>(g),static_cast<std::uint8_t>(b),0});
 if(ack_.light_rc==0) {last_color_=packed;light_owned_=true;light_tries_=0;}else ++light_tries_;
}
void NativePadBridge::tick(std::uint64_t now,const PadInput &in) {
 bool old_connected=connected_; unsigned old_count=input_.connected_count;
 if(in.valid) {input_=in;last_input_=now;seen_input_=true;connected_=in.connected;}
 else { input_.read_rc=in.read_rc; if(!seen_input_ || now-last_input_>=250 || (in.read_rc<0 && in.read_rc!=GC_APP_PAD_NOT_CALLED))connected_=false; }
 if(old_connected!=connected_ || (connected_ && old_count!=input_.connected_count)) {
  ++generation_; ack_.generation=generation_; have_reply_=false;
  acquire_tries_=0;next_acquire_=now;
  if(have_handle_)cleanup(now,4);
 }
 if(cleanup_pending_) {cleanup(now,4);return;}
 if(have_reply_ && now>=lease_until_) {have_reply_=false; if(have_handle_)cleanup(now,4);return;}
 if(!connected_) {if(have_handle_)cleanup(now,4);return;}
 if(!have_handle_ && acquire_tries_<3 && now>=next_acquire_) {
  ++acquire_tries_;next_acquire_=now+250;const int handle=backend_.acquire();have_handle_=handle>=0;
  handle_rc_=have_handle_?0:handle; // Never export the process-local handle value.
  // Bound consecutive failed borrows, not successful acquisitions over the app lifetime.
  // Lease cleanup may release a healthy handle repeatedly without a connection change.
  if(have_handle_)acquire_tries_=0;
 }
 if(!have_handle_ || !have_reply_)return;
 if(desired_.enabled)light_tick(now);else reset_light_tick(now);
 if(!running_)return;
 std::uint64_t elapsed=now-started_;
 std::uint64_t total=static_cast<std::uint64_t>(desired_.on_ms)*desired_.repeats+static_cast<std::uint64_t>(desired_.off_ms)*(desired_.repeats-1);
 bool on=elapsed<total && elapsed%(desired_.on_ms+desired_.off_ms)<desired_.on_ms;
 if(on!=motor_on_) {
  int rc=backend_.vibration(on?PadVibration{static_cast<std::uint8_t>(desired_.large),static_cast<std::uint8_t>(desired_.small)}:PadVibration{0,0});
  if(on)ack_.start_rc=rc;else ack_.stop_rc=rc;
  if(rc!=0) {ack_.phase=3;cleanup(now,3);return;}
  motor_on_=on;
 }
 if(elapsed>=total) {running_=false;ack_.phase=2;}
}
}

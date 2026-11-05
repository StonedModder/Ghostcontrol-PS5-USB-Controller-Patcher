// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gc_app_pad.h" // Build include path: repository core/src (or copy this public header to composed source root).
#include <cstddef>
#include <cstdint>
namespace ghostcontrol {
struct PadColor { std::uint8_t r,g,b,reserved; };
struct PadVibration { std::uint8_t large,small; };
static_assert(sizeof(PadColor)==4 && sizeof(PadVibration)==2);
// Only verified kit sample fields. No synthetic motion/orientation/touch data.
struct PadInput {
 bool valid=false,connected=false; std::uint8_t connected_count=0;
 std::uint32_t buttons=0; std::uint8_t left_x=128,left_y=128,right_x=128,right_y=128,l2=0,r2=0;
 std::uint64_t timestamp_us=0; int read_rc=GC_APP_PAD_NOT_CALLED;
};
class PadBackend {
 public: virtual ~PadBackend()=default;
 // acquire returns current APP's initial-user handle or negative rc. Never opens/binds/closes.
 virtual int acquire()=0;
 virtual int light(PadColor)=0;
 virtual int reset_light()=0;
 virtual int mode()=0;
 virtual int vibration(PadVibration)=0;
 virtual void release()=0; // forget reference only; owning Pad retains its handle
};
// Enable GC_NATIVE_PAD_PS5 for normal sceUserService/scePad imports; otherwise
// explicitly unavailable (-1), never a success stub. Existing Pad owner must be open.
class NativePadBackend final : public PadBackend {
 int handle_=-1;
 public:
 int acquire() override; int light(PadColor) override; int reset_light() override;
 int mode() override; int vibration(PadVibration) override; void release() override;
 // Optional SINGLE input reader. Read into at most 64 samples then feed InputTracker
 // chronologically. Do not call this in addition to kit Pad::read (shared input queue).
 int read(PadInput *samples,std::size_t capacity);
};
class NativePadBridge {
 PadBackend &backend_; std::uint64_t session_,generation_=1,boot_=0,last_id_=0;
 std::uint64_t lease_until_=0,started_=0,next_light_=0,next_acquire_=0,next_cleanup_=0;
 GcAppPadReply desired_{}; GcAppPadAck ack_{};
 PadInput input_{}; bool connected_=false,have_handle_=false,have_reply_=false,running_=false;
 bool motor_on_=false,light_owned_=false,cleanup_pending_=false;
 unsigned acquire_tries_=0,light_tries_=0,stop_tries_=0,reset_tries_=0;
 int handle_rc_=GC_APP_PAD_NOT_CALLED; std::uint32_t last_color_=0xffffffffu;
 void cleanup(std::uint64_t now,unsigned phase);
 void light_tick(std::uint64_t now);
 void reset_light_tick(std::uint64_t now);
 public:
 // session nonzero unique per app launch. Lifetime confined to main/update thread.
 NativePadBridge(PadBackend &,std::uint64_t session);
 ~NativePadBridge();
 NativePadBridge(const NativePadBridge &)=delete;
 NativePadBridge &operator=(const NativePadBridge &)=delete;
 // Supply latest REAL kit/native sample each frame. valid=false means no fresh data;
 // a stale input >250ms is treated disconnected for output safety, not fake readings.
 void tick(std::uint64_t now,const PadInput &);
 // Reply delivered via worker mailbox. sent_ms is monotonic time when poll was SENT,
 // not received. Reject stale generation/boot reply ordering and expired lease.
 bool accept(const GcAppPadReply &,std::uint64_t sent_ms,std::uint64_t now);
 GcAppPadPoll poll() const;
 // Light-only updates merge into this snapshot, never clear event phase/motor rc.
 // ACK snapshot repeats until acknowledged or superseded by later terminal phase;
 // repeated phase2 includes actual mode/start/stop results, so it subsumes phase1.
 GcAppPadAck ack() const {return ack_;}
 void shutdown(std::uint64_t now);
 private: std::uint64_t last_input_=0,last_sent_=0; bool seen_input_=false;
};
}

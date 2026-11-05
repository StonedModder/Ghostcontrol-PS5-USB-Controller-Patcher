/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_screen.hpp"
#include "json_reply.hpp"
namespace ghostcontrol {
Section build_screen_audio() {
    /* No live source: the shell would overwrite this with Unavailable from
     * AUDIO_STATUS. Capture actions are omitted; a press must not toast a
     * service-connection error for a provider this build does not include. */
    return {"Audio", "Headset capture is off in this build.", {
        {.label="Headset capture",
         .value="No headset capture in this build",
         .note="No headset capture in this build",
         .state=RowState::action,
         .detail={"Headset capture is off in this build.",
                  "Start, pause, volume and latency are not available here.",
                  "The service can still be running. This is not a connection failure."},
         .read_only=true},
    }};
}
bool AudioScreen::press(const Row &r,ServiceSnapshot &service) {
    if(!r.command || std::string_view(r.command)!="AUDIO_UI" || !r.argument || r.disabled || r.read_only) return false;
    if(std::string_view(r.argument)=="CANCEL") {cancel(service); return true;}
    if(request_) {feedback_="Wait for the current audio request."; return false;}
    request_=service.queue_fetch_request("AUDIO_UI",r.argument,300);
    feedback_=request_?"Waiting for audio response.":"Could not send the audio request; try again.";
    return request_!=0;
}
void AudioScreen::cancel(ServiceSnapshot &service) {
    const bool withdrawn=request_ && service.cancel_fetch_request(request_);
    request_=0; feedback_=withdrawn?"Cancelled before sending.":"Stopped waiting; an action already sent may still finish. Refresh audio.";
}
void AudioScreen::update(ServiceSnapshot &service) {
    if(!request_) return;
    const auto f=service.fetch_request(request_);
    if(f.state==ServiceSnapshot::ResultState::queued) return;
    request_=0;
    if(f.state==ServiceSnapshot::ResultState::none) {feedback_="Audio response unavailable; refresh before retrying."; return;}
    const JsonReply j(f.body);
    feedback_=f.valid?"Audio status: "+j.string_field("state")+"; check the headset for playback.":"Audio not confirmed: "+(j.string_field("error").empty()?f.detail:j.string_field("error"));
    if(j.is_true("persisted")) feedback_+=" Setting saved; playback has not been confirmed.";
}
}

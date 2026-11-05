// Ghostcontrol Toolbox - application entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, the renderer and the controller, then runs one frame at a
// time: read input, apply ordinary output to its own app pad, draw, present.
// Desired output comes from the resident companion over worker-owned local-console
// IPC. No network exchange or remote-process handle is used on the frame thread.
//
// Drawn with blackbearreloaded/ps5-homebrew-ui. Its ten-foot rules are not
// optional and are followed here: layout in the 1920x1080 virtual canvas, a
// 96 px side safe area, body text at 24 and above, and a hint row that says
// what the buttons do. See docs/UI-DESIGN.md.

#include "app_log.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "platform/ps5/audio_out.hpp"
#include "rgba_asset.hpp"
#include "service_snapshot.hpp"
#include "toolbox_ui.hpp"
#include "task_capture.hpp"
#include "service_launcher.hpp"
#include "service_launcher_platform.hpp"
#include "service_lifecycle.hpp"
#include "service_lifecycle_platform.hpp"
#include "native_pad_bridge.hpp"
#include "json_reply.hpp"

#include "gc_client.h"

#include "core/input.hpp"
#include "core/save_file.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <array>
#include <span>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{

constexpr const char *kAssets = "/app0/assets";

/* The system splash stays up until the app hides it. Returning from main
 * leaves it up with no process behind it, which is indistinguishable from a
 * hang, so a fatal failure hides the splash and then parks: a black screen
 * and a log beat an eternal splash. */
[[noreturn]] void fail(const char *what)
{
    ghostcontrol::app_log("fatal: %s", what);
    hui::sys::hide_splash_screen();
    hui::sys::park();
}

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    std::string bytes;
    if (!hui::save::read_file(path, &bytes) || !font->load(bytes))
    {
        ghostcontrol::app_log("font unavailable: %s", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

} // namespace

int main()
{
    ghostcontrol::app_log_open();
    ghostcontrol::app_log("Ghostcontrol Toolbox starting");

    ghostcontrol::app_log("opening display");
    hui::ps5::Display display;
    if (!display.open(1920, 1080))
        fail("display did not open");

    ghostcontrol::app_log("display open; initialising renderer and fonts");
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, semibold, display_face, mono;
    hui::ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_face, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono))
        fail("renderer or fonts unavailable");

    /* The project logo, drawn in the header and on reader panels. A missing
     * asset is not fatal: the primitive mark stands in. */
    std::uint32_t logo_texture = 0;
    int logo_width = 0;
    int logo_height = 0;
    {
        std::string meta;
        std::string pixels;
        const std::string base = std::string(kAssets) + "/ghostcontrol/logo";
        if (hui::save::read_file(base + ".txt", &meta) &&
            hui::save::read_file(base + ".rgba", &pixels) &&
            std::sscanf(meta.c_str(), "%d %d", &logo_width, &logo_height) == 2 &&
            logo_width > 0 && logo_height > 0 &&
            pixels.size() ==
                static_cast<std::size_t>(logo_width) * static_cast<std::size_t>(logo_height) * 4)
        {
            logo_texture = renderer.batch().create_texture(
                logo_width, logo_height,
                reinterpret_cast<const std::uint8_t *>(pixels.data()));
        }
        if (logo_texture == 0)
            ghostcontrol::app_log("logo unavailable; header falls back to the mark");
        else
            ghostcontrol::app_log("logo loaded %dx%d", logo_width, logo_height);
    }

    ghostcontrol::app_log("renderer and fonts ready; opening pad");
    hui::ps5::Pad pad;
    const bool pad_open = pad.open();
    ghostcontrol::app_log("app pad owner open=%d", pad_open ? 1 : 0);
    ghostcontrol::NativePadBackend native_backend;
    auto pad_session = static_cast<std::uint64_t>(hui::sys::monotonic_us()) ^
        (static_cast<std::uint64_t>(getpid()) << 32);
    if (!pad_session) pad_session = 1;
    ghostcontrol::NativePadBridge native_bridge(native_backend, pad_session);
    std::string last_track;
    unsigned long long native_reply_generation = 0;
    std::string last_logged_ack;

    /* Sound. The bank is scanned from the packaged assets; a missing bank is
     * not fatal -- the toolbox stays usable in silence rather than refusing
     * to start over a wav file. */
    hui::audio::Mixer mixer;
    /* The playlist attaches its stream to the mixer, so it has to be built
     * before the audio thread starts. */
    hui::audio::MusicPlayer music;
    const int songs = music.init(mixer, std::string(kAssets) + "/audio/music", 0x2545f4914f6cdd1dull);
    hui::ps5::AudioOut audio_out;
    audio_out.start(mixer);
    hui::audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    ghostcontrol::app_log("sounds files=%d rejected=%d music=%d first=%s", bank.files,
                          bank.rejected, songs, songs > 0 ? music.current().c_str() : "-");

    std::string network_detail;
    if (!ghostcontrol::platform_network_init(&network_detail))
        ghostcontrol::app_log("network init failed: %s", network_detail.c_str());
    // The service is polled off this thread from here on.
    ghostcontrol::ServiceSnapshot service;
    service.start();
    ghostcontrol::app_log("service poll started; entering frame loop");

    ghostcontrol::AppServicePreferencesWorker app_preferences;
    app_preferences.start();
    ghostcontrol::ToolboxUi ui(&service);
    ui.set_app_preferences(&app_preferences);
    ui.arm_beta_disclaimer();
    /* Support and community codes: same raw RGBA pair as the logo, generated
     * by tools/make-qr-assets.py and decoded back there before packaging. A
     * code that did not load is reported on its card, never faked. */
    const std::string plate_root = std::string(kAssets) + "/ghostcontrol";
    const auto qr_btc = ghostcontrol::load_qr_asset(renderer, plate_root, "qr-btc");
    const auto qr_discord = ghostcontrol::load_qr_asset(renderer, plate_root, "qr-discord");
    const auto qr_patreon = ghostcontrol::load_qr_asset(renderer, plate_root, "qr-patreon");
    ghostcontrol::app_log("support codes: btc=%d discord=%d patreon=%d",
                          qr_btc.present() ? 1 : 0, qr_discord.present() ? 1 : 0,
                          qr_patreon.present() ? 1 : 0);
    ui.set_qr(qr_btc, qr_discord, qr_patreon);
    ui.set_logo(logo_texture, logo_width, logo_height);
    /* The app carries its own companion and can hand it to the console's
     * loader, which is the only way a sandboxed title can start one. */
    ghostcontrol::ServiceLauncher launcher(ghostcontrol::platform_launch_transport(),
                                          ghostcontrol::platform_launch_assets());
    launcher.set_loader_host(ghostcontrol::platform_loader_host());
    /* One off-frame owner probes STATUS, validates and pushes. The frame only
     * queues a press or hands over the committed opt-in (APP-SVC-02). */
    ghostcontrol::ServiceLifecycle lifecycle(&launcher, ghostcontrol::platform_lifecycle_probe());
    lifecycle.start();
    ui.set_lifecycle(&lifecycle);
    /* Say up front whether this build has a companion and could reach the
     * loader, so a run that cannot start anything says why immediately. */
    {
        std::string bytes;
        const bool packaged = hui::save::read_file(
            std::string(ghostcontrol::ServiceLauncher::kAssetPath), &bytes);
        ghostcontrol::app_log("companion asset %s: %s (%zu bytes) at %s",
                              packaged ? "present" : "MISSING",
                              packaged ? "loaded" : "this build cannot start one",
                              bytes.size(),
                              std::string(ghostcontrol::ServiceLauncher::kAssetPath).c_str());
    }
    hui::InputTracker tracker;
    hui::ui::Feedback feedback;
    hui::gfx::DrawList list;
    std::array<hui::PadSample, 8> samples{};
    std::int64_t previous = hui::sys::monotonic_us();
    bool logged_first_swap = false;

    for (;;)
    {
        const std::int64_t now = hui::sys::monotonic_us();
        const float dt = static_cast<float>(now - previous) / 1000000.0f;
        music.pump(dt);
        if (music.current() != last_track) {
            last_track = music.current();
            ui.set_now_playing(last_track.empty() ? std::string("Silence") : last_track);
        }
        previous = now;

        const std::size_t count = pad.read(samples);
        /* The pad's top bit means the system also has this input. That must not
         * make the app deaf: the launch notice is a modal with no other way out,
         * so a sample carrying it would soft-lock the app. Keep the bit for the
         * record, give the tracker the plain buttons. */
        bool intercepted_now = false;
        for (std::size_t i = 0; i < count; ++i)
        {
            if ((samples[i].buttons & hui::pad_bits::kIntercepted) != 0)
            {
                intercepted_now = true;
                samples[i].buttons &= ~hui::pad_bits::kIntercepted;
            }
        }
        const hui::InputFrame input = tracker.update(
            std::span<const hui::PadSample>(samples.data(), count),
            static_cast<std::uint64_t>(now));
        // Kit Pad is the ONLY reader/handle owner; NativePadBackend borrows only
        // that same app's normal API handle. Never drain the input queue twice.
        ghostcontrol::PadInput native_input;
        if (count > 0) {
            const auto &sample = samples[count - 1];
            native_input.valid = true;
            native_input.connected = sample.connected &&
                (sample.buttons & hui::pad_bits::kIntercepted) == 0;
            // This API exposes one initial-user app pad, not a console-wide count.
            native_input.connected_count = sample.connected ? 1 : 0;
            native_input.buttons = sample.buttons;
            native_input.left_x = sample.left_x; native_input.left_y = sample.left_y;
            native_input.right_x = sample.right_x; native_input.right_y = sample.right_y;
            native_input.l2 = sample.l2; native_input.r2 = sample.r2;
            native_input.timestamp_us = sample.timestamp_us;
            // Kit read() hides the raw API return; do not fabricate an rc from count.
            native_input.read_rc = GC_APP_PAD_NOT_CALLED;
        }
        const auto now_ms = static_cast<std::uint64_t>(now / 1000);
        native_bridge.tick(now_ms, native_input);
        const auto incoming = service.native_pad_exchange();
        if (incoming.generation != native_reply_generation) {
            native_reply_generation = incoming.generation;
            const auto data = ghostcontrol::JsonReply(incoming.body).string_field("data");
            GcAppPadReply desired{};
            if (incoming.valid && gc_app_pad_parse_reply(data.c_str(), &desired) == 0)
                native_bridge.accept(desired, incoming.sent_ms, now_ms);
        }
        const auto native_poll = native_bridge.poll();
        const auto native_ack = native_bridge.ack();
        char poll_wire[GC_APP_PAD_WIRE_MAX]{}, ack_wire[GC_APP_PAD_WIRE_MAX]{};
        if (gc_app_pad_format_poll(&native_poll, poll_wire, sizeof(poll_wire)) == 0) {
            if (native_ack.boot && native_ack.revision)
                gc_app_pad_format_ack(&native_ack, ack_wire, sizeof(ack_wire));
            service.publish_native_pad(poll_wire, ack_wire);
        }
        char observation[1024]{};
        std::snprintf(observation, sizeof(observation),
            "{\"observed\":true,\"connected\":%s,\"app_pad_count\":%u,"
            "\"caps\":%u,\"read_rc\":%d,\"handle_rc\":%d,\"light_rc\":%d,"
            "\"event_id\":%llu,\"phase\":%u,\"mode_rc\":%d,\"start_rc\":%d,"
            "\"stop_rc\":%d,\"reset_rc\":%d,\"motion_valid\":false,"
            "\"producer_present\":%s}",
            native_poll.connected ? "true" : "false", native_poll.connected ? 1u : 0u,
            native_poll.caps, native_poll.read_rc, native_poll.handle_rc, native_ack.light_rc,
            static_cast<unsigned long long>(native_ack.event_id), native_ack.phase,
            native_ack.mode_rc, native_ack.start_rc, native_ack.stop_rc, native_ack.reset_rc,
            native_poll.handle_rc == 0 ? "true" : "false");
        service.publish_native_observation(observation);
        if (*ack_wire && last_logged_ack != ack_wire) {
            last_logged_ack = ack_wire;
            ghostcontrol::app_log("native output revision=%llu event=%llu phase=%u light=%d mode=%d start=%d stop=%d reset=%d",
                static_cast<unsigned long long>(native_ack.revision),
                static_cast<unsigned long long>(native_ack.event_id), native_ack.phase,
                native_ack.light_rc, native_ack.mode_rc, native_ack.start_rc,
                native_ack.stop_rc, native_ack.reset_rc);
        }

        feedback.clear();
        ui.update(input, dt, feedback);
        // ui.update() hands the committed opt-in to the lifecycle worker, which
        // decides once and does all probing/pushing on its own thread.

        /* The screen asks; the loop plays. Keeping playback out of the screen
         * means the screen stays drawable more than once per frame. */
        for (const hui::audio::CueEvent &event : feedback.cues)
            sounds.play(mixer, hui::audio::SoundSet::glass, event);
            music.duck();
        // NativePadBridge is the sole output scheduler. UI feedback must not
        // overwrite a requested test or neutralize it through kit Pad::tick().

        list.clear();
        ui.draw(list, fonts, service.read());

        renderer.begin();
        renderer.draw(list);
        renderer.present(0, display.width(), display.height());
        ghostcontrol::capture_task_frame(renderer);

        if (!display.swap()) {
            native_bridge.shutdown(now_ms);
            fail("swap failed");
        }

        if (!logged_first_swap)
        {
            logged_first_swap = true;
            /* Only now, with a real frame on screen to replace it. */
            sounds.play(mixer, hui::audio::SoundSet::glass,
                        hui::audio::CueEvent{hui::audio::Cue::welcome});
            const bool hidden = hui::sys::hide_splash_screen();
            ghostcontrol::app_log("first frame presented; splash_hidden=%d", hidden ? 1 : 0);
        }
    }

    native_bridge.shutdown(static_cast<std::uint64_t>(hui::sys::monotonic_us() / 1000));
    lifecycle.stop();       // cancels and joins local work; never companion EXIT
    app_preferences.stop(); // local settings only; never companion EXIT
    service.stop();
    audio_out.stop();
    pad.close();
    display.close();
    ghostcontrol::app_log("clean exit");
    ghostcontrol::app_log_close();
    return 0;
}

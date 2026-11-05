/* SPDX-License-Identifier: GPL-3.0-or-later
 * Task-based controller workspace. Rendering and navigation are independent
 * of the internal command catalog; live readings are never focus targets.
 */
#pragma once

#include "overlay.hpp"
#include "ghostscape.hpp"
#include "task_screen.hpp"
#include "profile_editor.hpp"
#include "app_service_preferences_worker.hpp"
#include "beta_disclaimer.hpp"
#include "service_launcher.hpp"
#include "service_lifecycle.hpp"
#include "screens.hpp"
#include "row_actions.hpp"
#include "service_snapshot.hpp"
#include "audio_screen.hpp"

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "ui/components/dialog.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ghostcontrol
{

/** One QR plate the host uploaded: the texture plus the pixel size it was
 *  created at. A zero texture means "not in this build", and the card says so
 *  instead of drawing a blank square. */
struct QrCode
{
    std::uint32_t texture = 0;
    int width = 0;
    int height = 0;
    bool present() const { return texture != 0 && width > 0 && height > 0; }
};

class ToolboxUi
{
  public:
    /** The service is kept only to queue a row's command and to read command
     *  results back; nothing here ever blocks on it. */
    explicit ToolboxUi(ServiceSnapshot *service);

    /** One frame of input. Sounds and rumble are asked for, not played.
     *
     *  The service view is what draw() will render this frame; the real app
     *  reads it from ServiceSnapshot, and the host snapshot harness passes a
     *  fake one. Live rows are resolved from it so changes between frames can
     *  be seen (and flashed) here rather than in draw(). */
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback,
                const ServiceSnapshot::View &service);
    /** As above, reading the view from the service snapshot. */
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    /** Draws the whole screen. Must not change state. */
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts,
              const ServiceSnapshot::View &service) const;

    /** Host-harness hook: shows a command-result toast as if the service had
     *  answered, so tools/host-ui-snapshots.sh can photograph one without a
     *  live service. Never called by the app. */
    void show_toast(std::string text);

    /** A Start GhosttoothAPI press. Queues one manual operation on the
     *  lifecycle worker and returns its receipt, or 0 when refused (no
     *  worker, closing, or an operation already in progress). Never probes,
     *  reads the package or pushes on the calling (frame) thread; the outcome
     *  arrives later through the lifecycle view. */
    std::uint64_t start_companion();
    /** Resident Stop / Restart presses (APP-SVC-04). Same contract as
     *  start_companion: queue on the worker, never block the frame. */
    std::uint64_t stop_companion();
    std::uint64_t restart_companion();

    /** The project logo, uploaded by the host (main.cpp and the snapshot
     *  harness). The real brand asset is shared by all presentation modes. */
    /* Name of the song the playlist is on, shown on the audio page. */
    void set_now_playing(std::string name) { now_playing_ = std::move(name); }
    void set_logo(std::uint32_t texture, int width, int height);
    /** Support and community codes, loaded once at start like the logo: the
     *  funding page draws all three, the Controllers workspace draws Discord.
     *  A code that did not load is reported on its card, never faked. */
    void set_qr(QrCode bitcoin, QrCode discord, QrCode patreon);

    /** Gives the shell its companion lifecycle owner. Called once by main.cpp
     *  before the frame loop; null means "this build cannot start one". The
     *  worker owns the launcher; the shell only queues intents and reads. */
    void set_lifecycle(ServiceLifecycle *lifecycle) { lifecycle_ = lifecycle; }
    const ServiceLifecycle::View &lifecycle_view() const { return lifecycle_view_; }
    void set_app_preferences(AppServicePreferencesWorker *worker) { app_preferences_ = worker; }
    const AppServicePreferencesWorker::View &app_preference_view() const { return app_preference_view_; }

    /** Arms the launch beta disclaimer. main.cpp calls this once so host
     *  tests and the snapshot harness never open it by accident. On the first
     *  update after arming, the kit Dialog appears unless "Don't show again"
     *  is already saved. */
    void arm_beta_disclaimer() { beta_disclaimer_armed_ = true; }
    bool beta_disclaimer_open() const { return beta_dialog_.is_open(); }
    /** True while the notice is painted, including its fade-out. */
    bool beta_disclaimer_visible() const { return beta_dialog_.visible(); }
    /** Host-harness hook: open the disclaimer without the preference check. */
    void open_beta_disclaimer(hui::ui::Feedback &feedback);
    /** Host-harness only: point the disclaimer preference at a private file. */
    void set_beta_disclaimer_path(std::string path)
    {
        beta_disclaimer_prefs_ = BetaDisclaimerPreference(std::move(path));
        beta_disclaimer_checked_ = false;
    }

    /** Host-harness hooks: open a modal directly, or the modal a named row
     *  declares exactly as confirming that row would. They let
     *  tools/host-ui-snapshots.sh photograph a modal without walking the list
     *  to it. Never called by the app. */
    void open_overlay(OverlayRequest request, hui::ui::Feedback &feedback);
    // Read-only production state seam for host input/worker tests.
    const OverlayRequest *active_overlay() const { return overlay_.request(); }
    const TaskPage &task_page() const { return task_; }
    std::size_t focused_task_button() const { return button_focus_; }
    bool directory_is_open() const { return page_menu_; }
    bool tools_are_open() const { return tools_open_; }
    std::string_view toast_text() const { return toast_text_; }
    // Same reading used by draw_rows; read-only production presentation seam.
    std::string row_reading(const Row &row, const ServiceSnapshot::View &service) const;
    const std::vector<std::string> &reader_content() const { return reader_lines_; }
    bool reader_is_open() const { return focus_ == Focus::reader; }
    std::size_t reader_scroll_offset() const { return reader_scroll_; }
    void open_row_overlay(std::string_view label, const ServiceSnapshot::View &service,
                          hui::ui::Feedback &feedback);

    /** Host-harness hook: opens the reader over the list with explicit lines,
     *  as if a fetch had returned them, so tools/host-ui-snapshots.sh can
     *  photograph the log and raw-input readers without a live service. When
     *  `refreshable` is true the reader behaves as a live one (Cross
     *  refreshes), so the hint row in the snapshot matches the app. Never
     *  called by the app. */
    void show_reader(std::string title, std::vector<std::string> lines,
                     hui::ui::Feedback &feedback, bool refreshable = false);

    /** The companion lifecycle owner, owned by main.cpp and handed over. Null
     *  on a host snapshot harness, where the Start row honestly reports it has
     *  no way to start anything. */
    ServiceLifecycle *lifecycle_ = nullptr;

  private:
    friend struct PresentationProbe; // HOST access to actual production drawing.
    bool endpoint_fresh(const char *endpoint) const;
    bool picker_fresh(const Row &row) const;
    bool action_fresh(const Row &row, const ServiceSnapshot::View &view) const;
    ServiceSnapshot::View fresh_view(const ServiceSnapshot::View &view) const;
    AppServicePreferencesWorker *app_preferences_ = nullptr;
    AppServicePreferencesWorker::View app_preference_view_;
    std::uint64_t app_preference_request_ = 0;
    ServiceLifecycle::View lifecycle_view_;
    ServiceLifecycle::View companion_rows_[3]; // last correlated Start/Stop/Restart record
    void refresh_lifecycle();
    std::uint64_t companion_reported_ = 0; // last lifecycle receipt toasted
    std::string companion_reading(const Row &row) const;
    std::uint64_t companion_request(LifecycleKind kind);
    enum class Focus
    {
        rail,
        content,
        reader,
    };

    const Section &section() const
    {
        return sections_[section_index_];
    }
    const Row &row() const
    {
        return section().rows[row_index_];
    }
    std::size_t visible_rows() const;
    std::size_t reader_lines() const;
    void clamp_scroll();
    void open_section(std::size_t index, hui::ui::Feedback &feedback);
    /** Fills the Testers row from the packaged list, so a name can be added
     *  by editing a text file rather than touching this code. */
    void load_testers();

    /* ---- the reader panel -----------------------------------------------
     * What the reader shows is decided when it opens, not read live from the
     * row, so a fetch-backed reader can rebuild its lines the moment a reply
     * lands without the frame ever touching the service. */
    enum class ReaderSource
    {
        detail,   // the row's own detail lines
        log_tail, // a fetched LOG_TAIL, hex-decoded
        raw_input,// historical "setup raw:" lines from the same tail
        setup_raw,// selected-source SETUP_INPUT cache, not LOG_TAIL or app-native input
        host,     // the snapshot harness's fixed lines: nothing to refresh
        bluetooth_diagnostics,
    };
    /** Opens the reader with these lines. A fetch-backed source starts an
     *  empty panel and queues the fetch; the reply fills it in. */
    void open_reader(std::vector<std::string> lines, ReaderSource source,
                     hui::ui::Feedback &feedback);
    bool reader_refreshable() const {
        return reader_source_ == ReaderSource::log_tail || reader_source_ == ReaderSource::raw_input ||
               reader_source_ == ReaderSource::setup_raw || reader_source_ == ReaderSource::bluetooth_diagnostics;
    }
    /** Queues the reader's exact refresh through the worker. No-op otherwise. */
    void refresh_reader();
    /** Decodes a LOG_TAIL reply (or reports why it cannot) into reader lines. */
    void rebuild_reader_lines(const ServiceSnapshot::Fetch &fetch);

    void rebuild_task(const ServiceSnapshot::View &service);
    void draw_workspace(hui::gfx::DrawList &, const hui::ui::Fonts &,
                        const ServiceSnapshot::View &) const;
    void draw_directory(hui::gfx::DrawList &, const hui::ui::Fonts &) const;
    void draw_tools(hui::gfx::DrawList &, const hui::ui::Fonts &,
                    const ServiceSnapshot::View &) const;
    void draw_reader(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const;
    /** The last command result, a pill above the hint row that fades on its
     *  own. Infrastructure pass: the sentence is real, the styling is not
     *  final. */
    void draw_toast(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const;
    void draw_hints(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const;
    /** The modal, drawn above the list and the reader alike. */
    void draw_overlay(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const;

    /** Builds the modal a row declares, starting it from the current service
     *  reading where the row has one. */
    void open_overlay_for_row(const Row &row, const ServiceSnapshot::View &service,
                              hui::ui::Feedback &feedback);
    /** Turns an accepted modal's value into queued commands and a toast. */
    void apply_overlay_result(const Row &row, const OverlayResult &result,
                              hui::ui::Feedback &feedback);

    /* ---- chained modals (wave 2c) ---------------------------------------
     * A row that needs two answers in order (pick a profile, then type its new
     * name) names Row::overlay_next. The shell opens the first modal as usual;
     * on accept it starts a chain, optionally queues Row::overlay_next_command
     * (a fetch, e.g. PROFILE_GET) whose reply seeds the second modal, then
     * opens that modal. The row's single overlay_command fires from the second
     * result with {parent} carrying the first. See docs/OVERLAY-API.md. */

    /** The row whose first modal was accepted, and what it chose, while the
     *  chain runs. `row` is a pointer into sections_, stable for the chain's
     *  life because the section cannot change under a modal. */
    struct Chain
    {
        bool active = false;
        const Row *row = nullptr;
        OverlayResult first;
        /** A fetch between the two modals, and the generation last seen, so a
         *  fresh reply is spotted exactly once. */
        const char *fetch_command = nullptr;
        bool fetch_pending = false;
        unsigned long long fetch_request = 0;
    };

    /** Starts a chain from the first modal's result: queues the between-modals
     *  fetch when the row names one, else opens the second modal now. */
    void begin_chain(const Row &row, const OverlayResult &first, hui::ui::Feedback &feedback);
    /** Opens the chain's second modal, seeded from the first result and, for a
     *  key binder, from the fetched profile. */
    void open_chain_next(hui::ui::Feedback &feedback);
    /** Tests the exact between-modal receipt against the latest endpoint attempt.
     * A later query cannot authorize this chain, even when it succeeds. */
    bool chain_source_fresh() const;
    /** Posts the second result with the first selection as parent. A keybinder
     * stays open after a change, so retain its source fence until cancellation. */
    void finish_chain(const Row &row, const OverlayResult &second, hui::ui::Feedback &feedback);

    ProfileEditor profile_editor_;
    ServiceSnapshot *service_ = nullptr;
    AudioScreen audio_screen_;
    RowActions row_actions_;
    bool runtime_fetch_queued_ = false;
    unsigned long long action_view_generation_ = 0;
    float action_view_age_ = 0.0f;
    unsigned long long action_runtime_generation_ = 0;
    float action_runtime_age_ = 0.0f;
    float action_pending_age_ = 0.0f;

    TaskPage task_;
    std::size_t button_focus_ = 0;
    std::size_t tool_index_ = 0;
    std::size_t menu_index_ = 0;
    bool page_menu_ = false;
    bool tools_open_ = false;
    std::vector<Section> sections_;
    struct PagePosition { std::size_t row = 0; std::size_t scroll = 0; };
    std::vector<PagePosition> page_positions_;
    std::size_t section_index_ = 0;
    std::size_t row_index_ = 0;
    std::size_t scroll_ = 0;
    std::size_t reader_scroll_ = 0;
    Focus focus_ = Focus::rail;

    /* The reader's content and where it came from, so the refresh verb knows
     * what to re-fetch. `reader_title_` is empty for a row reader (the row's
     * label titles it) and set for the harness's fixed readers. */
    ReaderSource reader_source_ = ReaderSource::detail;
    std::string reader_title_;
    std::vector<std::string> reader_lines_;
    mutable std::vector<std::string> reader_visual_lines_; // font-layout cache only
    /* This panel's exact request receipt, isolated from automatic endpoint polls. */
    unsigned long long reader_fetch_request_ = 0;
    bool reader_fetch_pending_ = false;

    hui::ui::SpringRect rail_highlight_;
    hui::ui::SpringRect row_highlight_;
    bool snapped_ = false;

    /* Time bases for the entrance animations: the whole screen, and the row
     * list, which restarts whenever the section changes so the new rows
     * arrive rather than appearing. */
    float age_ = 0.0f;
    float rows_age_ = 0.0f;
    float reader_age_ = 0.0f;
    /* Eased 0..1 for the reader appearing and leaving. */
    float reader_open_ = 0.0f;

    /* ---- motion state: advanced in update(), only read in draw() ---- */

    /* A section change: the content pane slides in from the direction of
     * travel and crossfades, and the newly focused rail chip pulses. */
    float switch_age_ = 100.0f;
    int switch_dir_ = 1;

    /* The row list glides when scroll_ changes instead of jumping; the
     * scrollbar thumb follows on its own, softer, tween. */
    hui::tween::Spring scroll_pos_;
    hui::tween::Spring scroll_thumb_;

    /* The last text each live row showed, and how long since it changed, so a
     * reading that moves can flash mint and tick its state dot. One entry per
     * (section, row); flash is >= kLiveFlashSeconds when idle. */
    struct LiveValue
    {
        std::string text;
        bool seen = false;
        float flash = 0.0f;
    };
    std::vector<std::vector<LiveValue>> live_;

    /* The confirmed command row dips for a moment after it is pressed. */
    float press_age_ = 100.0f;
    std::size_t press_row_ = 0;

    /* A quick mint flash on the service pill when the link comes up. */
    float link_flash_ = 100.0f;
    bool have_link_ = false;
    ServiceSnapshot::Link last_link_ = ServiceSnapshot::Link::connecting;

    /* The last command result and how long it has been up. */
    std::string now_playing_;
    std::string toast_text_;
    float toast_age_ = 1000.0f;

    /* The project logo texture, drawn in the header and on the reader panel.
     * Zero means "not uploaded yet"; no substitute artwork is synthesized. */
    QrCode qr_btc_, qr_discord_, qr_patreon_;
    std::uint32_t logo_texture_ = 0;
    int logo_w_ = 0;
    int logo_h_ = 0;

    /* The modal standing over the list, if any, and a free-running clock for
     * the widgets it hosts (their own update() advances their springs, but
     * their idle motion reads a time). */
    OverlayHost overlay_;
    /* Launch beta disclaimer: kit Dialog over the shell, once per launch
     * unless the player chose "Don't show again". */
    hui::ui::Dialog beta_dialog_;
    BetaDisclaimerPreference beta_disclaimer_prefs_;
    bool beta_disclaimer_armed_ = false;
    bool beta_disclaimer_checked_ = false;
    GhostScape ghostscape_;
    /* The row whose modal is up, so an accepted result is posted with that
     * row's metadata. Null for a modal opened without a row. */
    const Row *overlay_row_ = nullptr;
    /* A chained row's in-flight state, so the second modal opens the moment a
     * between-modals fetch lands. Inactive for every plain row. */
    Chain chain_;
    const Row *picker_row_ = nullptr;
    unsigned long long picker_request_ = 0;
    const Row *setup_picker_row_ = nullptr;
    unsigned long long setup_picker_request_ = 0;
    unsigned long long setup_save_request_ = 0;
    /* The physical-source selection the last SETUP_BEGIN pick carried (bounded
     * hex). Lets the guided-arm row re-send BEGIN immediately before ARM as one
     * atomic pair; the core refuses a lone ARM with setup_not_begun. Empty until
     * a controller is chosen this session. */
    std::string setup_selection_;
    float clock_ = 0.0f;
};

} // namespace ghostcontrol

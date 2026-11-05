/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APP-SVC-02: the one owner of "is GhosttoothAPI up, and if not, start it".
 *
 * Before this, both the Start row and (once) the automatic path called the
 * loader push synchronously from the frame, and nothing checked whether the
 * thing answering STATUS was actually GhosttoothAPI. This worker fixes both:
 *
 *   - A single thread owns the ServiceLauncher. The frame only enqueues an
 *     intent and copies a View; it never probes, reads the package or pushes.
 *   - Probe before send. A compatible GhosttoothAPI that already answers
 *     suppresses the push. A port that answers with something else, or that
 *     accepts and then stalls, is NOT "absent": nothing is sent over it.
 *   - The package is validated, then STATUS is probed again immediately
 *     before submission, so a companion that came up in between still
 *     suppresses the push.
 *   - Readiness is a strict, fresh STATUS taken by this worker after the
 *     push: ok:true, schema 1, service_kind "app-native", exact
 *     reviewed version "2.0.0-app-service-candidate", a positive bounded pid
 *     and started_ms, a nonzero per-lifetime boot instance and the port this
 *     app talks to. Empty,
 *     malformed, ok:false, foreign or wrong-port replies never count.
 *     Cached snapshot views and startup files are never consulted.
 *   - Automatic start happens at most once per app run, and only when the
 *     app-private opt-in was loaded committed and readback-verified
 *     (AppServicePreferencesWorker::View::startup_eligible). Withdrawing
 *     consent before the push cancels it.
 *   - One operation at a time. A request while one is queued or running is
 *     refused with a counted reason, never stacked.
 *   - stop() cancels and joins. It never sends EXIT: closing the app must not
 *     stop the resident companion.
 *
 * APP-SVC-03 permits Stop/Restart only through an ordinary provider that
 * atomically conditions EXIT on the complete expected reviewed identity and
 * separately witnesses ownership release for that same old identity BEFORE
 * replacement. Listener refusal, PID/marker absence, another STATUS or a new
 * boot alone never proves release. Production uses RESIDENT_EXIT_IF only
 * after fresh diagnostic capability, and validates the terminal receipt.
 * Missing capability means no conditional/unconditional EXIT or replacement.
 * One absolute operation deadline caps every phase, with cancellation and
 * post-callback checks. Any transferred bytes stay submitted/outcome-unknown,
 * never "nothing sent"; only a fresh qualified STATUS establishes readiness.
 *
 * What it does NOT establish: that the loader exists, that a push which got
 * no answer within the budget did not start anything (that outcome is
 * `start_unconfirmed`, not "failed"), or anything about controller output.
 */
#pragma once

#include "app_service_preferences_worker.hpp"
#include "service_launcher.hpp"

#include <atomic>
#include <memory>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace ghostcontrol
{

// Explicitly qualified STATUS producer version (core/src/gc_app_service.c).
// Unknown versions fail closed until their schema/identity contract is reviewed.
inline constexpr std::string_view kCompanionStatusVersion = "2.0.0-app-service-candidate";

/** The identity a compatible STATUS reply carries. `boot` is the companion's
 *  per-lifetime random instance; pid alone is reusable and is never used to
 *  decide freshness. */
struct CompanionIdentity
{
    std::uint64_t boot = 0;
    std::uint64_t pid = 0;
    std::uint64_t started_ms = 0;
    unsigned port = 0;
    std::string version;
};

/** Why a STATUS reply was or was not accepted as GhosttoothAPI. */
enum class StatusVerdict
{
    compatible,
    empty,           // nothing, or only whitespace
    malformed,       // not one valid JSON object
    not_ok,          // ok is missing or not true
    foreign_schema,  // schema missing or not 1
    foreign_product, // wrong/missing app-native kind or unreviewed version
    no_instance,     // invalid/missing/zero boot, pid or started_ms
    wrong_port,      // listen_port missing or not the port this app uses
};

/** Strict STATUS check. Fills `identity` only for `compatible`. */
StatusVerdict classify_companion_status(std::string_view reply, unsigned expected_port,
                                        CompanionIdentity *identity);
const char *status_verdict_text(StatusVerdict verdict);

/** What one STATUS exchange did at the transport level. */
enum class ProbeTransport
{
    answered,    // a complete one-line reply arrived; classify it
    refused,     // connection refused: nothing listens on the port
    unreachable, // route/address failure: says nothing about the companion
    stalled,     // connected (or connecting) but no complete reply in time
    truncated,   // a reply larger than the probe buffer
};

struct ProbeResult
{
    ProbeTransport transport = ProbeTransport::refused;
    std::string reply;
    int connect_errno = 0;
};

/** One STATUS exchange, injected so the policy runs on a host. `cancel`
 *  returns nonzero once the worker is stopping; implementations must poll it. */
struct LifecycleProbe
{
    ProbeResult (*status)(void *ctx, unsigned timeout_ms, int (*cancel)(void *),
                          void *cancel_ctx) = nullptr;
    void *ctx = nullptr;
    unsigned expected_port = 0;
    /* Deprecated compatibility slot; NEVER authorizes Stop/Restart and is
     * never called. Unconditional EXIT cannot bind an expected identity.
     * Retained only to avoid breaking existing aggregate HOST consumers. */
    ProbeResult (*exit)(void *ctx, unsigned timeout_ms, int (*cancel)(void *),
                        void *cancel_ctx) = nullptr;
    /* Optional ordinary provider contracts, diagnostic-capability gated in production.
     * conditional_exit atomically compares the complete expected identity
     * before accepting exit; a fresh STATUS or unqualified EXIT cannot do it.
     * release confirms teardown/ownership release for THAT SAME identity,
     * not listener closure, PID absence, a marker or another boot's STATUS.
     * HOST adapters must explicitly identify their evidence as synthetic. */
    ProbeResult (*conditional_exit)(void *ctx, const CompanionIdentity &expected,
                                   unsigned timeout_ms, int (*cancel)(void *),
                                   void *cancel_ctx) = nullptr;
    bool (*released)(void *ctx, const CompanionIdentity &expected,
                     unsigned timeout_ms, int (*cancel)(void *),
                     void *cancel_ctx) = nullptr;
    /* Owned production adapter: exactly one live logical worker per context. */
    std::shared_ptr<void> lifetime = {};
    std::shared_ptr<std::atomic_bool> worker_claim = {};
    /* Internal owned adapter retirement; no socket work and no synthetic witness call. */
    void (*transaction_end)(void *ctx) = nullptr;
};

enum class LifecycleKind
{
    automatic,
    manual,
    stop,    // explicit resident Stop: conditional EXIT plus same-identity release
    restart, // explicit resident Restart: stop, then start a different instance
};

enum class LifecyclePhase
{
    idle,
    queued,
    probing,
    validating,
    reprobing,
    sending,
    awaiting_ready,
    stopping, // conditional EXIT acknowledged; waiting for same-identity release
};

enum class LifecycleOutcome
{
    none,
    already_running,   // a compatible instance answered; nothing was sent
    ready,             // sent, then a fresh compatible instance answered
    start_unconfirmed, // sent, but no compatible answer within the budget
    identity_mismatch, // the port answers, but not as a compatible GhosttoothAPI
    unknown_liveness,  // the port accepted and stalled or overflowed; not absent
    unreachable,       // address/route failure; nothing was sent
    launch_failed,     // package/transport refused before or during the push
    cancelled,         // app closing, or consent withdrawn before the push
    stopped,           // conditional EXIT acknowledged plus same-identity ownership release
    restarted,         // old instance gone, then a DIFFERENT compatible instance answered
    stop_unconfirmed,  // EXIT refused/unanswered, or the old instance kept answering
};

class ServiceLifecycle
{
  public:
    struct Options
    {
        unsigned probe_timeout_ms = 1500;
        unsigned ready_budget_ms = 12000;
        unsigned ready_interval_ms = 250;
        unsigned transport_budget_ms = 15000; // whole loader push, absolute
        unsigned operation_budget_ms = 27000; // whole operation, never reset
        unsigned stop_budget_ms = 10000;      // whole stop phase including same-identity release
    };

    struct View
    {
        bool available = false; // worker running and able to accept intents
        LifecyclePhase phase = LifecyclePhase::idle;
        LifecycleKind kind = LifecycleKind::manual;
        LifecycleOutcome outcome = LifecycleOutcome::none;
        /* Receipt of the operation in phase, and of the last one that ended. */
        std::uint64_t active = 0, completed = 0;
        LifecycleKind completed_kind = LifecycleKind::manual;
        LaunchResult launch;           // the push result of the last operation
        bool identity_known = false;   // set only from this worker's own STATUS
        CompanionIdentity identity;
        /* The instance a Stop/Restart targeted, from its own fresh STATUS. */
        bool previous_known = false;
        CompanionIdentity previous;
        StatusVerdict last_verdict = StatusVerdict::empty;
        ProbeTransport last_transport = ProbeTransport::refused;
        std::string detail;
        /* Automatic start: decided once from the committed preference. */
        bool automatic_decided = false, automatic_scheduled = false;
        std::string automatic_note;
        unsigned sends = 0, probes = 0, refused_requests = 0;
    };

    ServiceLifecycle(ServiceLauncher *launcher, LifecycleProbe probe);
    ServiceLifecycle(ServiceLauncher *launcher, LifecycleProbe probe, Options options);
    ~ServiceLifecycle();
    ServiceLifecycle(const ServiceLifecycle &) = delete;
    ServiceLifecycle &operator=(const ServiceLifecycle &) = delete;

    void start();
    /** Cancels and joins local work. Never sends EXIT or any command. */
    void stop();

    /** A player press. Returns the receipt, or 0 when refused (worker not
     *  running, or an operation is already queued or in progress). */
    std::uint64_t request_start();
    /** Explicit resident Stop / Restart presses (APP-SVC-03). Same refusal
     *  rules as request_start. Stop sends EXIT only to a fresh compatible
     *  instance; Restart additionally requires a different instance after. */
    std::uint64_t request_stop();
    std::uint64_t request_restart();

    /** Called every frame with the app-private preference view. Cheap: takes
     *  a lock and compares. Makes the one automatic decision the first time
     *  the preference has settled, and cancels a not-yet-sent automatic push
     *  if consent is withdrawn. Returns the receipt when it schedules. */
    std::uint64_t observe_consent(const AppServicePreferencesWorker::View &consent);

    View read() const;

  private:
    void work();
    void run(std::uint64_t receipt, LifecycleKind kind);
    std::uint64_t request(LifecycleKind kind);
    /* 0 = finished, 1 = old instance released (restart continues), 2 = nothing
     * was running (restart continues as a plain start). */
    int stop_phase(std::uint64_t receipt, LifecycleKind kind, std::uint64_t *old_boot);
    void start_phase(std::uint64_t receipt, LifecycleKind kind, std::uint64_t exclude_boot,
                     bool already_probed_absent);
    ProbeResult probe_once(StatusVerdict *verdict, CompanionIdentity *identity);
    bool wait_or_stop(unsigned ms);
    void set_phase(LifecyclePhase phase);
    void finish(std::uint64_t receipt, LifecycleOutcome outcome, std::string detail);
    bool automatic_withdrawn() const;
    unsigned remaining_ms(unsigned cap) const;
    bool expired() const;
    static int cancel_requested(void *self);

    ServiceLauncher *launcher_;
    LifecycleProbe probe_;
    Options options_;
    std::chrono::steady_clock::time_point operation_end_, deadline_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    View view_;
    std::uint64_t next_receipt_ = 0;
    std::uint64_t pending_ = 0;
    LifecycleKind pending_kind_ = LifecycleKind::manual;
    bool owns_probe_ = false;
    bool stopping_ = false;
    bool consent_withdrawn_ = false;
    std::thread thread_;
};

const char *lifecycle_phase_text(LifecyclePhase phase);
/** One honest sentence for a finished operation. */
std::string lifecycle_outcome_text(const ServiceLifecycle::View &view);

} // namespace ghostcontrol

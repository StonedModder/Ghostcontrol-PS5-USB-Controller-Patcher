/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Policy half of the companion lifecycle owner. No sockets and no files: the
 * STATUS probe and the loader push are injected, so every rule in
 * service_lifecycle.hpp runs on a host.
 */
#include "service_lifecycle.hpp"

#include "app_log.hpp"
#include "json_reply.hpp"

#include <chrono>
#include <algorithm>
#include <cstdio>

namespace ghostcontrol
{
namespace
{
/* A plain unsigned decimal that fits 64 bits. JSON numbers with a sign,
 * fraction, exponent or leading zero are not identities. */
bool plain_u64(std::string_view raw, std::uint64_t *out)
{
    if (raw.empty() || raw.size() > 20) return false;
    if (raw.size() > 1 && raw[0] == '0') return false;
    std::uint64_t value = 0;
    for (char c : raw)
    {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

bool blank(std::string_view text)
{
    for (char c : text)
        if (c != ' ' && c != '\t' && c != 13 && c != '\n') return false;
    return true;
}

std::string short_instance(std::uint64_t boot)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%08llx",
                  static_cast<unsigned long long>(boot & 0xffffffffull));
    return text;
}
} // namespace

StatusVerdict classify_companion_status(std::string_view reply, unsigned expected_port,
                                        CompanionIdentity *identity)
{
    if (blank(reply)) return StatusVerdict::empty;
    const JsonReply json(reply);
    if (!json.valid) return StatusVerdict::malformed;
    if (json.raw("ok") != "true") return StatusVerdict::not_ok;
    if (json.raw("schema") != "1") return StatusVerdict::foreign_schema;
    const std::string_view kind_raw = json.raw("service_kind");
    const std::string_view version_raw = json.raw("version");
    if (kind_raw.empty() || kind_raw.front() != '"' ||
        json.string_field("service_kind") != "app-native" ||
        version_raw.size() < 3 || version_raw.front() != '"' ||
        json.string_field("version") != kCompanionStatusVersion)
        return StatusVerdict::foreign_product;
    std::uint64_t boot = 0, pid = 0, started_ms = 0;
    if (!plain_u64(json.raw("boot"), &boot) || boot == 0 ||
        !plain_u64(json.raw("pid"), &pid) || pid == 0 || pid > INT32_MAX ||
        !plain_u64(json.raw("started_ms"), &started_ms) || started_ms == 0)
        return StatusVerdict::no_instance;
    std::uint64_t port = 0;
    if (expected_port == 0 || !plain_u64(json.raw("listen_port"), &port) ||
        port != expected_port)
        return StatusVerdict::wrong_port;
    if (identity)
    {
        CompanionIdentity found;
        found.boot = boot;
        found.port = static_cast<unsigned>(port);
        found.version = json.string_field("version");
        found.pid = pid;
        found.started_ms = started_ms;
        *identity = found;
    }
    return StatusVerdict::compatible;
}

const char *status_verdict_text(StatusVerdict verdict)
{
    switch (verdict)
    {
    case StatusVerdict::compatible: return "compatible Ghosttooth API";
    case StatusVerdict::empty: return "empty reply";
    case StatusVerdict::malformed: return "malformed reply";
    case StatusVerdict::not_ok: return "reply does not report ok";
    case StatusVerdict::foreign_schema: return "unsupported STATUS schema";
    case StatusVerdict::foreign_product: return "not a Ghosttooth API app-native service";
    case StatusVerdict::no_instance: return "no per-lifetime instance identity";
    case StatusVerdict::wrong_port: return "answers for a different port";
    }
    return "unknown";
}

const char *lifecycle_phase_text(LifecyclePhase phase)
{
    switch (phase)
    {
    case LifecyclePhase::idle: return "Idle";
    case LifecyclePhase::queued: return "Queued";
    case LifecyclePhase::probing: return "Checking";
    case LifecyclePhase::validating: return "Checking package";
    case LifecyclePhase::reprobing: return "Rechecking";
    case LifecyclePhase::sending: return "Sending";
    case LifecyclePhase::awaiting_ready: return "Waiting for answer";
    case LifecyclePhase::stopping: return "Stopping";
    }
    return "Unknown";
}

std::string lifecycle_outcome_text(const ServiceLifecycle::View &view)
{
    const std::string instance =
        view.identity_known ? " (instance " + short_instance(view.identity.boot) + ")" : "";
    switch (view.outcome)
    {
    case LifecycleOutcome::none:
        return "No companion start has run yet.";
    case LifecycleOutcome::already_running:
        return "Ghosttooth API is already answering" + instance + "; nothing was sent.";
    case LifecycleOutcome::ready:
        return "Ghosttooth API started and answered STATUS" + instance + ".";
    case LifecycleOutcome::restarted:
        return "Ghosttooth API restarted; a new instance answered" + instance + ".";
    case LifecycleOutcome::stopped:
    case LifecycleOutcome::stop_unconfirmed:
        return view.detail;
    case LifecycleOutcome::start_unconfirmed:
    case LifecycleOutcome::identity_mismatch:
    case LifecycleOutcome::unknown_liveness:
    case LifecycleOutcome::unreachable:
    case LifecycleOutcome::launch_failed:
    case LifecycleOutcome::cancelled:
        return view.detail;
    }
    return view.detail;
}

ServiceLifecycle::ServiceLifecycle(ServiceLauncher *launcher, LifecycleProbe probe)
    : ServiceLifecycle(launcher, probe, Options{})
{
}

ServiceLifecycle::ServiceLifecycle(ServiceLauncher *launcher, LifecycleProbe probe,
                                   Options options)
    : launcher_(launcher), probe_(probe), options_(options)
{
}

ServiceLifecycle::~ServiceLifecycle() { stop(); }

void ServiceLifecycle::start()
{
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return;
    if (probe_.worker_claim && probe_.worker_claim->exchange(true)) {
        view_.available = false;
        view_.detail = "Lifecycle adapter is already owned by another worker.";
        return;
    }
    owns_probe_ = static_cast<bool>(probe_.worker_claim);
    stopping_ = false;
    view_.available = true;
    thread_ = std::thread(&ServiceLifecycle::work, this);
}

void ServiceLifecycle::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        view_.available = false;
        if (pending_)
        {
            /* Queued but never started: close its receipt honestly. */
            view_.completed = pending_;
            view_.completed_kind = pending_kind_;
            view_.outcome = LifecycleOutcome::cancelled;
            view_.detail = "App closing; the queued companion start never ran.";
            view_.active = 0;
            view_.phase = LifecyclePhase::idle;
            pending_ = 0;
        }
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (owns_probe_) { probe_.worker_claim->store(false); owns_probe_ = false; }
}

std::uint64_t ServiceLifecycle::request(LifecycleKind kind)
{
    std::lock_guard lock(mutex_);
    if (!view_.available || stopping_ || pending_ || view_.active)
    {
        ++view_.refused_requests;
        return 0;
    }
    const std::uint64_t receipt = ++next_receipt_;
    pending_ = receipt;
    pending_kind_ = kind;
    view_.active = receipt;
    view_.kind = kind;
    view_.phase = LifecyclePhase::queued;
    wake_.notify_one();
    return receipt;
}

std::uint64_t ServiceLifecycle::request_start() { return request(LifecycleKind::manual); }
std::uint64_t ServiceLifecycle::request_stop() { return request(LifecycleKind::stop); }
std::uint64_t ServiceLifecycle::request_restart() { return request(LifecycleKind::restart); }

std::uint64_t ServiceLifecycle::observe_consent(const AppServicePreferencesWorker::View &consent)
{
    std::lock_guard lock(mutex_);
    if (!view_.available || stopping_) return 0;
    if (view_.automatic_decided)
    {
        /* Withdrawal only matters before the push; after it, nothing can be
         * taken back and the result is reported as it is. */
        const bool automatic_open = view_.active != 0 && view_.kind == LifecycleKind::automatic &&
                                    view_.phase != LifecyclePhase::sending &&
                                    view_.phase != LifecyclePhase::awaiting_ready;
        if (automatic_open && !consent.startup_eligible) consent_withdrawn_ = true;
        return 0;
    }
    /* Decide exactly once, on the first settled committed preference. */
    if (!consent.available || !consent.loaded || consent.pending) return 0;
    view_.automatic_decided = true;
    if (!consent.startup_eligible)
    {
        view_.automatic_note = consent.error.empty()
                                   ? "App-private opt-in is off; no automatic start this run."
                                   : "App-private opt-in could not be verified; no automatic start this run.";
        return 0;
    }
    if (pending_ || view_.active)
    {
        view_.automatic_note = "A start was already requested; automatic start not repeated.";
        return 0;
    }
    const std::uint64_t receipt = ++next_receipt_;
    pending_ = receipt;
    pending_kind_ = LifecycleKind::automatic;
    consent_withdrawn_ = false;
    view_.automatic_scheduled = true;
    view_.automatic_note = "Automatic start scheduled once from the saved opt-in.";
    view_.active = receipt;
    view_.kind = LifecycleKind::automatic;
    view_.phase = LifecyclePhase::queued;
    wake_.notify_one();
    return receipt;
}

ServiceLifecycle::View ServiceLifecycle::read() const
{
    std::lock_guard lock(mutex_);
    return view_;
}

int ServiceLifecycle::cancel_requested(void *self)
{
    auto *lifecycle = static_cast<ServiceLifecycle *>(self);
    std::lock_guard lock(lifecycle->mutex_);
    return lifecycle->stopping_ ? 1 : 0;
}

bool ServiceLifecycle::wait_or_stop(unsigned ms)
{
    std::unique_lock lock(mutex_);
    // Wait to the exact phase deadline, including its final fractional ms.
    // Provider millisecond budgets remain floored; zero never means default.
    wake_.wait_until(lock, std::min(deadline_, std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(ms)), [this] { return stopping_; });
    return !stopping_;
}

bool ServiceLifecycle::expired() const
{
    return std::chrono::steady_clock::now() >= deadline_;
}

unsigned ServiceLifecycle::remaining_ms(unsigned cap) const
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline_) return 0;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count();
    // Floor: never extend a remainder or pass zero to a client as its default.
    return static_cast<unsigned>(std::min<std::int64_t>(left, cap));
}

bool ServiceLifecycle::automatic_withdrawn() const
{
    std::lock_guard lock(mutex_);
    return view_.kind == LifecycleKind::automatic && consent_withdrawn_;
}

void ServiceLifecycle::set_phase(LifecyclePhase phase)
{
    std::lock_guard lock(mutex_);
    view_.phase = phase;
}

void ServiceLifecycle::finish(std::uint64_t receipt, LifecycleOutcome outcome, std::string detail)
{
    bool automatic = false;
    {
        std::lock_guard lock(mutex_);
        view_.outcome = outcome;
        view_.detail = detail;
        view_.completed = receipt;
        view_.completed_kind = view_.kind;
        view_.active = 0;
        view_.phase = LifecyclePhase::idle;
        automatic = view_.kind == LifecycleKind::automatic;
    }
    /* Expired/cancelled/failed operations cannot retain a terminal receipt. */
    if (probe_.transaction_end) probe_.transaction_end(probe_.ctx);
    /* Logged outside the lock so a slow log write never stalls a frame read. */
    app_log("companion lifecycle: receipt=%llu automatic=%d outcome=%d detail=%s",
            static_cast<unsigned long long>(receipt), automatic ? 1 : 0,
            static_cast<int>(outcome), detail.c_str());
}

ProbeResult ServiceLifecycle::probe_once(StatusVerdict *verdict, CompanionIdentity *identity)
{
    ProbeResult result;
    const unsigned timeout = remaining_ms(options_.probe_timeout_ms);
    if (timeout == 0 || cancel_requested(this)) result.transport = ProbeTransport::stalled;
    else if (probe_.status)
        result = probe_.status(probe_.ctx, timeout,
                               &ServiceLifecycle::cancel_requested, this);
    else
        result.transport = ProbeTransport::unreachable;
    CompanionIdentity found;
    if (expired() || cancel_requested(this)) result.transport = ProbeTransport::stalled;
    StatusVerdict judged = StatusVerdict::empty;
    if (result.transport == ProbeTransport::answered)
        judged = classify_companion_status(result.reply, probe_.expected_port, &found);
    {
        std::lock_guard lock(mutex_);
        ++view_.probes;
        view_.last_transport = result.transport;
        if (result.transport == ProbeTransport::answered) view_.last_verdict = judged;
        if (judged == StatusVerdict::compatible)
        {
            view_.identity_known = true;
            view_.identity = found;
        }
    }
    *verdict = judged;
    if (identity) *identity = found;
    return result;
}

void ServiceLifecycle::work()
{
    for (;;)
    {
        std::uint64_t receipt = 0;
        LifecycleKind kind = LifecycleKind::manual;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || pending_ != 0; });
            if (stopping_) return;
            receipt = pending_;
            kind = pending_kind_;
            pending_ = 0;
        }
        run(receipt, kind);
    }
}

void ServiceLifecycle::run(std::uint64_t receipt, LifecycleKind kind)
{
    operation_end_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.operation_budget_ms);
    deadline_ = operation_end_;
    {
        std::lock_guard lock(mutex_);
        view_.kind = kind;
        view_.active = receipt;
        view_.identity_known = false;
        view_.identity = CompanionIdentity{};
        view_.previous_known = false;
        view_.previous = CompanionIdentity{};
        view_.launch = LaunchResult{};
        view_.detail.clear();
    }
    if (kind == LifecycleKind::stop || kind == LifecycleKind::restart)
    {
        std::uint64_t old_boot = 0;
        const int next = stop_phase(receipt, kind, &old_boot);
        if (next == 0) return;
        start_phase(receipt, kind, old_boot, true);
        return;
    }
    start_phase(receipt, kind, 0, false);
}

int ServiceLifecycle::stop_phase(std::uint64_t receipt, LifecycleKind kind, std::uint64_t *old_boot)
{
    deadline_ = std::min(operation_end_, std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.stop_budget_ms));
    const unsigned port = probe_.expected_port;
    set_phase(LifecyclePhase::probing);
    StatusVerdict verdict = StatusVerdict::empty;
    CompanionIdentity old;
    const ProbeResult probed = probe_once(&verdict, &old);
    if (expired())
    {
        finish(receipt, LifecycleOutcome::stop_unconfirmed, "Stop budget expired; no confirmed release. Nothing was pushed.");
        return 0;
    }
    if (cancel_requested(this))
    {
        finish(receipt, LifecycleOutcome::cancelled, "App closing; nothing was sent.");
        return 0;
    }
    switch (probed.transport)
    {
    case ProbeTransport::refused:
        finish(receipt, LifecycleOutcome::stop_unconfirmed,
               "Listener absent; ordinary ownership release is not confirmed. Nothing was sent or pushed.");
        return 0;
    case ProbeTransport::answered:
        if (verdict == StatusVerdict::compatible) break;
        finish(receipt, LifecycleOutcome::identity_mismatch,
               "Port " + std::to_string(port) + " answers, but not as a compatible Ghosttooth API (" +
                   status_verdict_text(verdict) + "). No EXIT was sent to it.");
        return 0;
    case ProbeTransport::unreachable:
        finish(receipt, LifecycleOutcome::unreachable,
               "The companion address is unreachable (errno " + std::to_string(probed.connect_errno) +
                   "); nothing was sent.");
        return 0;
    case ProbeTransport::stalled:
    case ProbeTransport::truncated:
        finish(receipt, LifecycleOutcome::unknown_liveness,
               "Port " + std::to_string(port) + " gave no usable STATUS; no EXIT was sent.");
        return 0;
    }
    {
        std::lock_guard lock(mutex_);
        view_.previous_known = true;
        view_.previous = old;
    }
    *old_boot = old.boot;
    const JsonReply observed(probed.reply);
    if (observed.raw("conditional_release") == "false") {
        std::string blocker = observed.string_field("lifecycle_blocker");
        if (blocker.size() > 120) blocker.clear();
        for (char c : blocker) if (c < 32 || c > 126) { blocker.clear(); break; }
        finish(receipt, LifecycleOutcome::stop_unconfirmed,
               "Unavailable: " + (blocker.empty() ? std::string("checked release provider unavailable") : blocker) +
               ". No EXIT or replacement was sent.");
        return 0;
    }
    if (!probe_.conditional_exit)
    {
        finish(receipt, LifecycleOutcome::stop_unconfirmed,
               "Unavailable: this build cannot condition EXIT on the expected product/process instance. Nothing was sent or pushed.");
        return 0;
    }

    /* Only an atomic ordinary conditional contract may authorize EXIT. */
    const unsigned exit_timeout = remaining_ms(options_.stop_budget_ms);
    if (exit_timeout == 0 || cancel_requested(this))
    {
        finish(receipt, LifecycleOutcome::stop_unconfirmed, "Stop cancelled or budget expired before conditional EXIT.");
        return 0;
    }
    const ProbeResult acked = probe_.conditional_exit(probe_.ctx, old, exit_timeout,
                                          &ServiceLifecycle::cancel_requested, this);
    const JsonReply ack(acked.reply);
    if (expired() || cancel_requested(this) || acked.transport != ProbeTransport::answered || !ack.success() || !ack.is_true("accepted"))
    {
        finish(receipt, LifecycleOutcome::stop_unconfirmed,
               cancel_requested(this) ? "App closing; whether EXIT reached Ghosttooth API is unknown."
                                      : "Ghosttooth API did not acknowledge EXIT; it may still be running. "
                                        "Nothing further was sent.");
        return 0;
    }

    if (!probe_.released)
    {
        finish(receipt, LifecycleOutcome::stop_unconfirmed,
               "Conditional EXIT acknowledged, but ordinary release for the same old identity is unavailable. Nothing was pushed.");
        return 0;
    }
    /* The acknowledgement and listener closure are not ownership release. */
    set_phase(LifecyclePhase::stopping);

    bool old_released = false;
    bool first = true;
    for (;;)
    {
        if (!first && !wait_or_stop(options_.ready_interval_ms))
        {
            finish(receipt, LifecycleOutcome::cancelled,
                   "App closing; exit may have completed but stop was not confirmed.");
            return 0;
        }
        first = false;
        const unsigned release_timeout = remaining_ms(options_.stop_budget_ms);
        if (!old_released && release_timeout && !cancel_requested(this))
            old_released = probe_.released(probe_.ctx, old, release_timeout,
                                          &ServiceLifecycle::cancel_requested, this);
        if (expired() || cancel_requested(this) || !remaining_ms(options_.probe_timeout_ms))
        {
            finish(receipt, LifecycleOutcome::stop_unconfirmed,
                   "Stop cancelled or budget expired before confirmed ordinary release and endpoint observation. Nothing was pushed.");
            return 0;
        }
        CompanionIdentity now;
        const ProbeResult check = probe_once(&verdict, &now);
        if (expired() || cancel_requested(this))
        {
            finish(receipt, LifecycleOutcome::stop_unconfirmed,
                   "Stop cancelled or budget expired before confirmed ordinary release. Nothing was pushed.");
            return 0;
        }
        if (old_released && check.transport == ProbeTransport::refused) break;
        if (check.transport == ProbeTransport::answered && verdict == StatusVerdict::compatible &&
            now.boot != old.boot && old_released)
        {
            /* A different STATUS alone never proves the old release. */
            if (kind == LifecycleKind::restart)
                finish(receipt, LifecycleOutcome::restarted,
                       "The old instance stopped and a different instance already answers; nothing was pushed.");
            else
                finish(receipt, LifecycleOutcome::stop_unconfirmed,
                       "The old instance stopped, but a different Ghosttooth API instance now answers.");
            return 0;
        }
        if (expired())
        {
            finish(receipt, LifecycleOutcome::stop_unconfirmed,
                   "EXIT was acknowledged but port " + std::to_string(port) + " still " +
                       (check.transport == ProbeTransport::answered ? "answers" : "accepts connections") +
                       " after " + std::to_string((options_.stop_budget_ms + 999) / 1000) +
                       " s. Nothing was forced or pushed.");
            return 0;
        }
    }
    if (kind == LifecycleKind::stop)
    {
        finish(receipt, LifecycleOutcome::stopped,
               "Conditional EXIT acknowledged; ordinary ownership release for the expected old instance was confirmed.");
        return 0;
    }
    deadline_ = operation_end_; // original operation budget, never a new one
    return 1;
}

void ServiceLifecycle::start_phase(std::uint64_t receipt, LifecycleKind kind, std::uint64_t exclude_boot,
                                   bool already_probed_absent)
{
    const unsigned port = probe_.expected_port;
    /* Replacing an instance this worker stopped: success then requires a
     * different boot identity. A restart with nothing running is a start. */
    const bool replacing = kind == LifecycleKind::restart && exclude_boot != 0;
    const std::string after_stop = replacing ? "The old instance stopped. " : "";

    /* One pre-push classification, used before validating the package and
     * again immediately before handing it over. Returns true only when the
     * port is genuinely refusing connections. */
    auto clear_to_send = [&](bool second) -> bool {
        set_phase(second ? LifecyclePhase::reprobing : LifecyclePhase::probing);
        StatusVerdict verdict = StatusVerdict::empty;
        CompanionIdentity found;
        const ProbeResult probed = probe_once(&verdict, &found);
        if (expired())
        {
            finish(receipt, LifecycleOutcome::unknown_liveness,
                   after_stop + "Operation budget expired before submission; nothing was sent.");
            return false;
        }
        if (cancel_requested(this))
        {
            finish(receipt, LifecycleOutcome::cancelled,
                   after_stop + "App closing; companion start cancelled before anything was sent.");
            return false;
        }
        switch (probed.transport)
        {
        case ProbeTransport::refused:
            return true;
        case ProbeTransport::answered:
            if (verdict == StatusVerdict::compatible && replacing && found.boot != exclude_boot)
                finish(receipt, LifecycleOutcome::restarted,
                       "A different instance came up before the push; nothing was sent.");
            else if (verdict == StatusVerdict::compatible && replacing)
                finish(receipt, LifecycleOutcome::stop_unconfirmed,
                       "The old instance answers again; nothing was pushed over it.");
            else if (verdict == StatusVerdict::compatible)
                finish(receipt, LifecycleOutcome::already_running,
                       second ? "Ghosttooth API came up before the push; nothing was sent."
                              : "Ghosttooth API already answers; nothing was sent.");
            else
                finish(receipt, LifecycleOutcome::identity_mismatch,
                       after_stop + "Port " + std::to_string(port) +
                           " answers, but not as a compatible Ghosttooth API (" + status_verdict_text(verdict) +
                           "). Nothing was sent.");
            return false;
        case ProbeTransport::unreachable:
            finish(receipt, LifecycleOutcome::unreachable,
                   after_stop + "The companion address is unreachable (errno " +
                       std::to_string(probed.connect_errno) + "); nothing was sent.");
            return false;
        case ProbeTransport::stalled:
        case ProbeTransport::truncated:
            finish(receipt, LifecycleOutcome::unknown_liveness,
                   after_stop + "Port " + std::to_string(port) +
                       " accepted but gave no usable STATUS; something may already own it. Nothing was sent.");
            return false;
        }
        return false;
    };
    auto withdrawn = [&]() -> bool {
        if (!automatic_withdrawn()) return false;
        finish(receipt, LifecycleOutcome::cancelled,
               "App-private opt-in changed before the push; automatic start cancelled.");
        return true;
    };

    if (!already_probed_absent && (!clear_to_send(false) || withdrawn())) return;

    set_phase(LifecyclePhase::validating);
    if (!launcher_)
    {
        LaunchResult none;
        none.state = LaunchState::transport_unavailable;
        none.detail = "This build cannot start the companion itself.";
        { std::lock_guard lock(mutex_); view_.launch = none; }
        finish(receipt, LifecycleOutcome::launch_failed, after_stop + none.detail);
        return;
    }
    std::string bytes;
    const LaunchResult prepared = launcher_->prepare(
        kind == LifecycleKind::automatic ? LaunchKind::automatic : LaunchKind::requested, &bytes);
    if (expired() || cancel_requested(this))
    {
        finish(receipt, LifecycleOutcome::cancelled,
               "Operation cancelled or budget expired during package preparation; nothing was sent.");
        return;
    }
    if (prepared.state != LaunchState::not_attempted)
    {
        { std::lock_guard lock(mutex_); view_.launch = prepared; }
        finish(receipt, LifecycleOutcome::launch_failed, after_stop + prepared.detail);
        return;
    }

    if (!clear_to_send(true) || withdrawn()) return;

    // Linearize authorization and the start of submission under the same
    // lock as consent withdrawal/stop. Never hold this lock across IO: Off
    // after submission has begun cannot retract bytes already handed over.
    bool closing = false, revoked = false;
    {
        std::lock_guard lock(mutex_);
        closing = stopping_;
        revoked = kind == LifecycleKind::automatic && consent_withdrawn_;
        if (!closing && !revoked) view_.phase = LifecyclePhase::sending;
    }
    if (closing || revoked)
    {
        finish(receipt, LifecycleOutcome::cancelled,
               closing ? "App closing; companion start cancelled before anything was sent."
                       : "App-private opt-in changed before the push; automatic start cancelled.");
        return;
    }
    LaunchControl control;
    control.deadline_ms = remaining_ms(options_.transport_budget_ms);
    if (control.deadline_ms == 0 || cancel_requested(this))
    {
        finish(receipt, LifecycleOutcome::cancelled, "Operation cancelled or budget expired before submission; nothing was sent.");
        return;
    }
    control.cancel = &ServiceLifecycle::cancel_requested;
    control.cancel_ctx = this;
    const LaunchResult sent = launcher_->submit(bytes, control);
    {
        std::lock_guard lock(mutex_);
        view_.launch = sent;
        if (sent.state == LaunchState::sent) ++view_.sends;
    }
    if (sent.state == LaunchState::cancelled)
    {
        finish(receipt, LifecycleOutcome::cancelled, after_stop + sent.detail);
        return;
    }
    if (sent.state != LaunchState::sent && sent.state != LaunchState::outcome_unknown)
    {
        finish(receipt, LifecycleOutcome::launch_failed, after_stop + sent.detail);
        return;
    }

    /* A complete push, or one that stopped part-way (it might still run):
     * either way only a fresh compatible STATUS says anything started. */
    set_phase(LifecyclePhase::awaiting_ready);
    deadline_ = std::min(operation_end_, std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.ready_budget_ms));
    StatusVerdict last = StatusVerdict::empty;
    ProbeTransport last_transport = ProbeTransport::refused;
    for (;;)
    {
        if (!wait_or_stop(options_.ready_interval_ms))
        {
            finish(receipt, LifecycleOutcome::cancelled,
                   after_stop + "App closing while waiting; Ghosttooth API was sent and may still start.");
            return;
        }
        if (expired()) break;
        // Never pass a fractional remainder as client default zero, or erase
        // the last actual observation with a probe that did not execute.
        if (remaining_ms(options_.probe_timeout_ms) == 0) continue;
        StatusVerdict verdict = StatusVerdict::empty;
        CompanionIdentity found;
        const ProbeResult probed = probe_once(&verdict, &found);
        if (cancel_requested(this))
        {
            finish(receipt, LifecycleOutcome::cancelled, "App closing after transfer; Ghosttooth API may still start.");
            return;
        }
        if (expired()) break;
        // The remainder can cross below 1 ms between the caller's check and
        // probe_once's own budget read. That skipped exchange is not an
        // observation and must not erase the last actual foreign answer.
        if (remaining_ms(options_.probe_timeout_ms) == 0) continue;
        last_transport = probed.transport;
        if (probed.transport == ProbeTransport::answered)
        {
            last = verdict;
            if (verdict == StatusVerdict::compatible && (!replacing || found.boot != exclude_boot))
            {
                finish(receipt, replacing ? LifecycleOutcome::restarted : LifecycleOutcome::ready,
                       replacing ? "A different Ghosttooth API instance answered after the push."
                                 : "Ghosttooth API answered STATUS after the push.");
                return;
            }
        }
        if (expired()) break;
    }
    const std::string seconds = std::to_string((options_.ready_budget_ms + 999) / 1000);
    if (last_transport == ProbeTransport::answered)
        finish(receipt, LifecycleOutcome::identity_mismatch,
               after_stop + "After the push, port " + std::to_string(port) +
                   " answers but not as a fresh compatible Ghosttooth API (" +
                   (last == StatusVerdict::compatible ? "same old instance" : status_verdict_text(last)) + ").");
    else if (sent.state == LaunchState::outcome_unknown)
        finish(receipt, LifecycleOutcome::start_unconfirmed,
               after_stop + sent.detail + " Nothing answered within " + seconds + " s; nothing was retried.");
    else
        finish(receipt, LifecycleOutcome::start_unconfirmed,
               after_stop + "Ghosttooth API was handed to the loader but did not answer within " + seconds +
                   " s. It may still be starting; nothing was retried.");
}

} // namespace ghostcontrol

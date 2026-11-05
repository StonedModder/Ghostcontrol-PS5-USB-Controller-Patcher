/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_service_preferences_worker.hpp"
namespace ghostcontrol {
AppServicePreferencesWorker::AppServicePreferencesWorker(std::string path)
    : preferences_(std::move(path)) {}
AppServicePreferencesWorker::~AppServicePreferencesWorker() { stop(); }
void AppServicePreferencesWorker::start() {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return;
    stopping_ = false;
    const auto generation = view_.generation + 1;
    view_ = View{};
    view_.generation = generation;
    revoked_ = false;
    view_.available = true;
    view_.pending = true;
    thread_ = std::thread(&AppServicePreferencesWorker::work, this);
}
void AppServicePreferencesWorker::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
        ++view_.generation;
        view_.available = false;
        view_.startup_eligible = false;
        view_.pending = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}
AppServicePreferencesWorker::View AppServicePreferencesWorker::read() const {
    std::lock_guard lock(mutex_);
    return view_;
}
std::uint64_t AppServicePreferencesWorker::request(bool enabled) {
    std::lock_guard lock(mutex_);
    if (!view_.available || !view_.loaded || stopping_) return 0;
    // An opt-out press revokes permission even if the bounded queue refuses it.
    if (!enabled) { view_.startup_eligible = false; revoked_ = true; }
    if ((view_.pending && view_.requested == enabled) || queue_.size() >= 8) return 0;
    const auto generation = ++view_.generation;
    queue_.push_back({generation, enabled});
    revoked_ = !enabled;
    view_.requested = enabled;
    view_.pending = true;
    view_.startup_eligible = false;
    view_.error.clear();
    wake_.notify_one();
    return generation;
}
void AppServicePreferencesWorker::cancel_pending() {
    std::lock_guard lock(mutex_);
    ++view_.generation;
    queue_.clear();
    view_.pending = false;
    view_.startup_eligible = false;
    revoked_ = true;
    view_.error = "Local request cancelled; an in-flight commit cannot be undone.";
}
void AppServicePreferencesWorker::work() {
    std::uint64_t initial_generation;
    { std::lock_guard lock(mutex_); initial_generation = view_.generation; }
    const bool loaded = preferences_.load(); // outside every snapshot/queue lock
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        view_.loaded = true;
        view_.authoritative_known = loaded;
        view_.authoritative = loaded && preferences_.autostart();
        view_.verified = loaded;
        if (!revoked_) {
            view_.requested = view_.authoritative;
            view_.error = preferences_.error();
        }
        view_.pending = false;
        // A cancelled initial load cannot resurrect eligibility.
        view_.startup_eligible = loaded && view_.authoritative && !revoked_ && view_.generation == initial_generation;
    }
    for (;;) {
        Intent intent{};
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            intent = queue_.front(); queue_.pop_front();
        }
        const auto result = preferences_.set_autostart(intent.enabled);
        {
            std::lock_guard lock(mutex_);
            // Publish actual commit facts even for a late/cancelled receipt.
            // Preserve the last verified disk state on precommit failure.
            view_.saved = result.saved;
            view_.verified = result.verified;
            if (result.verified) {
                view_.authoritative_known = true;
                view_.authoritative = intent.enabled;
            } else if (result.saved) {
                view_.authoritative_known = false;
            }
            view_.receipt = intent.generation;
            if (stopping_) return;
            if (intent.generation != view_.generation) continue;
            view_.pending = false;
            view_.error = result.error;
            view_.startup_eligible = result.saved && result.verified && intent.enabled && !revoked_;
        }
    }
}
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "app_service_preferences.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
namespace ghostcontrol {
// One filesystem owner. Frame clients only copy a view or enqueue an intent.
class AppServicePreferencesWorker {
public:
    struct View {
        bool available = false, loaded = false, pending = false;
        bool requested = false, saved = false, verified = false;
        bool authoritative_known = false, authoritative = false;
        bool startup_eligible = false;
        // This module never launches anything. The one consumer of
        // startup_eligible is ServiceLifecycle::observe_consent (APP-SVC-02).
        bool automatic_launch_available = false;
        std::uint64_t generation = 0, receipt = 0;
        std::string error;
    };
    explicit AppServicePreferencesWorker(std::string path = AppServicePreferences::kPath);
    ~AppServicePreferencesWorker();
    void start();
    void stop(); // joins ordinary file IO; no transport or daemon EXIT
    std::uint64_t request(bool enabled); // 0: unavailable, duplicate or full
    void cancel_pending(); // local fence; cannot undo a committed write
    View read() const;
private:
    struct Intent { std::uint64_t generation; bool enabled; };
    void work();
    AppServicePreferences preferences_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Intent> queue_;
    View view_;
    bool stopping_ = false;
    bool revoked_ = false;
    std::thread thread_;
};
}

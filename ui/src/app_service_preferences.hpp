/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string>
#include <string_view>
#include <utility>
namespace ghostcontrol {
// App-private launch policy. This is not the resident service's RUNTIME_SET
// autostart preference and never requires a running companion.
struct AppPreferenceWrite {
    bool saved = false;
    bool verified = false;
    std::string error;
};
class AppServicePreferences {
public:
    /* Console-verified writable location (2026-10-18, PPSA84718): the title
     * sandbox refuses lstat/create of a file at the /download0 root with
     * EPERM ("inspect preference: Operation not permitted"), while the
     * /download0/ghostcontrol directory (created by app_log_open) accepts
     * writes. Keep the preference beside the app log. */
    static constexpr const char *kPath = "/download0/ghostcontrol/app-service.ini";
    explicit AppServicePreferences(std::string path = kPath) : path_(std::move(path)) {}
    bool load(); // Missing = opt-out; malformed/unreadable = opt-out plus error.
    AppPreferenceWrite set_autostart(bool enabled); // Explicit user intent only.
    bool autostart() const { return autostart_; }
    const std::string &error() const { return error_; }
    static bool decode(std::string_view text, bool *enabled);
    static std::string encode(bool enabled);
private:
    std::string path_;
    bool autostart_ = false;
    std::string error_;
};
} // namespace ghostcontrol

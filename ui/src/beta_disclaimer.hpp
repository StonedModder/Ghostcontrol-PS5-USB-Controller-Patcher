/* SPDX-License-Identifier: GPL-3.0-or-later */
// App-private launch preference: hide the beta disclaimer after "Don't show
// again". Separate from AppServicePreferences (autostart) so each file keeps
// one schema and a failed write cannot clear the other setting.
#pragma once
#include <string>
#include <string_view>
namespace ghostcontrol {
struct DisclaimerWrite {
    bool saved = false;
    bool verified = false;
    std::string error;
};
class BetaDisclaimerPreference {
public:
    /* Same console-verified writable directory as app-service.ini. */
    static constexpr const char *kPath = "/download0/ghostcontrol/ui-prefs.ini";
    explicit BetaDisclaimerPreference(std::string path = kPath) : path_(std::move(path)) {}
    // Missing = show the disclaimer. Malformed = show plus error.
    bool load();
    DisclaimerWrite set_dismissed(bool dismissed);
    bool dismissed() const { return dismissed_; }
    const std::string &error() const { return error_; }
    static bool decode(std::string_view text, bool *dismissed);
    static std::string encode(bool dismissed);
private:
    std::string path_;
    bool dismissed_ = false;
    std::string error_;
};
} // namespace ghostcontrol

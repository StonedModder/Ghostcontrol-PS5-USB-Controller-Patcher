/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app_service_preferences.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ghostcontrol {
namespace {
constexpr std::string_view kOff = "schema=1\nautostart_service=false\n";
constexpr std::string_view kOn = "schema=1\nautostart_service=true\n";
constexpr std::size_t kMaxBytes = 128;
std::string failure(const char *operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}
// Do not follow a link or block on a device/FIFO in an app preference path.
bool read_preference(const std::string &path, std::string *text,
                     bool *missing, std::string *error) {
    *missing = false;
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { *missing = true; return true; }
        *error = failure("read preference"); return false;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        *error = failure("inspect preference"); ::close(fd); return false;
    }
    // The title sandbox may leave nlink unreported (0). Still reject actual
    // multiple links; a Linux-only exact-one check rejects a valid PS5 save.
    if (!S_ISREG(info.st_mode) || info.st_nlink > 1 || info.st_size < 0 ||
        static_cast<unsigned long long>(info.st_size) > kMaxBytes) {
        *error = "Preference is not a bounded single-link regular file.";
        ::close(fd); return false;
    }
    text->clear();
    char buffer[kMaxBytes + 1];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) continue;
            *error = failure("read preference"); ::close(fd); return false;
        }
        if (!n) break;
        if (text->size() + static_cast<std::size_t>(n) > kMaxBytes) {
            *error = "Preference exceeds its bound."; ::close(fd); return false;
        }
        text->append(buffer, static_cast<std::size_t>(n));
    }
    if (::close(fd) != 0) { *error = failure("close preference"); return false; }
    return true;
}
}
bool AppServicePreferences::decode(std::string_view text, bool *enabled) {
    if (!enabled) return false;
    *enabled = false;
    if (text == kOff) return true;
    if (text == kOn) { *enabled = true; return true; }
    return false;
}
std::string AppServicePreferences::encode(bool enabled) {
    return std::string(enabled ? kOn : kOff);
}
bool AppServicePreferences::load() {
    autostart_ = false;
    error_.clear();
    std::string text;
    bool missing = false;
    if (!read_preference(path_, &text, &missing, &error_)) return false;
    if (missing) return true;
    if (!decode(text, &autostart_)) {
        error_ = "Preference is malformed or uses an unsupported schema; autostart is off.";
        return false;
    }
    return true;
}
AppPreferenceWrite AppServicePreferences::set_autostart(bool enabled) {
    AppPreferenceWrite result;
    // PS5 titles deny lstat with EPERM even where ordinary file IO works.
    // NOFOLLOW + nonblocking open/fstat preserves symlink/FIFO refusal.
    const int existing = ::open(path_.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (existing >= 0) {
        struct stat old{};
        if (::fstat(existing, &old) != 0) {
            result.error = failure("inspect preference");
            error_ = result.error; ::close(existing); return result;
        }
        if (::close(existing) != 0) {
            result.error = failure("close preference inspection");
            error_ = result.error; return result;
        }
        if (!S_ISREG(old.st_mode) || old.st_nlink > 1) {
            result.error = "Refusing to replace a nonregular or linked preference.";
            error_ = result.error; return result;
        }
    } else if (errno != ENOENT) {
        result.error = failure("inspect preference"); error_ = result.error; return result;
    }
    const std::string temporary = path_ + ".tmp";
    // A stale/external temporary file is not ours to truncate or delete.
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { result.error = failure("create preference temporary"); error_ = result.error; return result; }
    const std::string text = encode(enabled);
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (!n) errno = EIO;
            result.error = failure("write preference"); break;
        }
        written += static_cast<std::size_t>(n);
    }
    if (result.error.empty() && ::fsync(fd) != 0) result.error = failure("sync preference");
    if (::close(fd) != 0 && result.error.empty()) result.error = failure("close preference temporary");
    if (result.error.empty() && ::rename(temporary.c_str(), path_.c_str()) != 0)
        result.error = failure("publish preference");
    if (!result.error.empty()) {
        ::unlink(temporary.c_str());
        error_ = result.error; return result;
    }
    result.saved = true;
    // Publication is distinct from verified readback. Never report rollback
    // after a committed write or allow a stale cached true value to launch.
    if (!load() || autostart_ != enabled) {
        autostart_ = false;
        if (error_.empty()) error_ = "Preference committed but readback did not match.";
        result.error = error_; return result;
    }
    result.verified = true;
    return result;
}
}

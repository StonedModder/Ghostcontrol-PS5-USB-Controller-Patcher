/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cctype>
#include <map>
#include <string>
#include <string_view>
namespace ghostcontrol {
// Small validating JSON reader. Only top-level fields are exposed; nested or
// quoted "ok" text must never turn an error into a successful acknowledgement.
class JsonReply {
    std::string_view text_;
    size_t pos_ = 0;
    std::map<std::string, std::string_view> fields_;
    void space() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    bool take(char c) { space(); if (pos_ == text_.size() || text_[pos_] != c) return false; ++pos_; return true; }
    bool string() {
        if (pos_ == text_.size() || text_[pos_++] != '"') return false;
        while (pos_ < text_.size()) {
            unsigned char c = text_[pos_++];
            if (c == '"') return true;
            if (c < 32) return false;
            if (c == '\\') {
                if (pos_ == text_.size()) return false;
                char e = text_[pos_++];
                if (e == 'u') {
                    for (int i = 0; i < 4; ++i) if (pos_ == text_.size() || !std::isxdigit(static_cast<unsigned char>(text_[pos_++]))) return false;
                } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) return false;
            }
        }
        return false;
    }
    bool value(unsigned depth) {
        space(); if (pos_ == text_.size() || depth > 32) return false;
        char c = text_[pos_];
        if (c == '"') return string();
        if (c == '{' || c == '[') {
            ++pos_; const char end = c == '{' ? '}' : ']';
            if (take(end)) return true;
            do {
                if (c == '{') { space(); if (!string() || !take(':')) return false; }
                if (!value(depth + 1)) return false;
                if (take(end)) return true;
            } while (take(','));
            return false;
        }
        for (auto word : {std::string_view("true"), std::string_view("false"), std::string_view("null")})
            if (text_.substr(pos_, word.size()) == word) { pos_ += word.size(); return true; }
        if (c == '-') ++pos_;
        if (pos_ == text_.size()) return false;
        if (text_[pos_] == '0') ++pos_;
        else { if (text_[pos_] < '1' || text_[pos_] > '9') return false; while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
        if (pos_ < text_.size() && text_[pos_] == '.') { ++pos_; size_t start = pos_; while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_; if (start == pos_) return false; }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_; if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            size_t start = pos_; while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_; if (start == pos_) return false;
        }
        return true;
    }
public:
    bool valid = false;
    explicit JsonReply(std::string_view text) : text_(text) {
        if (!take('{')) return;
        if (!take('}')) {
            do {
                space(); size_t start = pos_; if (!string()) return;
                std::string key = decode(text_.substr(start, pos_ - start));
                if (!take(':')) return;
                space(); start = pos_; if (!value(1)) return;
                if (!fields_.emplace(key, text_.substr(start, pos_ - start)).second) return;
                if (take('}')) break;
                if (!take(',')) return;
            } while (true);
        }
        space(); valid = pos_ == text_.size();
    }
    std::string_view raw(std::string_view key) const {
        if (!valid) return {};
        auto it = fields_.find(std::string(key)); return it == fields_.end() ? std::string_view{} : it->second;
    }
    bool is_true(std::string_view key) const { return raw(key) == "true"; }
    bool success() const { return valid && is_true("ok"); }
    std::string string_field(std::string_view key) const { return decode(raw(key)); }
    static std::string decode(std::string_view raw) {
        if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return {};
        std::string out;
        for (size_t i = 1; i + 1 < raw.size(); ++i) {
            char c = raw[i];
            if (c == '\\') {
                c = raw[++i];
                switch (c) {
                case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
                case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'u': {
                    unsigned code = 0;
                    for (int n = 0; n < 4; ++n) { char h = raw[++i]; code = code * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
                    if (code < 128) out.push_back(static_cast<char>(code));
                    else if (code < 2048) { out.push_back(static_cast<char>(192 | (code >> 6))); out.push_back(static_cast<char>(128 | (code & 63))); }
                    else { out.push_back(static_cast<char>(224 | (code >> 12))); out.push_back(static_cast<char>(128 | ((code >> 6) & 63))); out.push_back(static_cast<char>(128 | (code & 63))); }
                    continue;
                }
                }
            }
            out.push_back(c);
        }
        return out;
    }
};
} // namespace ghostcontrol

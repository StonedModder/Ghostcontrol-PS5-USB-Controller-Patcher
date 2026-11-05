/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reader for the controller service's one-line replies.
 *
 * The service answers in three shapes and the UI has to read all of them:
 *
 *   - JSON-ish objects hand-built with snprintf in core/src/gc_main.c, e.g.
 *     {"ok":true,"generation":7,"controllers":[{"id":"054c-09cc",...},...]}
 *   - INI text, the profile the service serialises in core/src/gc_profile.c,
 *     e.g. "[profile]\nid=xbox-test\nleft.deadzone=0.100\n"
 *   - short plain acknowledgements, which carry no fields at all.
 *
 * The app links no JSON library and the PS5 build runs with exceptions off,
 * so this is a scanner, not a parser: nothing is allocated, nothing is
 * copied, and every accessor hands back a view into the caller's reply.
 * That also means the reply must outlive every view taken from it.
 *
 * Why a scanner and not strstr: the service's own field names defeat text
 * search. RUNTIME_GET carries "ipc_port" and "ghostpad_port" but no "port",
 * and "ghostpad_last_error" but no "error"; DUALSENSE_FX_STATUS carries
 * "errors", "last_error", "r" and "current_r"; STATUS carries "pid" and
 * "vid_pid" in the same object, and a controller "name" is free text that
 * can contain a quoted key of its own. Looking for a key by substring finds
 * the wrong field in all of those. So keys are matched whole, at the level
 * they appear, and string values are consumed as values and never searched.
 *
 * Truncation is a normal case, not an error case. The service caps a reply
 * at GC_IPC_MAX_RESPONSE (64 KiB) and the UI's own poll buffer is 32 KiB
 * (ui/src/service_snapshot.cpp), so a long list can arrive cut off mid
 * object. Every scan is bounded by the view it was given, and a value is
 * only handed out once its terminator has arrived -- so a cut reply yields
 * the fields and list entries that did arrive and nothing invented.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace ghostcontrol::reply
{

/**
 * Whether the service reported success.
 *
 * Every command path in gc_main.c writes either {"ok":true,...} or
 * {"ok":false,"error":...}, and gc_ipc.c fills in {"ok":false,...} for the
 * paths that leave the buffer empty, so a failure always says so and always
 * says so in the first field -- which is why this survives truncation.
 *
 * STATUS (gc_state.c snapshot_json) and the short acknowledgements carry no
 * flag at all; a reply that does not report failure did not fail. An empty
 * reply is not a success: nothing answered.
 */
bool ok(std::string_view reply);

/**
 * The error code, when ok() is false.
 *
 * Empty when the reply carries no "error" field. Note that STATUS has a
 * top-level "error" describing the *service's* state, which is the empty
 * string while it is healthy; ok() deliberately does not consult it.
 *
 * The failure replies that also carry "detail" keep it hex-encoded; read it
 * with field(reply, "detail") and decode it in the caller.
 */
std::string_view error(std::string_view reply);

/**
 * A flat scalar field, JSON or INI, as a view into `reply`.
 *
 * Only the top level is searched, so a field of a nested object or of a list
 * entry is not found here -- pass the nested value, or the list entry, back
 * in as `reply` to read inside it. A string value comes back without its
 * quotes and with the service's backslash escapes left as written.
 */
std::optional<std::string_view> field(std::string_view reply,
                                      std::string_view key);

/* Typed reads of a named field. Each returns no value when the field is
 * absent, when it is not a scalar, or when it does not parse -- so "absent"
 * and a legitimate 0, 0.0 or false stay distinguishable. */
std::optional<long long> integer(std::string_view reply, std::string_view key);
std::optional<unsigned long long> unsigned_integer(std::string_view reply,
                                                   std::string_view key);
std::optional<bool> boolean(std::string_view reply, std::string_view key);
std::optional<double> real(std::string_view reply, std::string_view key);

/* The same conversions on raw scalar text, for the elements of a list(). */
std::optional<long long> as_integer(std::string_view text);
std::optional<unsigned long long> as_unsigned_integer(std::string_view text);
std::optional<bool> as_boolean(std::string_view text);
std::optional<double> as_real(std::string_view text);

/**
 * The elements of a named JSON array, as views into the reply.
 *
 * An object element arrives whole, braces included, ready to hand back to
 * field() -- which is how a caller walks per-controller or per-device rows:
 *
 *     for (std::string_view row : reply::list(answer, "controllers"))
 *         use(reply::field(row, "id"), reply::boolean(row, "enabled"));
 *
 * A string element arrives without its quotes, so an array of plain names
 * ("profiles":["default","ds4-wired"]) reads the same way.
 *
 * Nothing is allocated: the view holds two pointers and a flag, and the
 * iterator scans the reply as it goes.
 */
class ArrayView
{
  public:
    /** Forward iterator over the elements. */
    class Iterator
    {
      public:
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;

        Iterator() = default;
        Iterator(std::string_view body, bool at_end);

        std::string_view operator*() const { return element_; }
        Iterator &operator++();

        bool operator==(const Iterator &other) const
        {
            /* Two finished iterators are equal wherever they stopped, so a
             * range-for over a cut-off array ends instead of running on. */
            return done_ == other.done_ && (done_ || position_ == other.position_);
        }
        bool operator!=(const Iterator &other) const { return !(*this == other); }

      private:
        std::string_view body_{};
        std::string_view element_{};
        std::size_t position_ = 0;
        bool done_ = true;
    };

    ArrayView() = default;
    ArrayView(std::string_view body, bool complete)
        : body_(body), complete_(complete)
    {
    }

    Iterator begin() const { return Iterator(body_, false); }
    Iterator end() const { return Iterator(body_, true); }

    bool empty() const { return begin() == end(); }

    /**
     * False when the array had no closing ']': the reply hit a size cap and
     * the tail of the list is missing. The elements that did arrive are
     * still complete and still safe to read; the caller decides whether to
     * say "showing the first N" or to ask again.
     */
    bool complete() const { return complete_; }

  private:
    std::string_view body_{};
    bool complete_ = false;
};

/** The named array, or an empty view when there is no such array. */
ArrayView list(std::string_view reply, std::string_view key);

} // namespace ghostcontrol::reply

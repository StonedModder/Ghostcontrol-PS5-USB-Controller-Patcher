/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A copy of what the controller service last reported, kept off the frame
 * thread.
 *
 * gc_client_send opens a socket and waits, with a three-second timeout. Called
 * from update() or draw() a slow or absent service would freeze the UI, which
 * is the single most common way homebrew feels broken. So one worker polls the
 * service and publishes here, and screens only ever read this.
 *
 * Commands the player triggers do not go to the service from the frame either:
 * they join a small FIFO the worker drains, and the result comes back as a
 * toast sentence. See docs/UI-DESIGN.md.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>

namespace ghostcontrol
{

class ServiceSnapshot
{
  public:
    enum class Link
    {
        connecting, // no answer yet; the service may still be starting
        up,
        down,
    };

    struct View
    {
        Link link = Link::connecting;
        std::string status_json; // the raw STATUS reply
        std::string detail;      // why it is down, when it is
        unsigned long long polls = 0;

        /* A generation counter for the whole view, and one string per service
         * endpoint the full fetch reads. Each is kept last-good across
         * failures, so a single unreachable endpoint cannot blank the screen.
         * STATUS itself is status_json above. */
        unsigned long long generation = 0;
        std::string runtime_json;
        std::string bluetooth_json;
        std::string profiles_json;
        std::string controllers_json;
        std::string devices_json;
        std::string console_id_json;
        std::string fx_json;
        std::string log_tail_json;
        std::string setup_json;
        std::string audio_json;
        // Observed by the owning app's frame thread, never inferred from VDA slots.
        std::string native_pad_json;

        /* The paired devices parsed from the last BLUETOOTH_STATUS reply.
         * `address` is the exact uppercase string BLUETOOTH_FORGET takes;
         * `label` is the '<kind> - <ADDRESS>' a picker row shows. The list is
         * replaced whole on every good fetch and kept across a failed one, so
         * it behaves like the raw bluetooth_json beside it. */
        struct PairedDevice
        {
            std::string address;
            std::string label;
        };
        std::vector<PairedDevice> bluetooth_paired;
    };

    ServiceSnapshot() = default;
    ~ServiceSnapshot();
    ServiceSnapshot(const ServiceSnapshot &) = delete;
    ServiceSnapshot &operator=(const ServiceSnapshot &) = delete;

    /** Starts the poll loop. Safe to call once. */
    void start();
    /** Stops it and joins. The destructor does this too. */
    void stop();

    /** The latest view. Cheap enough to call every frame. */
    View read() const;

    /** The paired devices in a BLUETOOTH_STATUS reply, parsed into an address
     *  and the '<kind> - <ADDRESS>' label a picker shows.
     *
     *  The full fetch calls this so the polled view carries the list; it is
     *  public so the snapshot harness can build the same list from its fixed
     *  JSON without starting a worker. One parser, so the harness and the app
     *  cannot drift. An empty reply yields no devices -- never a placeholder
     *  entry. Malformed, noncanonical (uppercase XX:XX:XX:XX:XX:XX) or
     *  duplicate address metadata rejects the whole list, never a subset. */
    static std::vector<View::PairedDevice> parse_bluetooth_paired(std::string_view bluetooth_json);

    struct ControllerRule { std::string id, output_type; };
    static bool parse_controller_rules(std::string_view json, std::vector<ControllerRule> &rules);
    static const char *controller_output_label(std::string_view type);
    static bool controller_output_type(std::string_view type);
    static std::string controller_rule_label(const ControllerRule &rule);

    // Validated identified physical input options, never STATUS/VDA slots.
    // selection is the bounded hex(path|vid|pid|profile|identity|generation).
    struct SetupSource {
        std::string label, selection, connection_generation;
        /* True when DEVICES already names a per-controller profile. Setup then
         * edits that profile instead of inventing a new one. */
        bool edits_existing = false;
    };
    static bool parse_setup_sources(std::string_view json, std::vector<SetupSource> &sources);
    // Formats validated bounded diagnostics; never treats app-native input as identified raw input.
    static std::vector<std::string> setup_raw_lines(std::string_view json);

    /** Adds one command to the worker's FIFO queue.
     *
     *  False when the command is empty, longer than the client allows, or
     *  carries a control character; when the argument is too long or carries
     *  one; when the queue is full; or while stopping. The frame thread calls
     *  this and never touches the service.
     *
     *  `timeout_ms` is the per-command deadline the worker gives this command
     *  on the wire. 0 (the default) keeps the client's usual three-second
     *  budget, so every existing caller is unchanged; a screen that knows its
     *  command is slow (UPDATE_CHECK reaches the network) passes a longer one
     *  so a legitimately slow command is not reported as a timeout. */
    bool queue_command(const char *command, const char *argument = nullptr,
                       unsigned timeout_ms = 0);

    /** Atomically queues a bounded group, e.g. all three RGB component writes.
     * All-or-none validation/capacity; no readback or poll can split the group. */
    bool queue_commands(const std::vector<std::pair<std::string, std::string>> &commands,
                        unsigned timeout_ms = 0);

    /** A command's reply, kept for a screen to read off the frame thread.
     *
     *  `queue_command` folds a reply into a toast sentence, which is all a
     *  plain action row needs. A screen that wants to *show* a reply -- the
     *  log reader decoding a LOG_TAIL, a picker listing PROFILE_FILES --
     *  queues the command here instead and reads the answer back with
     *  `last_reply`. Nothing blocks the frame on either path.
     *
     *  `body` is the service's one-line reply, empty until one lands (and
     *  empty again when the service could not be reached); `reached` says
     *  whether the service answered at all, so "waiting" and "failed" stay
     *  distinguishable; `generation` is bumped every time a fresh reply lands,
     *  so a reader can tell a new answer from the one already on screen. The
     *  generation is kept per command. */
    enum class ResultState { none, queued, acknowledged, applied, readback_failed, rejected, transport_failed, saved };
    struct Fetch
    {
        std::string body;
        bool reached = false;
        unsigned long long generation = 0;
        bool valid = false; // validated endpoint/ack JSON, not merely a connected socket
        ResultState state = ResultState::none;
        std::string detail;
    };

    /** Queue toggle intent, resolved against authoritative RUNTIME_GET on the worker.
     * Callers must not invert read().runtime_json for repeated button presses. */
    bool queue_runtime_toggle(const char *key);

    /** Adds one reply-capture command to the same worker FIFO.
     *
     *  Validation and capacity are exactly `queue_command`'s: false for an
     *  empty, oversized or control-carrying command or argument, when the
     *  queue is full, or while stopping. `timeout_ms` is the wire deadline,
     *  as above. Unlike `queue_command` this posts no toast; the reply is
     *  left in the fetch store for `last_reply`. */
    bool queue_fetch(const char *command, const char *argument = nullptr,
                     unsigned timeout_ms = 0);

    /** Queue one fetch with an instance-local nonzero request ID (0 on refusal).
     * The exact request's completion is retained independently of automatic polls
     * and other fetches. Same validation/FIFO/deadline as queue_fetch. At most 32
     * request records are retained; only completed records may be evicted. IDs
     * survive start/stop and refuse exhaustion, never wrap or get reused. */
    unsigned long long queue_fetch_request(const char *command, const char *argument = nullptr,
                                          unsigned timeout_ms = 0);
    /** queued until this specific attempt finishes; none for unknown/evicted IDs.
     * An evicted record is unavailable, never replaced by another attempt. */
    Fetch fetch_request(unsigned long long request_id) const;
    // Removes only still-queued work. Dispatched work cannot be retracted.
    bool cancel_fetch_request(unsigned long long request_id);

    /** The most recent reply captured for `command`, by generation order.
     *
     *  Replies to actions and automatic endpoint polls are captured too. The
     *  `valid` flag describes the latest attempt; View retains last-good fields
     *  across failures. Never treat a retained View field as fresh after an
     *  invalid last_reply(endpoint). Generation 0 means no completed attempt. */
    Fetch last_reply(std::string_view command) const;

    /** Latest-value frame mailboxes, never command FIFO entries. Poll and ACK
     * payloads are copied without network I/O. Unaccepted ACKs are retried;
     * identical accepted ACKs are deduplicated. */
    bool publish_native_pad(std::string_view poll, std::string_view ack);
    struct NativeExchange {
        std::string body;
        unsigned long long generation = 0;
        std::uint64_t sent_ms = 0; // CLOCK_MONOTONIC at SEND, not receipt
        bool valid = false;
    };
    NativeExchange native_pad_exchange() const;
    void publish_native_observation(std::string_view json);

    /** Asks the worker for a full fetch as soon as it can. */
    void refresh();

    /** Live input mode: polls SETUP_INPUT at ~30 Hz when the service is responsive.
     * Commands and explicit refresh/readback still have priority. */
    void set_live(bool enabled);

    /** Motion mode: poll DUALSENSE_FX_STATUS faster for the gyro / lights
     *  view. */
    void set_motion(bool enabled);

    /** The most recent SETUP_INPUT reply while live mode is on. */
    std::string live_input() const;

    /** The oldest pending command result, cleared on read. Empty when none. */
    std::string take_toast();

  private:
    struct Command
    {
        std::string name;
        std::string argument;
        /* The per-command wire deadline in ms; 0 means the client's default.
         * Carried with the command so the frame thread never reaches across
         * to the worker to change it. */
        unsigned timeout_ms = 0;
        /* True when the reply belongs in the fetch store (queue_fetch) rather
         * than being folded into a toast (queue_command). */
        bool fetch = false;
        bool toggle = false;
        bool batch_last = true;
        unsigned long long fetch_request_id = 0;
    };

    unsigned long long queue_fetch_impl(const char *command, const char *argument,
                                        unsigned timeout_ms, bool tracked);
    void run();
    void run_command(const Command &command, std::string &reply);
    void run_controller_output(const Command &command, std::string &reply);
    void run_setup_begin(const Command &command, std::string &reply);
    void run_setup_save(const Command &command, std::string &reply);
    void run_fetch(const Command &command, std::string &reply);
    void run_audio(const Command &command, std::string &reply);
    void fetch_status(std::string &reply);
    void fetch_endpoint(const char *name, std::string &reply);
    void readback_runtime(std::string &reply);
    void push_toast(const std::string &text);
    int exchange(const char *name, const char *arg, std::string &reply, unsigned timeout_ms = 0);
    void fetch_fx(std::string &reply);

    mutable std::mutex lock_;
    View view_;

    std::deque<Command> commands_;
    /* The last reply captured per command name, for a screen to read off the
     * frame thread (see queue_fetch / last_reply). */
    std::map<std::string, Fetch> fetches_;
    unsigned long long fetch_generation_ = 0;
    std::map<unsigned long long, Fetch> fetch_requests_;
    unsigned long long next_fetch_request_ = 0;
    std::string live_input_;
    std::string native_poll_, native_ack_;
    NativeExchange native_exchange_;
    std::deque<std::string> toasts_;
    std::vector<Command> runtime_pending_;
    std::condition_variable wake_;
    bool live_ = false;
    bool motion_ = false;
    bool refresh_ = false;
    bool stopping_ = false;

    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace ghostcontrol

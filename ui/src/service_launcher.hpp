/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Starting the companion from inside the app.
 *
 * A sandboxed title cannot fork/exec a process and has no /data to write an
 * ELF into, so "start the service" cannot mean spawning it. What the console
 * does offer is the loader: it accepts an ELF pushed to a TCP port and runs
 * it as the loading user. That is the only ordinary path, so the app pushes its
 * own packaged companion there when the service is not answering.
 *
 * Three rules this exists to keep honest:
 *
 *   1. Bytes accepted by the loader prove a transfer, not a running service.
 *      Only an answering STATUS may ever say the service is up.
 *   2. The app never invents success. A missing asset, a refused loader or a
 *      truncated push are each reported as themselves, with the reason.
 *   3. Automatic attempts are bounded. A refused loader is not retried every
 *      frame; a Start/Restart press always is, because that is the player
 *      asking again on purpose.
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ghostcontrol
{

/** What an attempt actually achieved. "sent" is deliberately weaker than
 *  "running", and there is no state that means both. */
enum class LaunchState
{
    not_attempted,
    no_asset,               // the companion is not packaged with this build
    rejected,               // the packaged bytes are not a usable ELF
    transport_unavailable,  // this build has no loader transport compiled in
    transport_unreachable,  // the loader refused, or the push did not complete
    sent,                   // bytes were handed over; service state still unknown
    already_sent,           // this build already pushed; not repeated
    outcome_unknown,        // partial OR full transfer with an uncertain exchange/startup outcome
                            // (never "not started")
    cancelled,              // stopped by the caller before any byte was sent
};

/** Automatic = the app doing it for you on launch. Requested = a player press. */
enum class LaunchKind
{
    automatic,
    requested,
};

struct LaunchResult
{
    LaunchState state = LaunchState::not_attempted;
    std::size_t bytes = 0;
    std::string detail;
    /* The manifest version of the package that was validated (APP-SVC-04). */
    std::string package_version;
    /* Never set from a push. Only a STATUS reply may fill this in. */
    LaunchState service_running = LaunchState::not_attempted;
    std::size_t payload_bytes = 0; // validated submission size; distinguishes partial/full observation
};

/** One absolute budget and a request-local cancel for a whole push:
 *  connect, every partial write, shutdown and the short reply wait share the
 *  same monotonic deadline (APP-SVC-03). It is never reset per byte. */
struct LaunchControl
{
    unsigned deadline_ms = 15000;
    int (*cancel)(void *) = nullptr; // nonzero = stop now; polled at least every 50 ms
    void *cancel_ctx = nullptr;
};

/** Bounded transport results. */
enum : int
{
    kLoaderComplete = 0,   // the loader took the whole payload
    kLoaderNotSent = -1,   // nothing was transferred (refused, unreachable, bad address)
    kLoaderPartial = -2,   // some bytes went out, then the exchange failed or ran out of time
    kLoaderCancelled = -3, // cancelled; `transferred` says whether anything went out
};

/** The push itself, injected so the policy is testable without a console. */
struct LaunchTransport
{
    /* Unbounded-signature form: returns 0 only when the loader took the whole
     * payload; any other value means nothing usable was sent. */
    int (*send_bytes)(void *ctx, const char *host, unsigned port,
                      const std::string &payload, std::string *detail) = nullptr;
    /* Bounded form, preferred when present. Returns one of kLoader*; reports
     * how many payload bytes the socket accepted in `transferred`. */
    int (*send_bounded)(void *ctx, const char *host, unsigned port, const std::string &payload,
                        const LaunchControl &control, std::string *detail,
                        std::size_t *transferred) = nullptr;
    void *ctx = nullptr;
};

/** The packaged companion, read through the app's own file API. */
struct LaunchAssets
{
    bool (*read)(const void *ctx, std::string_view path, std::string *out) = nullptr;
    void *ctx = nullptr;
};

class ServiceLauncher
{
  public:
    ServiceLauncher(LaunchTransport transport, LaunchAssets assets);

    /** Attempts a launch and reports exactly what happened. Never blocks the
     *  caller longer than one push with a bounded deadline. Equivalent to
     *  prepare() followed by submit(). */
    LaunchResult launch(LaunchKind kind);

    /** The first half of launch(): applies the automatic gate, then loads and
     *  validates the packaged companion without sending it. A result in state
     *  `not_attempted` means `bytes` now holds a usable ELF and nothing has
     *  been sent; any other state is the final outcome of this attempt. The
     *  lifecycle worker uses the split to re-probe STATUS between validating
     *  the package and handing it to the loader. */
    LaunchResult prepare(LaunchKind kind, std::string *bytes);

    /** The second half of launch(): hands bytes that prepare() accepted to the
     *  loader only when equal to its internally copied validated preparation.
     *  Failed/new prepare and cancellation invalidate authorization. No reread. */
    LaunchResult submit(const std::string &bytes, const LaunchControl &control = LaunchControl{});

    /** Whether this build already handed its companion to the loader. */
    bool pushed() const { return pushed_; }
    bool automatic_pending() const { return !attempted_; }
    const LaunchResult &last_result() const { return last_result_; }

    /** The path the packaged companion is read from, for the UI to show. */
    static constexpr std::string_view kAssetPath =
        "/app0/assets/ghostcontrol/GhosttoothAPI.elf";
    /** Its build-time manifest (tools/companion-manifest.py). Nothing is sent
     *  unless the ELF matches it exactly: name, byte count, SHA-256, and the
     *  STATUS protocol this app speaks. A manifest beside the ELF proves the
     *  two are consistent, not that either is authentic. */
    static constexpr std::string_view kManifestPath =
        "/app0/assets/ghostcontrol/GhosttoothAPI.manifest.json";

    /** Empty when `manifest` describes exactly `elf`; otherwise the reason.
     *  Fills `version` from the manifest when it matches. */
    static std::string check_manifest(std::string_view manifest, std::string_view elf,
                                      std::string *version = nullptr);
    static constexpr unsigned kLoaderPort = 9021;
    static constexpr std::string_view kLoaderHost = "127.0.0.1";

    /** Console builds replace this with the discovered LAN address. */
    void set_loader_host(std::string host) { if (!host.empty()) loader_host_ = std::move(host); }

  private:
    LaunchTransport transport_;
    LaunchAssets assets_;
    std::string loader_host_{kLoaderHost};
    std::string prepared_version_;
    std::string prepared_bytes_, prepared_manifest_;
    bool authorized_ = false;
    bool pushed_ = false;
    bool attempted_ = false;
    LaunchResult last_result_{};
};

} // namespace ghostcontrol

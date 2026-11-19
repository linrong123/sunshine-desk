/**
 * @file office/connect_confirm.h
 * @brief Per-connection host approval state machine for paired clients.
 *
 * The tracker is independent of networking, Win32, and configuration so every
 * transition can be tested with an injected clock. Clients are keyed by their
 * verified certificate PEM; the protocol is described in `office/CONNECTION.md`.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace office {
  /**
   * @brief Externally visible state of a client's connection request.
   */
  enum class connect_state_e {
    not_required,  ///< Host approval is not required for this host.
    pending,  ///< A request is waiting for the host user.
    approved,  ///< The host user approved, or a reconnection grace is active.
    denied,  ///< The host user denied the most recent request.
    expired,  ///< The most recent request was not answered in time.
    none,  ///< No request exists for this client.
  };

  /**
   * @brief Protocol spelling of a connection request state.
   * @param state State to convert.
   * @return Lowercase protocol token such as `pending`.
   */
  std::string_view to_string(connect_state_e state);

  /**
   * @brief Pending request shown to the host user.
   */
  struct connect_prompt_t {
    std::uint64_t id {};  ///< Opaque request identifier passed back to @ref connect_confirm_t::resolve.
    std::string name;  ///< Paired device name.
    std::string address;  ///< Source address of the request.
  };

  /**
   * @brief Result of @ref connect_confirm_t::start.
   */
  struct connect_start_t {
    connect_state_e state {connect_state_e::none};  ///< State after the call.
    bool limited {false};  ///< `true` when the global pending limit rejected a new request.
  };

  /**
   * @brief Thread-safe tracker for connection requests, one-time permits, and reconnection grace.
   *
   * A client starts at most one pending request; at most @ref max_pending requests exist globally.
   * Approval issues a permit valid for @ref permit_lifetime that the next launch or resume consumes.
   * When an admitted stream stops, the same certificate may reconnect without approval for
   * @ref grace_lifetime unless the client quit explicitly or its authorization was revoked.
   * A client whose admitted stream has not stopped yet (for example while the host is still
   * waiting for a dropped connection to time out) is admitted again without a new prompt. An
   * admission whose stream does not start within @ref permit_lifetime lapses, so a failed launch
   * does not leave the client approved.
   */
  class connect_confirm_t {
  public:
    using clock_t = std::chrono::steady_clock;  ///< Monotonic clock used for every deadline.
    using now_fn_t = std::function<clock_t::time_point()>;  ///< Injectable time source.

    static constexpr auto pending_lifetime = std::chrono::seconds {60};  ///< Time the host user has to answer.
    static constexpr auto result_retention = std::chrono::seconds {30};  ///< Time `denied` and `expired` stay queryable.
    static constexpr auto permit_lifetime = std::chrono::seconds {30};  ///< Validity of an approval permit.
    static constexpr auto grace_lifetime = std::chrono::seconds {120};  ///< Reconnection window after an admitted stream stops.
    static constexpr std::size_t max_pending = 3;  ///< Global limit of simultaneously pending requests.

    /**
     * @brief Create a tracker.
     * @param now Time source; defaults to `std::chrono::steady_clock::now`.
     */
    explicit connect_confirm_t(now_fn_t now = clock_t::now);

    /**
     * @brief Start a request or return the client's current one.
     * @param cert Verified client certificate PEM.
     * @param name Paired device name shown to the host user.
     * @param address Source address shown to the host user.
     * @return `approved` while admitted, within a grace period, or for an unused permit, `pending` for a new or existing
     *         request, or `none` with `limited` set when @ref max_pending requests are already waiting.
     */
    connect_start_t start(const std::string &cert, const std::string &name, const std::string &address);

    /**
     * @brief Query a client's request without changing it.
     * @param cert Verified client certificate PEM.
     * @return Current request state, `approved` while admitted or during a grace period, or `none`.
     */
    connect_state_e status(const std::string &cert);

    /**
     * @brief Withdraw a client's request or unused permit.
     * @param cert Verified client certificate PEM.
     * @return Always `none`.
     */
    connect_state_e cancel(const std::string &cert);

    /**
     * @brief Record the host user's answer.
     * @param id Request identifier from @ref pending.
     * @param allow `true` to approve, `false` to deny.
     * @return `true` if a pending request with this identifier was resolved.
     */
    bool resolve(std::uint64_t id, bool allow);

    /**
     * @brief Consume the right to start or resume one stream.
     * @param cert Verified client certificate PEM.
     * @return `true` if the client is already admitted, or a grace period or an approval permit was consumed.
     */
    bool admit(const std::string &cert);

    /**
     * @brief Note that a stream of this client started; its admission then lasts until the stream stops.
     * @param cert Client certificate PEM of the started stream.
     */
    void stream_started(const std::string &cert);

    /**
     * @brief Note that a stream of this client stopped without an explicit quit.
     * @param cert Client certificate PEM of the stopped stream.
     */
    void stream_stopped(const std::string &cert);

    /**
     * @brief Note an explicit quit; cancels any reconnection grace of the client.
     * @param cert Verified client certificate PEM.
     */
    void quit(const std::string &cert);

    /**
     * @brief Drop every request, permit, and grace for a disabled or deleted client.
     * @param cert Client certificate PEM.
     */
    void revoke(const std::string &cert);

    /**
     * @brief List pending requests for the host user, oldest first.
     * @return Pending requests.
     */
    std::vector<connect_prompt_t> pending();

    /**
     * @brief Drop all state, for example when approval is switched off.
     */
    void clear();

  private:
    /**
     * @brief Stored request of one client.
     */
    struct request_t {
      connect_state_e state {connect_state_e::none};  ///< `pending`, `approved`, `denied`, or `expired`.
      std::uint64_t id {};  ///< Request identifier.
      std::string name;  ///< Paired device name.
      std::string address;  ///< Source address.
      clock_t::time_point deadline;  ///< Expiry of the current state.
    };

    /**
     * @brief Apply every deadline that has passed; caller holds the mutex.
     * @param now Current time.
     */
    void prune(clock_t::time_point now);

    now_fn_t now_;  ///< Time source.
    std::mutex mutex_;  ///< Serializes every member below.
    std::map<std::string, request_t, std::less<>> requests_;  ///< Requests keyed by certificate.
    std::map<std::string, clock_t::time_point, std::less<>> grace_;  ///< Reconnection grace deadlines keyed by certificate.
    std::map<std::string, std::optional<clock_t::time_point>, std::less<>> active_;  ///< Admitted certificates; the value is the start deadline until a stream starts.
    std::uint64_t next_id_ {1};  ///< Next request identifier.
  };
}  // namespace office

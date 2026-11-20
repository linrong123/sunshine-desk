/**
 * @file office/adaptive_fec.h
 * @brief Adaptive video FEC: per-session loss estimation from client feedback and FEC percentage control.
 *
 * The design is specified in `office/ADAPTIVE_FEC.md`. Moonlight clients send one
 * `SS_FRAME_FEC_STATUS` message (control type 0x5502) for every FEC block that needed recovery or
 * could not be recovered. The host turns those messages into an unbiased packet-loss estimate and
 * raises the video FEC percentage above the configured `fec_percentage` while loss persists.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>

// local includes
#include "src/config.h"

namespace office {
  inline constexpr std::string_view adaptive_fec_key = "desk_adaptive_fec";  ///< Configuration key, `enabled` (default) or `disabled`.
  inline constexpr std::string_view adaptive_fec_max_key = "desk_adaptive_fec_max";  ///< Configuration key for the FEC percentage ceiling.
  inline constexpr int adaptive_fec_default_max = 60;  ///< Default ceiling for the adaptive FEC percentage.
  inline constexpr std::uint16_t frame_fec_status_type = 0x5502;  ///< Control message type of `SS_FRAME_FEC_STATUS` sent by the client.
  inline constexpr std::size_t frame_fec_status_size = 21;  ///< Bytes of `SS_FRAME_FEC_STATUS` fields; clients send 24 including padding.

  /** @brief Whether new sessions adapt the FEC percentage; written by `apply_adaptive_fec_config`. */
  inline std::atomic_bool adaptive_fec_enabled {true};

  /** @brief Ceiling for the adaptive FEC percentage; written by `apply_adaptive_fec_config`. */
  inline std::atomic_int adaptive_fec_max {adaptive_fec_default_max};

  /** @brief Decoded `SS_FRAME_FEC_STATUS`, converted from big-endian wire order. */
  struct frame_fec_status_t {
    std::uint32_t frame_index {};  ///< Frame number of the reported block.
    std::uint16_t highest_received_sequence {};  ///< Highest RTP sequence number received in the block.
    std::uint16_t next_contiguous_sequence {};  ///< First sequence number of the block not received in order.
    std::uint16_t missing_before_highest {};  ///< Packets missing below the highest received sequence number.
    std::uint16_t total_data {};  ///< Data shards in the block.
    std::uint16_t total_parity {};  ///< Parity shards in the block.
    std::uint16_t received_data {};  ///< Data shards received.
    std::uint16_t received_parity {};  ///< Parity shards received.
    std::uint8_t fec_percentage {};  ///< FEC percentage the block was sent with.
    std::uint8_t block_index {};  ///< Index of the block within its frame.
    std::uint8_t block_count {};  ///< Number of FEC blocks in the frame.
  };

  /**
   * @brief Decode a frame FEC status payload.
   * @param payload Message body after the 4-byte control header.
   * @return The status, or `std::nullopt` if the payload is too short or structurally invalid.
   */
  std::optional<frame_fec_status_t> parse_frame_fec_status(std::string_view payload);

  /** @brief Bernoulli trials and losses contributed by one FEC block report. */
  struct loss_observation_t {
    std::uint32_t trials {};  ///< Packets observed after the first lost packet.
    std::uint32_t losses {};  ///< Packets lost among those trials.
  };

  /**
   * @brief Convert one block report into an unbiased loss observation.
   *
   * Clients only report blocks whose first loss falls in the data shards. Conditioned on that first
   * loss at offset `j`, the packets after it are fresh trials, and the client stops observing at a
   * stopping time (the D-th received packet, or the end of the block), so the ratio of summed
   * losses to summed trials estimates the packet loss rate without bias. Reports with `j == 0`,
   * clean or stale blocks, and inconsistent counts are rejected.
   *
   * @param status Decoded report.
   * @return The observation, or `std::nullopt` if the report carries no usable signal.
   */
  std::optional<loss_observation_t> observe_frame_fec_status(const frame_fec_status_t &status);

  /** @brief Tuning of the adaptive FEC controller. */
  struct adaptive_fec_params_t {
    int floor {20};  ///< Lowest percentage; the configured `fec_percentage`.
    int ceiling {adaptive_fec_default_max};  ///< Highest percentage.
    int step {5};  ///< Granularity of the percentage and size of each decrease.
    int headroom {10};  ///< Percentage points added to twice the loss rate.
    double hysteresis {0.025};  ///< Loss margin added before a decrease is allowed.
    std::uint32_t min_trials {200};  ///< Trials below which the estimate is shrunk toward zero.
    std::chrono::milliseconds window {2000};  ///< Sliding window for the loss estimate.
    std::chrono::milliseconds rise_interval {1000};  ///< Minimum time between two changes.
    std::chrono::milliseconds decay_hold {10000};  ///< Time a lower target must persist before each decrease.
  };

  /**
   * @brief Percentage that protects against a packet loss rate.
   *
   * `ceil_step(2 * loss_percent + headroom)` clamped to `[floor, ceiling]`; zero loss maps to the floor.
   *
   * @param loss Packet loss rate between 0 and 1.
   * @param params Controller tuning.
   * @return Target percentage.
   */
  int adaptive_fec_target(double loss, const adaptive_fec_params_t &params);

  /** @brief One percentage change, for logging. */
  struct adaptive_fec_change_t {
    int from {};  ///< Previous percentage.
    int to {};  ///< New percentage.
    double loss {};  ///< Loss estimate that drove the change.
    std::uint64_t trials {};  ///< Trials in the window behind the estimate.
  };

  /**
   * @brief Pure controller: block reports in, FEC percentage out.
   *
   * Not thread-safe; one thread feeds reports and calls `update`. Rises go straight to the target,
   * decreases go one step at a time after the lower target has held for `decay_hold`.
   */
  class adaptive_fec_controller_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Time source of all time points.

    /**
     * @brief Start at the floor.
     * @param params Tuning; the ceiling is raised to the floor if lower.
     */
    explicit adaptive_fec_controller_t(adaptive_fec_params_t params);

    /**
     * @brief Feed one decoded report.
     * @param status Decoded report.
     * @param now Arrival time.
     * @return `true` if the report contributed to the estimate.
     */
    bool add(const frame_fec_status_t &status, clock::time_point now);

    /**
     * @brief Re-evaluate the percentage.
     * @param now Current time.
     * @return The change, if the percentage moved.
     */
    std::optional<adaptive_fec_change_t> update(clock::time_point now);

    /** @brief Current percentage. @return Percentage in `[floor, ceiling]`. */
    int percentage() const;

    /** @brief Tuning in effect. @return Parameters after clamping. */
    const adaptive_fec_params_t &params() const;

  private:
    /** @brief One accepted observation in the sliding window. */
    struct entry_t {
      clock::time_point time;  ///< Arrival time.
      loss_observation_t observation;  ///< Trials and losses.
    };

    /** @brief Drop observations older than the window. @param now Current time. */
    void expire(clock::time_point now);

    /** @brief Loss estimate, shrunk toward zero below `min_trials`. @return Loss rate between 0 and 1. */
    double estimate() const;

    adaptive_fec_params_t params_;  ///< Tuning after clamping.
    int percentage_;  ///< Current percentage.
    std::deque<entry_t> window_;  ///< Observations inside the window, oldest first.
    std::uint64_t trials_ {};  ///< Sum of trials in `window_`.
    std::uint64_t losses_ {};  ///< Sum of losses in `window_`.
    std::array<std::uint64_t, 8> recent_blocks_ {};  ///< Recently accepted block keys, to drop repeated reports.
    std::size_t recent_count_ {};  ///< Valid entries in `recent_blocks_`.
    std::size_t recent_next_ {};  ///< Next slot to overwrite in `recent_blocks_`.
    std::optional<clock::time_point> last_change_;  ///< Time of the last change.
    std::optional<clock::time_point> lower_since_;  ///< Since when a lower target has held.
  };

  /**
   * @brief Adaptive FEC state owned by one streaming session.
   *
   * `percentage` is written by the control thread and read by the video thread; the controller is
   * touched only by the control thread after `adaptive_fec_start`.
   */
  struct adaptive_fec_session_t {
    std::atomic_int percentage {-1};  ///< Current percentage, or -1 when the session does not adapt.
    std::unique_ptr<adaptive_fec_controller_t> controller;  ///< Controller; null when the session does not adapt.
  };

  /**
   * @brief Reset state at session start, before the control and video threads see the session.
   * @param state Session state.
   * @param configured_percentage Configured `fec_percentage`, used as the floor.
   * @param session_id Launch session ID for the log.
   */
  void adaptive_fec_start(adaptive_fec_session_t &state, int configured_percentage, std::uint32_t session_id);

  /**
   * @brief Handle one 0x5502 payload on the control thread; malformed payloads are ignored.
   * @param state Session state.
   * @param payload Message body after the 4-byte control header.
   */
  void adaptive_fec_feedback(adaptive_fec_session_t &state, std::string_view payload);

  /**
   * @brief Re-evaluate on the control thread and log a change at info level.
   * @param state Session state.
   * @param now Current time.
   * @param session_id Launch session ID for the log.
   */
  void adaptive_fec_tick(adaptive_fec_session_t &state, adaptive_fec_controller_t::clock::time_point now, std::uint32_t session_id);

  /**
   * @brief Percentage for the next video frame, read on the video thread.
   * @param state Session state.
   * @param configured_percentage Configured `fec_percentage`.
   * @return The adaptive percentage, or `configured_percentage` when the session does not adapt.
   */
  int adaptive_fec_percentage(const adaptive_fec_session_t &state, int configured_percentage);

  /**
   * @brief Apply `desk_adaptive_fec` and `desk_adaptive_fec_max`, removing them from @p vars.
   * @param vars Parsed configuration; office keys are consumed.
   */
  void apply_adaptive_fec_config(config::config_map_t &vars);
}  // namespace office

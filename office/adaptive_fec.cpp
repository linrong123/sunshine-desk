/**
 * @file office/adaptive_fec.cpp
 * @brief Adaptive video FEC: report decoding, loss estimation, controller, and configuration.
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>

// local includes
#include "office/adaptive_fec.h"
#include "office/authorization.h"
#include "src/logging.h"

using namespace std::literals;

namespace office {
  namespace {
    /** @brief Read a big-endian 16-bit field. @param data Field start. @return Host-order value. */
    std::uint16_t read_be16(const unsigned char *data) {
      return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    }

    /** @brief Read a big-endian 32-bit field. @param data Field start. @return Host-order value. */
    std::uint32_t read_be32(const unsigned char *data) {
      return (std::uint32_t {data[0]} << 24) | (std::uint32_t {data[1]} << 16) | (std::uint32_t {data[2]} << 8) | std::uint32_t {data[3]};
    }

    /**
     * @brief Identify a block so repeated reports of it are counted once.
     * @param status Decoded report.
     * @return Key combining frame and block index.
     */
    std::uint64_t block_key(const frame_fec_status_t &status) {
      return (std::uint64_t {status.frame_index} << 8) | status.block_index;
    }

    /**
     * @brief Format a loss rate as a percentage with one decimal.
     * @param loss Loss rate between 0 and 1.
     * @return Text such as `14.8`.
     */
    std::string format_percent(const double loss) {
      const auto permille = static_cast<long long>(std::lround(loss * 1000.0));
      return std::to_string(permille / 10) + '.' + std::to_string(permille % 10);
    }

    /**
     * @brief Parse a percentage between 0 and 100.
     * @param value Configuration value.
     * @return The value, or `std::nullopt` if it is not an integer in range.
     */
    std::optional<int> parse_percentage(std::string_view value) {
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
      }
      int result = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
      if (value.empty() || error != std::errc {} || end != value.data() + value.size() || result < 0 || result > 100) {
        return std::nullopt;
      }
      return result;
    }
  }  // namespace

  std::optional<frame_fec_status_t> parse_frame_fec_status(const std::string_view payload) {
    if (payload.size() < frame_fec_status_size) {
      return std::nullopt;
    }
    const auto *data = reinterpret_cast<const unsigned char *>(payload.data());
    frame_fec_status_t status;
    status.frame_index = read_be32(data);
    status.highest_received_sequence = read_be16(data + 4);
    status.next_contiguous_sequence = read_be16(data + 6);
    status.missing_before_highest = read_be16(data + 8);
    status.total_data = read_be16(data + 10);
    status.total_parity = read_be16(data + 12);
    status.received_data = read_be16(data + 14);
    status.received_parity = read_be16(data + 16);
    status.fec_percentage = data[18];
    status.block_index = data[19];
    status.block_count = data[20];

    // The protocol carries at most 4 FEC blocks per frame (2 bits).
    if (status.block_count == 0 || status.block_count > 4 || status.block_index >= status.block_count || status.total_data == 0) {
      return std::nullopt;
    }
    return status;
  }

  std::optional<loss_observation_t> observe_frame_fec_status(const frame_fec_status_t &status) {
    const std::uint32_t data = status.total_data;
    const std::uint32_t total = data + status.total_parity;
    const std::uint32_t received = std::uint32_t {status.received_data} + status.received_parity;
    const std::uint32_t missing = status.missing_before_highest;

    // A report with every data shard received describes a clean (stale) block, not a loss.
    if (data < 2 || status.received_data >= data || status.received_parity > status.total_parity || received == 0 || received + missing > total) {
      return std::nullopt;
    }

    // The client counts packets from the first sequence number of the block, so the span from the
    // lowest to the highest received sequence number holds every received and missing packet.
    const auto lowest = static_cast<std::uint16_t>(status.highest_received_sequence - (received + missing - 1));
    const std::uint32_t first_loss = static_cast<std::uint16_t>(status.next_contiguous_sequence - lowest);

    // Offset 0 is excluded: the block is only known to the client because a later packet arrived,
    // which would bias the trials that follow. Offsets at or past the data shards are never reported.
    if (first_loss < 1 || first_loss >= data || first_loss > received) {
      return std::nullopt;
    }

    if (received >= data) {
      // Recovered: the client stopped at the D-th received packet, the highest one.
      if (missing < 1) {
        return std::nullopt;
      }
      return loss_observation_t {received + missing - first_loss - 1, missing - 1};
    }

    // Unrecoverable: every packet after the first loss was observed, received or not.
    return loss_observation_t {total - first_loss - 1, total - received - 1};
  }

  int adaptive_fec_target(const double loss, const adaptive_fec_params_t &params) {
    if (!(loss > 0.0)) {
      return params.floor;
    }
    const auto step = std::max(params.step, 1);
    const auto wanted = 2.0 * loss * 100.0 + params.headroom;
    const auto rounded = static_cast<int>(std::ceil(wanted / step - 1e-9)) * step;
    return std::clamp(rounded, params.floor, params.ceiling);
  }

  adaptive_fec_controller_t::adaptive_fec_controller_t(adaptive_fec_params_t params):
      params_ {params} {
    params_.floor = std::clamp(params_.floor, 0, 100);
    params_.ceiling = std::clamp(params_.ceiling, params_.floor, 100);
    params_.step = std::max(params_.step, 1);
    percentage_ = params_.floor;
  }

  bool adaptive_fec_controller_t::add(const frame_fec_status_t &status, const clock::time_point now) {
    const auto observation = observe_frame_fec_status(status);
    if (!observation) {
      return false;
    }

    // The client may report the same block twice (a skipped block re-sends the previous state).
    const auto key = block_key(status);
    for (std::size_t i = 0; i < recent_count_; ++i) {
      if (recent_blocks_[i] == key) {
        return false;
      }
    }
    recent_blocks_[recent_next_] = key;
    recent_next_ = (recent_next_ + 1) % recent_blocks_.size();
    recent_count_ = std::min(recent_count_ + 1, recent_blocks_.size());

    window_.push_back({now, *observation});
    trials_ += observation->trials;
    losses_ += observation->losses;
    return true;
  }

  void adaptive_fec_controller_t::expire(const clock::time_point now) {
    while (!window_.empty() && now - window_.front().time > params_.window) {
      trials_ -= window_.front().observation.trials;
      losses_ -= window_.front().observation.losses;
      window_.pop_front();
    }
  }

  double adaptive_fec_controller_t::estimate() const {
    // Few trials mean few loss reports; shrinking toward zero keeps thin evidence from raising FEC.
    const auto denominator = std::max<std::uint64_t>(trials_, params_.min_trials);
    return denominator == 0 ? 0.0 : static_cast<double>(losses_) / static_cast<double>(denominator);
  }

  std::optional<adaptive_fec_change_t> adaptive_fec_controller_t::update(const clock::time_point now) {
    expire(now);
    const auto loss = estimate();
    const auto change_allowed = [&](const std::chrono::milliseconds interval) {
      return !last_change_ || now - *last_change_ >= interval;
    };
    const auto apply = [&](const int to) {
      adaptive_fec_change_t change {percentage_, to, loss, trials_};
      percentage_ = to;
      last_change_ = now;
      return change;
    };

    // Rise straight to the target.
    if (const auto target = adaptive_fec_target(loss, params_); target > percentage_) {
      lower_since_.reset();
      if (change_allowed(params_.rise_interval)) {
        return apply(target);
      }
      return std::nullopt;
    }

    // Decrease one step once a lower target, computed with a loss margin, has held long enough.
    const auto lower = losses_ == 0 ? params_.floor : adaptive_fec_target(loss + params_.hysteresis, params_);
    if (lower >= percentage_) {
      lower_since_.reset();
      return std::nullopt;
    }
    if (!lower_since_) {
      lower_since_ = now;
    }
    if (now - *lower_since_ >= params_.decay_hold && change_allowed(params_.decay_hold)) {
      lower_since_ = now;
      return apply(std::max(percentage_ - params_.step, lower));
    }
    return std::nullopt;
  }

  int adaptive_fec_controller_t::percentage() const {
    return percentage_;
  }

  const adaptive_fec_params_t &adaptive_fec_controller_t::params() const {
    return params_;
  }

  void adaptive_fec_start(adaptive_fec_session_t &state, const int configured_percentage, const std::uint32_t session_id) {
    state.controller.reset();
    state.percentage.store(-1, std::memory_order_relaxed);

    const auto ceiling = adaptive_fec_max.load();
    // FEC explicitly turned off (0%) is respected; a ceiling at or below the floor leaves nothing to adapt.
    if (!adaptive_fec_enabled.load() || configured_percentage <= 0 || ceiling <= configured_percentage) {
      return;
    }

    adaptive_fec_params_t params;
    params.floor = configured_percentage;
    params.ceiling = ceiling;
    state.controller = std::make_unique<adaptive_fec_controller_t>(params);
    state.percentage.store(state.controller->percentage(), std::memory_order_relaxed);
    BOOST_LOG(info) << "Adaptive FEC [session "sv << session_id << "]: "sv << params.floor << "% to "sv << params.ceiling << '%';
  }

  void adaptive_fec_feedback(adaptive_fec_session_t &state, const std::string_view payload) {
    if (!state.controller) {
      return;
    }
    if (const auto status = parse_frame_fec_status(payload)) {
      state.controller->add(*status, adaptive_fec_controller_t::clock::now());
    }
  }

  void adaptive_fec_tick(adaptive_fec_session_t &state, const adaptive_fec_controller_t::clock::time_point now, const std::uint32_t session_id) {
    if (!state.controller) {
      return;
    }
    const auto change = state.controller->update(now);
    if (!change) {
      return;
    }
    state.percentage.store(change->to, std::memory_order_relaxed);
    BOOST_LOG(info) << "Adaptive FEC [session "sv << session_id << "]: "sv << change->from << "% -> "sv << change->to
                    << "% (loss "sv << format_percent(change->loss) << "% over "sv << change->trials << " packets)"sv;
  }

  int adaptive_fec_percentage(const adaptive_fec_session_t &state, const int configured_percentage) {
    const auto percentage = state.percentage.load(std::memory_order_relaxed);
    return percentage < 0 ? configured_percentage : percentage;
  }

  void apply_adaptive_fec_config(config::config_map_t &vars) {
    bool enabled = true;
    if (const auto it = vars.find(adaptive_fec_key); it != vars.end()) {
      if (const auto parsed = parse_config_switch(it->second)) {
        enabled = *parsed;
      } else {
        BOOST_LOG(warning) << "Invalid value for "sv << adaptive_fec_key << ": "sv << it->second << "; using enabled"sv;
      }
      vars.erase(it);
    }
    adaptive_fec_enabled.store(enabled);

    int ceiling = adaptive_fec_default_max;
    if (const auto it = vars.find(adaptive_fec_max_key); it != vars.end()) {
      if (const auto parsed = parse_percentage(it->second)) {
        ceiling = *parsed;
      } else {
        BOOST_LOG(warning) << "Invalid value for "sv << adaptive_fec_max_key << ": "sv << it->second << "; using "sv << adaptive_fec_default_max;
      }
      vars.erase(it);
    }
    adaptive_fec_max.store(ceiling);
  }
}  // namespace office

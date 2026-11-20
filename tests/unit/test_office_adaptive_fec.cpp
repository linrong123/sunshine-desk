/**
 * @file tests/unit/test_office_adaptive_fec.cpp
 * @brief Tests for adaptive FEC: report decoding, loss estimation, controller behavior, and configuration.
 */
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

// local includes
#include "office/adaptive_fec.h"
#include "src/config.h"

using namespace std::literals;

namespace {
  using clock_type = office::adaptive_fec_controller_t::clock;

  /**
   * @brief Encode a report exactly as the client sends it: big-endian fields, padded to 24 bytes.
   * @param status Report to encode.
   * @return Payload after the 4-byte control header.
   */
  std::string encode(const office::frame_fec_status_t &status) {
    std::string out;
    const auto put16 = [&](const std::uint16_t value) {
      out.push_back(static_cast<char>(value >> 8));
      out.push_back(static_cast<char>(value & 0xFF));
    };
    out.push_back(static_cast<char>(status.frame_index >> 24));
    out.push_back(static_cast<char>((status.frame_index >> 16) & 0xFF));
    out.push_back(static_cast<char>((status.frame_index >> 8) & 0xFF));
    out.push_back(static_cast<char>(status.frame_index & 0xFF));
    put16(status.highest_received_sequence);
    put16(status.next_contiguous_sequence);
    put16(status.missing_before_highest);
    put16(status.total_data);
    put16(status.total_parity);
    put16(status.received_data);
    put16(status.received_parity);
    out.push_back(static_cast<char>(status.fec_percentage));
    out.push_back(static_cast<char>(status.block_index));
    out.push_back(static_cast<char>(status.block_count));
    out.append(3, '\0');
    return out;
  }

  /**
   * @brief Reproduce the report a Moonlight client sends for one FEC block (RtpVideoQueue.c).
   *
   * Packets arrive in order. The client completes the block on the D-th received packet and reports
   * only if it needed recovery; a block with fewer than D received packets is reported as
   * unrecoverable; a block with no received packet is unknown to the client.
   *
   * @param frame Frame index.
   * @param base Sequence number of the first shard.
   * @param data Data shards.
   * @param percentage FEC percentage.
   * @param received Reception flag per shard, data first.
   * @return The report, or `std::nullopt` when the client sends none.
   */
  std::optional<office::frame_fec_status_t> client_report(const std::uint32_t frame, const std::uint16_t base, const int data, const int percentage, const std::vector<bool> &received) {
    office::frame_fec_status_t status;
    status.frame_index = frame;
    status.total_data = static_cast<std::uint16_t>(data);
    status.total_parity = static_cast<std::uint16_t>((data * percentage + 99) / 100);
    status.fec_percentage = static_cast<std::uint8_t>(percentage);
    status.block_count = 1;
    const int total = data + status.total_parity;

    int count = 0;
    int missing = 0;
    int highest = -1;
    int next_contiguous = 0;
    for (int i = 0; i < total; ++i) {
      if (!received[i]) {
        continue;
      }
      missing += highest < 0 ? i : i - highest - 1;
      highest = i;
      ++count;
      if (i < data) {
        ++status.received_data;
      } else {
        ++status.received_parity;
      }
      while (next_contiguous <= i && received[next_contiguous]) {
        ++next_contiguous;
      }
      if (count == data) {
        if (status.received_data == data) {
          return std::nullopt;  // Complete without recovery: not reported.
        }
        break;
      }
    }
    if (count == 0) {
      return std::nullopt;
    }
    status.highest_received_sequence = static_cast<std::uint16_t>(base + highest);
    status.next_contiguous_sequence = static_cast<std::uint16_t>(base + next_contiguous);
    status.missing_before_highest = static_cast<std::uint16_t>(missing);
    return status;
  }

  /**
   * @brief Build a report that contributes exactly the given trials and losses.
   * @param frame Frame index; distinct frames avoid duplicate suppression.
   * @param trials Trials, at least 1.
   * @param losses Losses, at most `trials - 1`.
   * @return Unrecoverable FEC-less block with its first loss at offset 1.
   */
  office::frame_fec_status_t synthetic(const std::uint32_t frame, const int trials, const int losses) {
    const int data = trials + 2;
    std::vector<bool> received(data, false);
    const int count = data - 1 - losses;
    received[0] = true;
    for (int i = 2; i <= count; ++i) {
      received[i] = true;
    }
    return *client_report(frame, 1000, data, 0, received);
  }

  /** @brief Default tuning with the given floor and ceiling. @param floor Floor. @param ceiling Ceiling. @return Parameters. */
  office::adaptive_fec_params_t params_with(const int floor, const int ceiling) {
    office::adaptive_fec_params_t params;
    params.floor = floor;
    params.ceiling = ceiling;
    return params;
  }

  /** @brief Changes recorded while driving a controller. */
  struct trace_t {
    std::vector<office::adaptive_fec_change_t> changes;  ///< Every change in order.
    std::vector<clock_type::time_point> times;  ///< Time of each change.
  };

  /**
   * @brief Drive a controller for a period, feeding one synthetic block and one update every 100 ms.
   * @param controller Controller under test.
   * @param start Start time; advanced to the end of the period.
   * @param duration Period to simulate.
   * @param trials Trials per block, 0 for no reports.
   * @param losses Losses per block.
   * @param frame Frame counter, advanced per block.
   * @param trace Receives the changes.
   */
  void drive(office::adaptive_fec_controller_t &controller, clock_type::time_point &start, const std::chrono::milliseconds duration, const int trials, const int losses, std::uint32_t &frame, trace_t &trace) {
    const auto end = start + duration;
    for (; start < end; start += 100ms) {
      if (trials > 0) {
        controller.add(synthetic(frame++, trials, losses), start);
      }
      if (const auto change = controller.update(start)) {
        trace.changes.push_back(*change);
        trace.times.push_back(start);
      }
    }
  }
}  // namespace

TEST(OfficeAdaptiveFecTest, ParseDecodesBigEndianFields) {
  office::frame_fec_status_t status;
  status.frame_index = 0x01020304;
  status.highest_received_sequence = 0xFFFE;
  status.next_contiguous_sequence = 0x1234;
  status.missing_before_highest = 3;
  status.total_data = 40;
  status.total_parity = 8;
  status.received_data = 37;
  status.received_parity = 3;
  status.fec_percentage = 20;
  status.block_index = 2;
  status.block_count = 4;

  const auto payload = encode(status);
  ASSERT_EQ(payload.size(), 24U);
  const auto parsed = office::parse_frame_fec_status(payload);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->frame_index, 0x01020304U);
  EXPECT_EQ(parsed->highest_received_sequence, 0xFFFE);
  EXPECT_EQ(parsed->next_contiguous_sequence, 0x1234);
  EXPECT_EQ(parsed->missing_before_highest, 3);
  EXPECT_EQ(parsed->total_data, 40);
  EXPECT_EQ(parsed->total_parity, 8);
  EXPECT_EQ(parsed->received_data, 37);
  EXPECT_EQ(parsed->received_parity, 3);
  EXPECT_EQ(parsed->fec_percentage, 20);
  EXPECT_EQ(parsed->block_index, 2);
  EXPECT_EQ(parsed->block_count, 4);

  // The 21 field bytes are enough; padding is optional.
  EXPECT_TRUE(office::parse_frame_fec_status(std::string_view {payload}.substr(0, office::frame_fec_status_size)).has_value());
}

TEST(OfficeAdaptiveFecTest, ParseRejectsShortAndMalformedPayloads) {
  EXPECT_FALSE(office::parse_frame_fec_status({}).has_value());
  EXPECT_FALSE(office::parse_frame_fec_status("\x01\x02\x03"sv).has_value());

  office::frame_fec_status_t status;
  status.total_data = 10;
  status.block_count = 1;
  const auto valid = encode(status);
  ASSERT_TRUE(office::parse_frame_fec_status(valid).has_value());
  EXPECT_FALSE(office::parse_frame_fec_status(std::string_view {valid}.substr(0, office::frame_fec_status_size - 1)).has_value());

  auto bad = status;
  bad.block_count = 0;
  EXPECT_FALSE(office::parse_frame_fec_status(encode(bad)).has_value());
  bad.block_count = 5;
  EXPECT_FALSE(office::parse_frame_fec_status(encode(bad)).has_value());
  bad = status;
  bad.block_index = 1;
  EXPECT_FALSE(office::parse_frame_fec_status(encode(bad)).has_value());
  bad = status;
  bad.total_data = 0;
  EXPECT_FALSE(office::parse_frame_fec_status(encode(bad)).has_value());

  // Arbitrary bytes never crash the session wrapper.
  office::adaptive_fec_session_t session;
  office::adaptive_fec_start(session, 20, 1);
  office::adaptive_fec_feedback(session, {});
  office::adaptive_fec_feedback(session, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"sv);
  office::adaptive_fec_tick(session, clock_type::now(), 1);
  EXPECT_EQ(office::adaptive_fec_percentage(session, 20), 20);
}

TEST(OfficeAdaptiveFecTest, ObserveRecoveredBlock) {
  // D=10, P=2; shard 3 lost; the client recovers on the 10th received packet (index 10).
  std::vector<bool> received(12, true);
  received[3] = false;
  const auto status = client_report(7, 65530, 10, 20, received);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->received_data, 9);
  EXPECT_EQ(status->received_parity, 1);
  EXPECT_EQ(status->missing_before_highest, 1);

  const auto observation = office::observe_frame_fec_status(*status);
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->trials, 7U);  // Indexes 4..10.
  EXPECT_EQ(observation->losses, 0U);
}

TEST(OfficeAdaptiveFecTest, ObserveUnrecoverableBlock) {
  // D=10, P=2; shards 2, 5, 8 lost; only 9 of 10 needed packets arrive.
  std::vector<bool> received(12, true);
  received[2] = received[5] = received[8] = false;
  const auto status = client_report(8, 100, 10, 20, received);
  ASSERT_TRUE(status.has_value());

  const auto observation = office::observe_frame_fec_status(*status);
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->trials, 9U);  // Indexes 3..11.
  EXPECT_EQ(observation->losses, 2U);  // Shards 5 and 8.
}

TEST(OfficeAdaptiveFecTest, ObserveRejectsReportsWithoutSignal) {
  std::vector<bool> received(12, true);

  // First shard lost: the block is only known because a later packet arrived.
  received[0] = false;
  auto status = client_report(1, 0, 10, 20, received);
  ASSERT_TRUE(status.has_value());
  EXPECT_FALSE(office::observe_frame_fec_status(*status).has_value());

  // A stale report of a clean block.
  office::frame_fec_status_t clean;
  clean.total_data = 10;
  clean.total_parity = 2;
  clean.received_data = 10;
  clean.highest_received_sequence = 9;
  clean.next_contiguous_sequence = 10;
  clean.block_count = 1;
  EXPECT_FALSE(office::observe_frame_fec_status(clean).has_value());

  // Counts that cannot fit in the block.
  received.assign(12, true);
  received[3] = false;
  status = client_report(2, 0, 10, 20, received);
  ASSERT_TRUE(status.has_value());
  auto inconsistent = *status;
  inconsistent.missing_before_highest = 50;
  EXPECT_FALSE(office::observe_frame_fec_status(inconsistent).has_value());
  inconsistent = *status;
  inconsistent.received_parity = 3;
  EXPECT_FALSE(office::observe_frame_fec_status(inconsistent).has_value());
  inconsistent = *status;
  inconsistent.next_contiguous_sequence = static_cast<std::uint16_t>(inconsistent.next_contiguous_sequence + 20);
  EXPECT_FALSE(office::observe_frame_fec_status(inconsistent).has_value());

  // Single-shard blocks carry no trials after a first loss.
  inconsistent = *status;
  inconsistent.total_data = 1;
  EXPECT_FALSE(office::observe_frame_fec_status(inconsistent).has_value());
}

TEST(OfficeAdaptiveFecTest, EstimatorIsUnbiasedUnderIndependentLoss) {
  std::mt19937 rng {12345};
  const auto uniform = [&]() {
    return static_cast<double>(rng()) / 4294967296.0;
  };

  for (const double loss : {0.02, 0.05, 0.15, 0.30}) {
    for (const int percentage : {0, 20, 40, 60}) {
      std::uint64_t trials = 0;
      std::uint64_t losses = 0;
      std::uint16_t base = 65000;
      for (std::uint32_t frame = 0; frame < 20000; ++frame) {
        const int data = 2 + static_cast<int>(rng() % 59);
        const int total = data + (data * percentage + 99) / 100;
        std::vector<bool> received(total);
        for (int i = 0; i < total; ++i) {
          received[i] = uniform() >= loss;
        }
        if (const auto status = client_report(frame, base, data, percentage, received)) {
          if (const auto observation = office::observe_frame_fec_status(*status)) {
            trials += observation->trials;
            losses += observation->losses;
          }
        }
        base = static_cast<std::uint16_t>(base + total);
      }
      ASSERT_GT(trials, 50000U);
      const auto estimate = static_cast<double>(losses) / static_cast<double>(trials);
      EXPECT_NEAR(estimate, loss, 0.01) << "loss " << loss << " at " << percentage << "%";
    }
  }
}

TEST(OfficeAdaptiveFecTest, TargetMapsLossToClampedSteps) {
  const auto params = params_with(20, 60);
  EXPECT_EQ(office::adaptive_fec_target(0.0, params), 20);
  EXPECT_EQ(office::adaptive_fec_target(0.05, params), 20);
  EXPECT_EQ(office::adaptive_fec_target(0.10, params), 30);
  EXPECT_EQ(office::adaptive_fec_target(0.101, params), 35);
  EXPECT_EQ(office::adaptive_fec_target(0.15, params), 40);
  EXPECT_EQ(office::adaptive_fec_target(0.25, params), 60);
  EXPECT_EQ(office::adaptive_fec_target(0.50, params), 60);
  EXPECT_EQ(office::adaptive_fec_target(0.05, params_with(30, 60)), 30);
}

TEST(OfficeAdaptiveFecTest, ZeroLossStaysAtFloor) {
  office::adaptive_fec_controller_t controller {params_with(20, 60)};
  auto now = clock_type::time_point {} + 1h;
  std::uint32_t frame = 0;
  trace_t trace;

  // No reports at all, then recovered blocks with a single (first) loss and nothing after it.
  drive(controller, now, 30s, 0, 0, frame, trace);
  drive(controller, now, 30s, 300, 0, frame, trace);
  EXPECT_TRUE(trace.changes.empty());
  EXPECT_EQ(controller.percentage(), 20);
}

TEST(OfficeAdaptiveFecTest, RisesFastToTarget) {
  office::adaptive_fec_controller_t controller {params_with(20, 60)};
  auto now = clock_type::time_point {} + 1h;
  std::uint32_t frame = 0;
  trace_t trace;

  drive(controller, now, 100ms, 300, 45, frame, trace);  // 15% loss.
  ASSERT_EQ(trace.changes.size(), 1U);
  EXPECT_EQ(trace.changes[0].from, 20);
  EXPECT_EQ(trace.changes[0].to, 40);
  EXPECT_NEAR(trace.changes[0].loss, 0.15, 1e-9);
  EXPECT_EQ(trace.changes[0].trials, 300U);

  // Loss rises again shortly after: the next increase waits for the minimum interval.
  drive(controller, now, 500ms, 300, 75, frame, trace);  // 25% loss.
  EXPECT_EQ(controller.percentage(), 40);
  drive(controller, now, 1s, 300, 75, frame, trace);
  EXPECT_EQ(controller.percentage(), 60);
  ASSERT_EQ(trace.changes.size(), 2U);
  EXPECT_GE(trace.times[1] - trace.times[0], 1s);
}

TEST(OfficeAdaptiveFecTest, ThinEvidenceDoesNotRaise) {
  office::adaptive_fec_controller_t controller {params_with(20, 60)};
  auto now = clock_type::time_point {} + 1h;
  std::uint32_t frame = 0;
  trace_t trace;

  // One lossy block of 10 trials every 5 s: shrunk far below the threshold of a rise.
  for (int i = 0; i < 12; ++i) {
    controller.add(synthetic(frame++, 10, 5), now);
    drive(controller, now, 5s, 0, 0, frame, trace);
  }
  EXPECT_TRUE(trace.changes.empty());
}

TEST(OfficeAdaptiveFecTest, ClampsToCeilingAndFloor) {
  office::adaptive_fec_controller_t controller {params_with(20, 45)};
  auto now = clock_type::time_point {} + 1h;
  std::uint32_t frame = 0;
  trace_t trace;
  drive(controller, now, 2s, 300, 150, frame, trace);  // 50% loss.
  EXPECT_EQ(controller.percentage(), 45);

  // A ceiling below the floor collapses to the floor.
  office::adaptive_fec_controller_t inverted {params_with(70, 60)};
  EXPECT_EQ(inverted.params().ceiling, 70);
  trace_t inverted_trace;
  drive(inverted, now, 2s, 300, 150, frame, inverted_trace);
  EXPECT_TRUE(inverted_trace.changes.empty());
  EXPECT_EQ(inverted.percentage(), 70);
}

TEST(OfficeAdaptiveFecTest, DecaysSlowlyOneStepAtATime) {
  office::adaptive_fec_controller_t controller {params_with(20, 60)};
  auto now = clock_type::time_point {} + 1h;
  std::uint32_t frame = 0;
  trace_t trace;

  drive(controller, now, 3s, 300, 45, frame, trace);
  ASSERT_EQ(controller.percentage(), 40);
  const auto rise_time = trace.times.back();

  drive(controller, now, 120s, 0, 0, frame, trace);  // Loss stops.
  ASSERT_EQ(trace.changes.size(), 5U);
  const int expected[] = {35, 30, 25, 20};
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(trace.changes[i + 1].to, expected[i]);
    const auto previous = i == 0 ? rise_time : trace.times[i];
    EXPECT_GE(trace.times[i + 1] - previous, 10s);
  }
  EXPECT_EQ(controller.percentage(), 20);
}

TEST(OfficeAdaptiveFecTest, HysteresisPreventsFlapping) {
  office::adaptive_fec_controller_t controller {params_with(20, 60)};
  auto now = clock_type::time_point {} + 1h;
  std::uint32_t frame = 0;
  trace_t trace;

  // Loss alternates between 9% and 11% every second: targets 30 and 35.
  for (int i = 0; i < 60; ++i) {
    drive(controller, now, 1s, 300, i % 2 == 0 ? 27 : 33, frame, trace);
  }
  ASSERT_FALSE(trace.changes.empty());
  for (const auto &change : trace.changes) {
    EXPECT_GT(change.to, change.from) << "decrease from " << change.from << " to " << change.to;
  }
  EXPECT_LE(trace.changes.size(), 2U);
  EXPECT_EQ(controller.percentage(), 35);
}

TEST(OfficeAdaptiveFecTest, DuplicateReportsCountOnce) {
  office::adaptive_fec_controller_t controller {params_with(20, 60)};
  const auto now = clock_type::time_point {} + 1h;
  const auto status = synthetic(42, 300, 45);
  EXPECT_TRUE(controller.add(status, now));
  EXPECT_FALSE(controller.add(status, now));
  auto other_block = status;
  other_block.block_count = 2;
  other_block.block_index = 1;
  EXPECT_TRUE(controller.add(other_block, now));
}

TEST(OfficeAdaptiveFecTest, SessionFollowsSwitchAndConfiguredFloor) {
  const auto enabled = office::adaptive_fec_enabled.load();
  const auto ceiling = office::adaptive_fec_max.load();

  office::adaptive_fec_session_t session;
  office::adaptive_fec_enabled = false;
  office::adaptive_fec_max = 60;
  office::adaptive_fec_start(session, 20, 1);
  EXPECT_EQ(session.controller, nullptr);
  EXPECT_EQ(office::adaptive_fec_percentage(session, 20), 20);
  EXPECT_EQ(office::adaptive_fec_percentage(session, 25), 25);  // Follows the configured value, as upstream.

  office::adaptive_fec_enabled = true;
  office::adaptive_fec_start(session, 0, 2);  // FEC turned off by configuration stays off.
  EXPECT_EQ(session.controller, nullptr);
  EXPECT_EQ(office::adaptive_fec_percentage(session, 0), 0);

  office::adaptive_fec_start(session, 20, 3);
  ASSERT_NE(session.controller, nullptr);
  EXPECT_EQ(office::adaptive_fec_percentage(session, 20), 20);

  auto now = clock_type::now();
  for (std::uint32_t frame = 0; frame < 5; ++frame) {
    office::adaptive_fec_feedback(session, encode(synthetic(frame, 300, 45)));
  }
  office::adaptive_fec_tick(session, now, 3);
  EXPECT_EQ(office::adaptive_fec_percentage(session, 20), 40);

  // A new session starts again at the floor.
  office::adaptive_fec_start(session, 20, 4);
  EXPECT_EQ(office::adaptive_fec_percentage(session, 20), 20);

  office::adaptive_fec_enabled = enabled;
  office::adaptive_fec_max = ceiling;
}

TEST(OfficeAdaptiveFecTest, ConfigParsesAndConsumesKeys) {
  const auto enabled = office::adaptive_fec_enabled.load();
  const auto ceiling = office::adaptive_fec_max.load();

  config::config_map_t vars;
  vars.emplace(std::string {office::adaptive_fec_key}, "disabled");
  vars.emplace(std::string {office::adaptive_fec_max_key}, " 75 ");
  office::apply_adaptive_fec_config(vars);
  EXPECT_FALSE(office::adaptive_fec_enabled.load());
  EXPECT_EQ(office::adaptive_fec_max.load(), 75);
  EXPECT_TRUE(vars.empty());

  vars.emplace(std::string {office::adaptive_fec_key}, "maybe");
  vars.emplace(std::string {office::adaptive_fec_max_key}, "101");
  office::apply_adaptive_fec_config(vars);
  EXPECT_TRUE(office::adaptive_fec_enabled.load());
  EXPECT_EQ(office::adaptive_fec_max.load(), office::adaptive_fec_default_max);
  EXPECT_TRUE(vars.empty());

  vars.emplace(std::string {office::adaptive_fec_max_key}, "abc");
  office::apply_adaptive_fec_config(vars);
  EXPECT_EQ(office::adaptive_fec_max.load(), office::adaptive_fec_default_max);

  office::apply_adaptive_fec_config(vars);  // Missing keys mean the defaults.
  EXPECT_TRUE(office::adaptive_fec_enabled.load());
  EXPECT_EQ(office::adaptive_fec_max.load(), office::adaptive_fec_default_max);

  office::adaptive_fec_enabled = enabled;
  office::adaptive_fec_max = ceiling;
}

TEST(OfficeAdaptiveFecTest, LargeFrameFallsBackToFloorBeforeSkippingFec) {
  // Same formula as stream::fec::max_data_shards: 255 shards shared by data and parity.
  constexpr auto max_data_shards = [](const int percentage) {
    return (255 * 100) / (100 + percentage);
  };
  constexpr std::size_t blocksize = 1000;
  constexpr std::size_t max_blocks = 4;
  const auto fit = [&](const std::size_t packets, const int percentage, const int floor) {
    return office::fit_fec_blocks(packets * blocksize, blocksize, percentage, floor, max_blocks, max_data_shards);
  };

  // 159 data shards per block at 60%, 212 at 20%.
  auto result = fit(600, 60, 20);  // 4 blocks at 60% fit.
  EXPECT_EQ(result.percentage, 60);
  EXPECT_EQ(result.blocks, 4u);

  result = fit(700, 60, 20);  // 5 blocks at 60%, 4 at the floor.
  EXPECT_EQ(result.percentage, 20);
  EXPECT_EQ(result.blocks, 4u);

  result = fit(900, 60, 20);  // Too large even at the floor: the caller skips FEC as upstream does.
  EXPECT_EQ(result.percentage, 20);
  EXPECT_EQ(result.blocks, 5u);

  result = fit(900, 20, 20);  // Not adapting: unchanged from upstream.
  EXPECT_EQ(result.percentage, 20);
  EXPECT_EQ(result.blocks, 5u);

  result = fit(1, 60, 20);
  EXPECT_EQ(result.percentage, 60);
  EXPECT_EQ(result.blocks, 1u);
}

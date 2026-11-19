/**
 * @file office/display_scale.h
 * @brief Authenticated, target-bound Windows desktop scaling policy and backend contract.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace office {
  /** @brief Validated absolute system scaling values for a single display source. */
  struct display_scale_values {
    int current {};  ///< Current system scaling percentage, never a client-side zoom value.
    int recommended {};  ///< System-recommended scaling percentage.
    std::vector<int> supported;  ///< Exact supported scaling percentages in ascending order.
  };

  /** @brief A backend snapshot retaining private target identity only inside the server. */
  struct display_scale_snapshot {
    std::string identity;  ///< Private source and topology identity; never serialized to clients.
    display_scale_values values;  ///< Scaling values read from the selected capture display.
  };

  /** @brief Backend or protocol outcome; a successful read always carries a snapshot. */
  struct display_scale_result {
    int status {503};  ///< GameStream XML status, with 200 reserved for verified success.
    std::string message;  ///< Human-readable failure explanation without machine identity.
    std::optional<display_scale_snapshot> snapshot;  ///< Verified system state, if available.
    std::string token;  ///< Random process-local target token safe to send to the paired client.
  };

  /** @brief Injectable operating-system boundary; tests must install a synthetic implementation. */
  class display_scale_backend {
  public:
    /** @brief Destroy the operating-system backend. */
    virtual ~display_scale_backend() = default;

    /**
     * @brief Read the uniquely selected capture source and its actual scaling.
     * @return Snapshot on success, otherwise an explicit failure without guessed values.
     */
    virtual display_scale_result read() = 0;

    /**
     * @brief Revalidate the expected target and apply exactly one supported scale.
     * @param expected Fresh snapshot identifying the capture source and topology.
     * @param scale Exact supported absolute percentage; implementations must not clamp.
     * @return Status of the operating-system write; the caller independently reads it back.
     */
    virtual display_scale_result write(const display_scale_snapshot &expected, int scale) = 0;
  };

  /**
   * @brief Decode the undocumented Windows relative scale indices without clamping.
   * @param minimum Minimum relative index, whose absolute scale is 100 percent.
   * @param current Current relative index returned by Windows.
   * @param maximum Maximum relative index returned by Windows.
   * @return Validated absolute values, or no value for custom, unknown or invalid indices.
   */
  std::optional<display_scale_values> decode_display_scale(std::int32_t minimum, std::int32_t current, std::int32_t maximum);

  /**
   * @brief Serialize read/modify/read operations and enforce strict target-bound request policy.
   * @param write True for POST writes, false for parameterless GET reads.
   * @param query Decoded query pairs, preserving duplicate and unknown parameters.
   * @return GameStream outcome with actual system values only after a verified read.
   */
  display_scale_result request_display_scale(bool write, const std::vector<std::pair<std::string, std::string>> &query);

#ifdef _WIN32
  /**
   * @brief Construct the Windows DisplayConfig implementation without changing any display state.
   * @return Backend that targets only Sunshine's configured capture source.
   */
  std::unique_ptr<display_scale_backend> make_windows_display_scale_backend();
#endif

#ifdef SUNSHINE_TESTS
  /**
   * @brief Replace the backend and invalidate tokens while holding the request mutex.
   * @param backend Synthetic backend, or nullptr to restore the production platform backend.
   */
  void set_display_scale_backend_for_test(std::unique_ptr<display_scale_backend> backend);
#endif
}  // namespace office

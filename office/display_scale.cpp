/**
 * @file office/display_scale.cpp
 * @brief Strict display scaling requests, opaque target tokens, and verified readback.
 */
#include "display_scale.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

namespace office {
  namespace {
    constexpr std::array scales {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};  ///< Known Windows standard DPI percentages.

    /** @brief Request state protected as one unit to prevent interleaved reads and writes. */
    struct scale_service {
      std::mutex mutex;  ///< Serializes requests, backend replacement, and token changes.
      std::unique_ptr<display_scale_backend> backend;  ///< Lazily constructed platform backend.
      std::string identity;  ///< Last successfully queried private topology identity.
      std::string token;  ///< Random opaque token for the last queried topology.
    };

    /** @brief Get process-local service state. @return Shared state for all paired clients. */
    scale_service &service() {
      static scale_service value;
      return value;
    }

    /**
     * @brief Generate an opaque token without embedding monitor or host identity.
     * @return 128 random bits encoded as 32 hexadecimal characters.
     */
    std::string random_token() {
      std::random_device random;
      std::ostringstream token;
      token << std::hex << std::setfill('0');
      for (int i = 0; i < 4; ++i) {
        token << std::setw(8) << static_cast<std::uint32_t>(random());
      }
      return token.str();
    }

    /**
     * @brief Reject malformed snapshots even if a platform reports success.
     * @param snapshot Values and private identity returned by the backend.
     * @return Whether all percentages are known, ordered, and internally consistent.
     */
    bool valid_snapshot(const display_scale_snapshot &snapshot) {
      const auto &values = snapshot.values;
      const auto &supported = values.supported;
      return !snapshot.identity.empty() && !supported.empty() &&
             std::is_sorted(supported.begin(), supported.end()) &&
             std::adjacent_find(supported.begin(), supported.end()) == supported.end() &&
             std::all_of(supported.begin(), supported.end(), [](int value) {
               return std::find(scales.begin(), scales.end(), value) != scales.end();
             }) &&
             std::find(supported.begin(), supported.end(), values.current) != supported.end() && std::find(supported.begin(), supported.end(), values.recommended) != supported.end();
    }

    /**
     * @brief Query the platform and attach a fresh token whenever the target changes.
     * @param state Locked process-local service state.
     * @return Verified snapshot and token, or a failure that invalidates cached identity.
     */
    display_scale_result read(scale_service &state) {
      if (!state.backend) {
#ifdef _WIN32
        state.backend = make_windows_display_scale_backend();
#else
        return {501, "System display scaling is supported only on Windows", {}, {}};
#endif
      }
      auto result = state.backend->read();
      if (result.status != 200 || !result.snapshot || !valid_snapshot(*result.snapshot)) {
        state.identity.clear();
        state.token.clear();
        if (result.status == 200) {
          return {503, "The display returned unknown or inconsistent scaling values", {}, {}};
        }
        result.snapshot.reset();
        result.token.clear();
        return result;
      }
      if (state.identity != result.snapshot->identity) {
        state.identity = result.snapshot->identity;
        state.token = random_token();
      }
      result.token = state.token;
      return result;
    }
  }  // namespace

  std::optional<display_scale_values> decode_display_scale(std::int32_t minimum, std::int32_t current, std::int32_t maximum) {
    // Widen before negation/addition, including INT_MIN and hostile driver data.
    const auto recommended = -static_cast<std::int64_t>(minimum);
    const auto last = recommended + maximum;
    const auto selected = recommended + current;
    if (minimum > 0 || maximum < 0 || current < minimum || current > maximum || recommended < 0 || recommended >= static_cast<std::int64_t>(scales.size()) || last < recommended || last >= static_cast<std::int64_t>(scales.size()) || selected < 0 || selected > last) {
      return std::nullopt;
    }
    return display_scale_values {scales[selected], scales[recommended], {scales.begin(), scales.begin() + last + 1}};
  }

  display_scale_result request_display_scale(bool write, const std::vector<std::pair<std::string, std::string>> &query) {
    auto &state = service();
    std::lock_guard lock {state.mutex};
    int requested {};
    std::string token;
    if (!write && !query.empty()) {
      return {400, "GET requires an empty query", {}, {}};
    }
    if (write) {
      if (query.size() != 2) {
        return {400, "POST requires exactly one scale and one displayToken", {}, {}};
      }
      bool found_scale = false;
      for (const auto &[key, value] : query) {
        if (key == "scale" && !found_scale && !value.empty()) {
          found_scale = true;
          const auto parsed = std::from_chars(value.data(), value.data() + value.size(), requested);
          if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || std::to_string(requested) != value) {
            return {400, "scale must be a canonical integer percentage", {}, {}};
          }
        } else if (key == "displayToken" && token.empty() && !value.empty()) {
          token = value;
        } else {
          return {400, "Unexpected, empty or duplicate parameter", {}, {}};
        }
      }
      if (!found_scale || token.empty()) {
        return {400, "POST requires scale and displayToken", {}, {}};
      }
      if (token.size() != 32 || !std::all_of(token.begin(), token.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
          })) {
        return {400, "displayToken must be the 32-character lowercase hexadecimal token from GET", {}, {}};
      }
    }
    auto before = read(state);
    if (before.status != 200 || !write) {
      return before;
    }
    if (token != before.token) {
      return {409, "The capture display changed; read the current display before retrying", {}, {}};
    }
    const auto &supported = before.snapshot->values.supported;
    if (std::find(supported.begin(), supported.end(), requested) == supported.end()) {
      return {422, "The requested scale is not supported by the capture display", {}, {}};
    }
    if (before.snapshot->values.current != requested) {
      auto applied = state.backend->write(*before.snapshot, requested);
      if (applied.status != 200) {
        applied.snapshot.reset();
        applied.token.clear();
        return applied;
      }
    }
    auto after = read(state);
    if (after.status != 200) {
      return after;
    }
    if (after.snapshot->identity != before.snapshot->identity || after.snapshot->values.current != requested) {
      return {409, "System scaling readback did not confirm the requested capture display and scale", {}, {}};
    }
    return after;
  }

#ifdef SUNSHINE_TESTS
  void set_display_scale_backend_for_test(std::unique_ptr<display_scale_backend> backend) {
    auto &state = service();
    std::lock_guard lock {state.mutex};
    state.backend = std::move(backend);
    state.identity.clear();
    state.token.clear();
  }
#endif
}  // namespace office

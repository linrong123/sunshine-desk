/**
 * @file office/policy.h
 * @brief Office application and cursor request policy.
 */
#pragma once

#include <atomic>
#include <optional>
#include <string_view>

namespace office {
  constexpr std::string_view desktop_apps = R"({"env":{},"apps":[{"name":"Desktop","image-path":"desktop.png"}]})";  ///< Runtime desktop-only catalog; persisted user configuration is untouched.

  /**
   * @brief Whether application parsing and listing use `desktop_apps` instead of the configured file.
   *
   * The service entry point enables this before the first application refresh. It stays disabled in
   * unit tests so upstream tests keep exercising the configured applications file unchanged.
   */
  inline std::atomic_bool desktop_catalog_only {false};

  /**
   * @brief Default LAN and WAN video encryption mode: mandatory (`config::ENCRYPTION_MODE_MANDATORY`).
   *
   * Applies only when `lan_encryption_mode` or `wan_encryption_mode` is absent from the configuration;
   * explicit values are honored unchanged.
   */
  inline constexpr int default_encryption_mode = 2;

  /**
   * @brief Control-stream ENet peer timeout limit, matching Moonlight's `enet_peer_timeout(peer, 2, 10000, 10000)`.
   *
   * With ENet defaults (limit 32, minimum 5 s) the host dropped the peer after about 5 s without
   * acknowledgements while the client keeps it for 10 s, so 5-10 s blackouts on lossy long-haul links
   * ended sessions from the host side (office/CONNECTION.md).
   */
  inline constexpr unsigned control_timeout_limit = 2;
  inline constexpr unsigned control_timeout_ms = 10000;  ///< Control-stream ENet minimum and maximum peer timeout.

  /**
   * @brief Validate the explicit visibility parameter without changing cursor state.
   * @tparam Query Associative query container preserving duplicate keys.
   * @param args Parsed query parameters, including duplicate keys.
   * @return Requested visibility, or no value for missing, duplicate, or invalid input.
   */
  template<class Query>
  std::optional<bool> cursor_visibility(const Query &args) {
    if (args.count("visible") != 1) {
      return std::nullopt;
    }
    const auto &value = args.find("visible")->second;
    if (value == "0") {
      return false;
    }
    if (value == "1") {
      return true;
    }
    return std::nullopt;
  }
}  // namespace office

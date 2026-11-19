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

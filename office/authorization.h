/**
 * @file office/authorization.h
 * @brief Direct-connection authorization: per-connection host approval and paired-client management.
 *
 * This module glues @ref office::connect_confirm_t to configuration, the GameStream
 * launch/resume handlers, stream lifetime, and paired-client changes made from the tray.
 * The client protocol is specified in `office/CONNECTION.md`.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <boost/property_tree/ptree_fwd.hpp>

// local includes
#include "office/connect_confirm.h"
#include "src/config.h"

namespace office {
  inline constexpr std::string_view confirm_connections_key = "desk_confirm_connections";  ///< Configuration key, `enabled` or `disabled`.
  inline constexpr std::string_view config_temporary_suffix = ".office-tmp";  ///< Start of the temporary sibling written first, then renamed over the configuration file.
  inline constexpr int connect_timeout_seconds = 60;  ///< `timeoutSeconds` reported to clients; equals the pending lifetime.

  /**
   * @brief Whether the configuration asks for host approval of every connection.
   */
  inline std::atomic_bool confirm_connections {false};

  /**
   * @brief Whether the host can currently show approval prompts.
   *
   * Set while the Windows prompt thread of @ref authorization_ui_t runs; always `false` on other
   * platforms, so approval is never required where nobody could answer. Tests set it directly.
   */
  inline std::atomic_bool prompt_available {false};

  /**
   * @brief Verified identity of the client behind a GameStream HTTPS request.
   */
  struct client_identity_t {
    std::string cert;  ///< Client certificate PEM accepted by TLS verification.
    std::string name;  ///< Paired device name.
    std::string address;  ///< Normalized source address.
  };

  /**
   * @brief Access the process-wide connection request tracker.
   * @return Tracker shared by the HTTPS handlers, stream hooks, and host prompts.
   */
  connect_confirm_t &connect_tracker();

  /**
   * @brief Whether connections currently require host approval.
   * @return `true` when approval is configured and prompts are available.
   */
  bool confirm_effective();

  /**
   * @brief Parse a boolean Office configuration value.
   * @param value Raw value: `enabled`, `true`, `on`, `yes`, `1` or `disabled`, `false`, `off`, `no`, `0`.
   * @return Parsed value, or `std::nullopt` when the value is not recognized.
   */
  std::optional<bool> parse_config_switch(std::string_view value);

  /**
   * @brief Parse and consume Office keys (authorization, clipboard, and adaptive FEC) from a configuration map.
   * @param vars Parsed configuration entries; consumed keys are erased.
   */
  void apply_config(config::config_map_t &vars);

  /**
   * @brief Turn per-connection approval on or off at runtime.
   *
   * Turning it off drops every request, permit, and grace period.
   *
   * @param enabled New setting.
   * @param persist `true` to rewrite `desk_confirm_connections` in the configuration file.
   * @return `false` if persisting was requested and the configuration file could not be written.
   */
  bool set_confirm_connections(bool enabled, bool persist);

  /**
   * @brief Set or remove one key in the configuration file, keeping every other key.
   *
   * The file is rewritten through a temporary sibling and a rename, so an interrupted write never
   * truncates it. Keys are written sorted as `key = value`; comments are not preserved. Only the file
   * changes; the running configuration is not reloaded. The whole read-modify-write holds a
   * process-wide lock shared with @ref read_config_value and @ref set_confirm_connections, so
   * concurrent writers from the tray and the main window never lose each other's updates. The web
   * UI's `saveConfig` does not take this lock.
   *
   * @param key Configuration key.
   * @param value New value, or `std::nullopt` to remove the key and fall back to the built-in default.
   * @return `true` if the file was written.
   */
  bool persist_config_value(std::string_view key, const std::optional<std::string> &value);

  /**
   * @brief Read one key from the configuration file.
   * @param key Configuration key.
   * @return The value in the file, or `std::nullopt` if the key is absent.
   */
  std::optional<std::string> read_config_value(std::string_view key);

  /**
   * @brief Temporary sibling used by one configuration write.
   *
   * The name is unique per process (a random token chosen at startup) and per write (@p sequence),
   * so neither a concurrent write nor another process reuses it.
   *
   * @param config_file Configuration file.
   * @param sequence Write sequence number.
   * @return `<config_file>.office-tmp.<token>-<sequence>`.
   */
  std::filesystem::path config_temporary_path(const std::filesystem::path &config_file, std::uint64_t sequence);

  /**
   * @brief Sequence number the next configuration write will use, without consuming it.
   *
   * Lets tests predict the temporary file of the next write.
   *
   * @return Next value passed to @ref config_temporary_path.
   */
  std::uint64_t next_config_temporary_sequence();

  /**
   * @brief Handle `/office/connect-request` for an already verified client.
   * @param actions Every `action` query value of the request.
   * @param client Verified client identity; an empty certificate (connection unknown or client disabled) gets `status_code=401`.
   * @return GameStream XML response tree.
   */
  boost::property_tree::ptree connect_request(const std::vector<std::string> &actions, const client_identity_t &client);

  /**
   * @brief Reject a launch or resume that lacks host approval.
   *
   * When approval is required and neither a permit nor a reconnection grace exists, the request
   * starts a connection request for the host user and the response becomes
   * `status_code=403`, `status_message="Host approval required"`. Otherwise one permit or
   * grace is consumed and the stream may start.
   *
   * @param tree Response tree populated on rejection.
   * @param result_field Field set to 0 on rejection, such as `root.gamesession` or `root.resume`.
   * @param client Verified client identity; with approval required, an empty certificate is rejected without a request.
   * @return `true` if the request was rejected.
   */
  bool reject_unapproved_stream(boost::property_tree::ptree &tree, std::string_view result_field, const client_identity_t &client);

  /**
   * @brief Advertise `OfficeConnectConfirm` in `serverinfo`.
   * @param tree Response tree that already contains the upstream `root` fields.
   */
  void add_connect_capability(boost::property_tree::ptree &tree);

  /**
   * @brief Keep an admitted client admitted for as long as its stream runs.
   * @param cert Client certificate PEM of the started stream; empty for unpaired test sessions.
   */
  void stream_started(const std::string &cert);

  /**
   * @brief Start the reconnection grace of a stream that stopped.
   * @param cert Client certificate PEM of the stopped stream; empty for unpaired test sessions.
   */
  void stream_stopped(const std::string &cert);

  /**
   * @brief Cancel the reconnection grace of a client that quit explicitly.
   * @param cert Verified client certificate PEM.
   */
  void client_quit(const std::string &cert);

  /**
   * @brief Certificates of the clients with a running stream, as reported by the stream hooks.
   * @return One entry per client with at least one running stream, sorted; an empty string stands for
   *         streams of unpaired test sessions.
   */
  std::vector<std::string> streaming_certs();

  /**
   * @brief Enable or disable a paired client from the host.
   *
   * Disabling stops the client's streams, terminates the running app when no stream is left, and
   * drops the client's requests, permits, and grace period.
   *
   * @param uuid Paired client identifier.
   * @param enabled New state.
   * @return `true` if the client exists and was updated.
   */
  bool set_client_access(std::string_view uuid, bool enabled);

  /**
   * @brief Delete a paired client from the host, with the same cleanup as disabling it.
   * @param uuid Paired client identifier.
   * @return `true` if the client existed and was removed.
   */
  bool delete_client(std::string_view uuid);
}  // namespace office

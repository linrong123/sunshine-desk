/**
 * @file office/main_window.h
 * @brief Sunshine Desk main window: platform-independent view model and the request to open it.
 *
 * The Windows window itself lives in `office/windows/main_window.cpp` and runs on the prompt
 * thread of @ref office::authorization_ui_t. This header keeps the logic that decides what the
 * window shows (connectable addresses, device rows, name validation, restart rules) free of
 * Win32 so it can be tested anywhere. Behaviour and limits are described in `office/MAIN_WINDOW.md`.
 */
#pragma once

// standard includes
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace office {
  inline constexpr int default_http_port = 47989;  ///< GameStream HTTP port that clients assume when none is given.
  inline constexpr std::string_view device_name_key = "sunshine_name";  ///< Configuration key of the advertised host name.
  inline constexpr std::size_t device_name_max_length = 64;  ///< Longest accepted device name, in Unicode code points.

  /**
   * @brief One unicast address of one network adapter, as reported by the operating system.
   */
  struct adapter_address_t {
    std::string adapter;  ///< Adapter display name.
    std::string address;  ///< Address text without port, zone, or brackets.
    bool up {true};  ///< Whether the adapter is operational.
    bool tunnel {false};  ///< Whether the adapter is a tunnel or software loopback interface.
    bool preferred {true};  ///< Whether the address is usable (not tentative, deprecated, or duplicate).
  };

  /**
   * @brief Address the main window offers for clients to connect to.
   */
  struct host_address_t {
    std::string adapter;  ///< Adapter display name.
    std::string address;  ///< Address text.
    bool ipv6 {false};  ///< Whether the address is IPv6.
    std::string connect_text;  ///< What to type into the client, including the port when it is not the default.
  };

  /**
   * @brief Format an address the way Moonlight's "add host" field expects it.
   * @param address IPv4 or IPv6 address text.
   * @param ipv6 Whether the address is IPv6.
   * @param http_port GameStream HTTP port of this host.
   * @return The bare address for the default port, otherwise `a.b.c.d:port` or `[v6]:port`.
   */
  std::string format_connect_address(std::string_view address, bool ipv6, int http_port);

  /**
   * @brief Pick the addresses a client on the network can reach.
   *
   * Drops adapters that are down, tunnels, loopback, link-local (IPv4 169.254/16, IPv6 fe80::/10),
   * unspecified, multicast, and addresses that are not preferred (tentative, deprecated, duplicate).
   * Temporary IPv6 privacy addresses cannot be told apart reliably and stay in the list. IPv4 comes
   * before IPv6; within a family the adapter order is kept. Duplicates are removed.
   *
   * @param addresses Addresses of every adapter.
   * @param http_port GameStream HTTP port of this host.
   * @return Connectable addresses in display order.
   */
  std::vector<host_address_t> connectable_addresses(const std::vector<adapter_address_t> &addresses, int http_port);

  /**
   * @brief Explain which port clients use and which ports the firewall must allow.
   * @param http_port GameStream HTTP port of this host (the base port).
   * @return Hint text in Chinese.
   */
  std::string port_hint(int http_port);

  /**
   * @brief Name shown for a paired device.
   * @param name Name stored at pairing time.
   * @param index Zero-based position in the device list.
   * @return The stored name, or "设备 N" when it is empty or "-".
   */
  std::string device_display_name(std::string_view name, std::size_t index);

  /**
   * @brief One row of the paired-device list.
   */
  struct device_row_t {
    std::string uuid;  ///< Paired client identifier.
    std::string name;  ///< Display name.
    bool enabled {true};  ///< Whether the client may connect.
    bool streaming {false};  ///< Whether the client has a running stream.
    std::string label;  ///< Text of the list row.
  };

  /**
   * @brief Build the device list from @ref nvhttp::get_all_clients.
   * @param clients JSON array of `{name, uuid, enabled}` objects.
   * @param streaming_uuids Identifiers of clients with a running stream.
   * @return Rows in the stored order; entries without a uuid are skipped.
   */
  std::vector<device_row_t> device_rows(const nlohmann::json &clients, const std::set<std::string, std::less<>> &streaming_uuids);

  /**
   * @brief Text of the service status line.
   * @param rows Current device rows.
   * @param other_streams Running streams that belong to no paired device in @p rows.
   * @return Status text in Chinese.
   */
  std::string service_status(const std::vector<device_row_t> &rows, std::size_t other_streams);

  /**
   * @brief Validate and normalize a device name typed by the host user.
   *
   * Surrounding spaces are trimmed. Control characters, `#`, a leading `[`, and names longer than
   * @ref device_name_max_length code points are rejected because the configuration file format
   * cannot carry them safely. Overlong names are rejected, never truncated, so a multi-byte
   * character is never cut. The limit counts code points, not UTF-8 bytes: upstream only reports
   * `sunshine_name` in serverinfo, while the mDNS instance name comes from the computer name (see
   * office/MAIN_WINDOW.md).
   *
   * @param input Name typed by the user, UTF-8.
   * @return The normalized name; an empty string means "use the computer name"; `std::nullopt` if invalid.
   */
  std::optional<std::string> normalize_device_name(std::string_view input);

  /**
   * @brief Whether a configuration key only takes effect after the service restarts.
   * @param key Configuration key.
   * @return `true` for keys read once at startup, such as `sunshine_name`.
   */
  bool restart_required(std::string_view key);

  /**
   * @brief Whether a saved device name differs from the one the running service advertises.
   * @param saved Value of `sunshine_name` in the configuration file, if any.
   * @param running Name the running service advertises.
   * @param host_name Computer name used when the key is absent or empty.
   * @return `true` if a restart would change the advertised name.
   */
  bool device_name_pending(const std::optional<std::string> &saved, std::string_view running, std::string_view host_name);

  /**
   * @brief Where a click on the pairing notification leads.
   */
  enum class pin_target_e {
    main_window,  ///< The native main window; pairing prompts appear on their own.
    web_ui,  ///< The upstream web PIN page.
  };

  /**
   * @brief Choose the target of the pairing notification.
   * @param native_available Whether the native prompt thread runs.
   * @return @ref pin_target_e::main_window when native prompts are available, otherwise @ref pin_target_e::web_ui.
   */
  pin_target_e pin_notification_target(bool native_available);

  /**
   * @brief Ask the prompt thread to open the main window, or bring it to the front.
   *
   * Safe to call from any thread; it only posts a message. Always `false` on platforms without the
   * native window.
   *
   * @return `true` if the request was delivered to a running prompt thread.
   */
  bool open_main_window();

  /**
   * @brief Open the main window, falling back to the web UI when it is unavailable.
   */
  void open_desk_ui();

  /**
   * @brief Handle a click on the pairing notification.
   *
   * Opens the main window when native prompts are available (the pairing prompt itself is shown
   * by the prompt thread), otherwise the upstream web PIN page.
   */
  void open_pin_ui();
}  // namespace office

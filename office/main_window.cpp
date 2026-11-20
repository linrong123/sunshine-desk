/**
 * @file office/main_window.cpp
 * @brief Sunshine Desk main window: platform-independent view model and the request to open it.
 */
// standard includes
#include <algorithm>
#include <format>
#include <string>
#include <utility>

// lib includes
#include <boost/asio/ip/address.hpp>

// local includes
#include "office/authorization.h"
#include "office/main_window.h"
#include "src/entry_handler.h"

using namespace std::literals;

namespace office {
  namespace {
    /**
     * @brief Whether a parsed address is reachable from other machines on the network.
     * @param address Parsed address.
     * @return `false` for loopback, unspecified, multicast, and link-local addresses.
     */
    bool reachable(const boost::asio::ip::address &address) {
      if (address.is_loopback() || address.is_unspecified() || address.is_multicast()) {
        return false;
      }
      if (address.is_v4()) {
        const auto bytes = address.to_v4().to_bytes();
        return !(bytes[0] == 169 && bytes[1] == 254);
      }
      const auto v6 = address.to_v6();
      return !v6.is_link_local() && !v6.is_v4_mapped();
    }

    /**
     * @brief Count the Unicode code points of a UTF-8 string.
     * @param text UTF-8 text.
     * @return Number of non-continuation bytes.
     */
    std::size_t code_points(const std::string_view text) {
      return static_cast<std::size_t>(std::ranges::count_if(text, [](const char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
      }));
    }

    /**
     * @brief Trim spaces and tabs from both ends.
     * @param text Text to trim.
     * @return Trimmed view.
     */
    std::string_view trim(std::string_view text) {
      constexpr auto blanks = " \t"sv;
      const auto first = text.find_first_not_of(blanks);
      if (first == std::string_view::npos) {
        return {};
      }
      const auto last = text.find_last_not_of(blanks);
      return text.substr(first, last - first + 1);
    }
  }  // namespace

  std::string format_connect_address(const std::string_view address, const bool ipv6, const int http_port) {
    if (http_port == default_http_port) {
      return std::string {address};
    }
    return ipv6 ? std::format("[{}]:{}", address, http_port) : std::format("{}:{}", address, http_port);
  }

  std::vector<host_address_t> connectable_addresses(const std::vector<adapter_address_t> &addresses, const int http_port) {
    std::vector<host_address_t> v4;
    std::vector<host_address_t> v6;
    std::set<std::string, std::less<>> seen;
    for (const auto &entry : addresses) {
      if (!entry.up || entry.tunnel || !entry.preferred) {
        continue;
      }
      const auto text = std::string_view {entry.address}.substr(0, entry.address.find('%'));
      boost::system::error_code error;
      const auto parsed = boost::asio::ip::make_address(std::string {text}, error);
      if (error || !reachable(parsed)) {
        continue;
      }
      auto canonical = parsed.to_string();
      if (!seen.insert(canonical).second) {
        continue;
      }
      const auto ipv6 = parsed.is_v6();
      auto connect_text = format_connect_address(canonical, ipv6, http_port);
      (ipv6 ? v6 : v4).push_back({entry.adapter, std::move(canonical), ipv6, std::move(connect_text)});
    }
    v4.insert(v4.end(), std::make_move_iterator(v6.begin()), std::make_move_iterator(v6.end()));
    return v4;
  }

  std::string port_hint(const int http_port) {
    // Offsets follow nvhttp (HTTPS -5, HTTP 0), rtsp (RTSP setup +21), and stream (video +9 .. audio +11).
    const auto ports = std::format("防火墙需放行 TCP {}、{}、{} 和 UDP {}-{}。", http_port - 5, http_port, http_port + 21, http_port + 9, http_port + 11);
    if (http_port == default_http_port) {
      return std::format("在 Moonlight Desk 中输入上面任一地址即可，默认端口 {} 无需填写。{}", http_port, ports);
    }
    return std::format("本机使用非默认端口 {}，请按上面的格式连同端口输入。{}", http_port, ports);
  }

  std::string device_display_name(const std::string_view name, const std::size_t index) {
    if (name.empty() || name == "-"sv) {
      return std::format("设备 {}", index + 1);
    }
    return std::string {name};
  }

  std::vector<device_row_t> device_rows(const nlohmann::json &clients, const std::set<std::string, std::less<>> &streaming_uuids) {
    std::vector<device_row_t> rows;
    if (!clients.is_array()) {
      return rows;
    }
    for (const auto &client : clients) {
      if (!client.is_object()) {
        continue;
      }
      auto uuid = client.value("uuid", ""s);
      if (uuid.empty()) {
        continue;
      }
      device_row_t row;
      row.name = device_display_name(client.value("name", ""s), rows.size());
      row.enabled = client.value("enabled", true);
      row.streaming = streaming_uuids.contains(uuid);
      row.uuid = std::move(uuid);
      row.label = row.name;
      if (!row.enabled) {
        row.label += "（已禁用）";
      }
      if (row.streaming) {
        row.label += " · 串流中";
      }
      rows.push_back(std::move(row));
    }
    return rows;
  }

  std::string service_status(const std::vector<device_row_t> &rows, const std::size_t other_streams) {
    std::string names;
    for (const auto &row : rows) {
      if (row.streaming) {
        names += names.empty() ? row.name : "、" + row.name;
      }
    }
    if (other_streams > 0) {
      const auto other = std::format("其他会话 {} 个", other_streams);
      names += names.empty() ? other : "、" + other;
    }
    if (names.empty()) {
      return "服务运行中 · 当前无串流";
    }
    return std::format("服务运行中 · 正在串流：{}", names);
  }

  std::optional<std::string> normalize_device_name(const std::string_view input) {
    const auto name = trim(input);
    if (code_points(name) > device_name_max_length || name.starts_with('[') || name.find('#') != std::string_view::npos) {
      return std::nullopt;
    }
    if (std::ranges::any_of(name, [](const char c) {
          const auto byte = static_cast<unsigned char>(c);
          return byte < 0x20 || byte == 0x7F;
        })) {
      return std::nullopt;
    }
    return std::string {name};
  }

  bool restart_required(const std::string_view key) {
    // desk_confirm_connections is applied at runtime; the advertised name is read once at startup.
    return key == device_name_key;
  }

  bool device_name_pending(const std::optional<std::string> &saved, const std::string_view running, const std::string_view host_name) {
    const auto effective = saved && !saved->empty() ? std::string_view {*saved} : host_name;
    return effective != running;
  }

  pin_target_e pin_notification_target(const bool native_available) {
    return native_available ? pin_target_e::main_window : pin_target_e::web_ui;
  }

  void open_desk_ui() {
    if (!open_main_window()) {
      launch_ui();
    }
  }

  void open_pin_ui() {
    if (pin_notification_target(prompt_available.load()) == pin_target_e::main_window && open_main_window()) {
      return;
    }
    launch_ui("/pin");
  }

#ifndef _WIN32
  bool open_main_window() {
    return false;
  }
#endif
}  // namespace office

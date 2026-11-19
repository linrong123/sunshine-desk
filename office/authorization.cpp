/**
 * @file office/authorization.cpp
 * @brief Direct-connection authorization: per-connection host approval and paired-client management.
 */
// standard includes
#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "office/authorization.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/process.h"
#include "src/rtsp.h"

using namespace std::literals;

namespace office {
  namespace {
    namespace pt = boost::property_tree;

    /**
     * @brief Parse a boolean configuration value.
     * @param value Raw value.
     * @param result Parsed value; unchanged when the value is not recognized.
     * @return `true` if the value was recognized.
     */
    bool parse_switch(const std::string_view value, bool &result) {
      constexpr std::array on {"enabled"sv, "true"sv, "on"sv, "yes"sv, "1"sv};
      constexpr std::array off {"disabled"sv, "false"sv, "off"sv, "no"sv, "0"sv};
      if (std::ranges::find(on, value) != on.end()) {
        result = true;
        return true;
      }
      if (std::ranges::find(off, value) != off.end()) {
        result = false;
        return true;
      }
      return false;
    }

    /**
     * @brief Rewrite the configuration file with `desk_confirm_connections` set.
     * @param enabled Value to write.
     * @return `true` if the file was written.
     */
    bool persist_confirm_connections(const bool enabled) {
      const auto &path = config::sunshine.config_file;
      auto vars = config::parse_config(file_handler::read_file(path.c_str()));
      vars.insert_or_assign(std::string {confirm_connections_key}, enabled ? "enabled"s : "disabled"s);

      std::vector<std::pair<std::string, std::string>> sorted {vars.begin(), vars.end()};
      std::ranges::sort(sorted);
      std::string content;
      for (const auto &[key, value] : sorted) {
        content += std::format("{} = {}\n", key, value);
      }
      if (file_handler::write_file(path.c_str(), content) != 0) {
        BOOST_LOG(warning) << "Failed to persist config option '"sv << confirm_connections_key << "'"sv;
        return false;
      }
      return true;
    }

    /**
     * @brief Stop a client's streams and the app once no stream is left.
     * @param cert Client certificate PEM; may be empty if the client was not found.
     */
    void stop_client_streams(const std::string &cert) {
      if (!cert.empty()) {
        rtsp_stream::terminate_sessions_by_cert(cert);
        connect_tracker().revoke(cert);
      }
      if (rtsp_stream::session_count() == 0 && proc::proc.running() > 0) {
        proc::proc.terminate();
      }
    }
  }  // namespace

  connect_confirm_t &connect_tracker() {
    static connect_confirm_t tracker;
    return tracker;
  }

  bool confirm_effective() {
    return confirm_connections.load() && prompt_available.load();
  }

  void apply_config(config::config_map_t &vars) {
    bool enabled = false;
    if (const auto it = vars.find(confirm_connections_key); it != vars.end()) {
      if (!parse_switch(it->second, enabled)) {
        BOOST_LOG(warning) << "Invalid value for "sv << confirm_connections_key << ": "sv << it->second << "; using disabled"sv;
      }
      vars.erase(it);
    }
    confirm_connections.store(enabled);
    if (!enabled) {
      connect_tracker().clear();
    }
  }

  bool set_confirm_connections(const bool enabled, const bool persist) {
    confirm_connections.store(enabled);
    if (!enabled) {
      connect_tracker().clear();
    }
    BOOST_LOG(info) << "Per-connection host approval "sv << (enabled ? "enabled"sv : "disabled"sv);
    return !persist || persist_confirm_connections(enabled);
  }

  pt::ptree connect_request(const std::vector<std::string> &actions, const client_identity_t &client) {
    pt::ptree tree;
    if (actions.size() != 1 || (actions.front() != "start" && actions.front() != "status" && actions.front() != "cancel")) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Expected exactly one action=start, action=status, or action=cancel parameter");
      return tree;
    }

    const auto &action = actions.front();
    auto state = connect_state_e::not_required;
    auto status_code = 200;
    if (confirm_effective()) {
      auto &tracker = connect_tracker();
      if (action == "start") {
        const auto result = tracker.start(client.cert, client.name, client.address);
        state = result.state;
        if (result.limited) {
          status_code = 503;
          tree.put("root.<xmlattr>.status_message", "Too many pending connection requests");
        } else if (state == connect_state_e::pending) {
          BOOST_LOG(info) << "Connection request from '"sv << client.name << "' at "sv << client.address << " is waiting for host approval"sv;
        }
      } else if (action == "status") {
        state = tracker.status(client.cert);
      } else {
        state = tracker.cancel(client.cert);
      }
    }

    tree.put("root.<xmlattr>.status_code", status_code);
    tree.put("root.state", std::string {to_string(state)});
    tree.put("root.timeoutSeconds", connect_timeout_seconds);
    return tree;
  }

  bool reject_unapproved_stream(pt::ptree &tree, const std::string_view result_field, const client_identity_t &client) {
    if (!confirm_effective()) {
      return false;
    }
    auto &tracker = connect_tracker();
    if (tracker.admit(client.cert)) {
      return false;
    }

    tracker.start(client.cert, client.name, client.address);
    BOOST_LOG(info) << "Stream request from '"sv << client.name << "' at "sv << client.address << " requires host approval"sv;
    tree.put(std::string {result_field}, 0);
    tree.put("root.<xmlattr>.status_code", 403);
    tree.put("root.<xmlattr>.status_message", "Host approval required");
    return true;
  }

  void add_connect_capability(pt::ptree &tree) {
    tree.put("root.OfficeConnectConfirm", confirm_effective() ? 1 : 0);
  }

  void stream_started(const std::string &cert) {
    if (!cert.empty()) {
      connect_tracker().stream_started(cert);
    }
  }

  void stream_stopped(const std::string &cert) {
    if (!cert.empty()) {
      connect_tracker().stream_stopped(cert);
    }
  }

  void client_quit(const std::string &cert) {
    if (!cert.empty()) {
      connect_tracker().quit(cert);
    }
  }

  bool set_client_access(const std::string_view uuid, const bool enabled) {
    if (!nvhttp::set_client_enabled(uuid, enabled)) {
      return false;
    }
    BOOST_LOG(info) << "Paired client "sv << uuid << (enabled ? " enabled"sv : " disabled"sv) << " from the host"sv;
    if (!enabled) {
      stop_client_streams(nvhttp::get_cert_by_uuid(uuid));
    }
    return true;
  }

  bool delete_client(const std::string_view uuid) {
    auto cert = nvhttp::get_cert_by_uuid(uuid);
    if (!nvhttp::unpair_client(uuid)) {
      return false;
    }
    BOOST_LOG(info) << "Paired client "sv << uuid << " deleted from the host"sv;
    stop_client_streams(cert);
    return true;
  }
}  // namespace office

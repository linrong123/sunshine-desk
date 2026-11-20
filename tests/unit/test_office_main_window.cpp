/**
 * @file tests/unit/test_office_main_window.cpp
 * @brief Tests for the platform-independent logic of the Sunshine Desk main window.
 */
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "office/authorization.h"
#include "office/main_window.h"
#include "src/config.h"
#include "src/file_handler.h"

using namespace std::literals;

namespace {
  using office::adapter_address_t;

  /**
   * @brief Connect texts of the addresses the main window would show.
   * @param addresses Adapter addresses to filter.
   * @param http_port GameStream HTTP port.
   * @return One connect text per shown address, in display order.
   */
  std::vector<std::string> shown(const std::vector<adapter_address_t> &addresses, const int http_port = office::default_http_port) {
    std::vector<std::string> texts;
    for (const auto &entry : office::connectable_addresses(addresses, http_port)) {
      texts.push_back(entry.connect_text);
    }
    return texts;
  }

  TEST(OfficeMainWindowTest, FormatsAddressesTheWayMoonlightExpects) {
    EXPECT_EQ(office::format_connect_address("192.168.1.5", false, office::default_http_port), "192.168.1.5");
    EXPECT_EQ(office::format_connect_address("2001:db8::1", true, office::default_http_port), "2001:db8::1");
    EXPECT_EQ(office::format_connect_address("192.168.1.5", false, 48000), "192.168.1.5:48000");
    EXPECT_EQ(office::format_connect_address("2001:db8::1", true, 48000), "[2001:db8::1]:48000");
  }

  TEST(OfficeMainWindowTest, DropsUnreachableAddresses) {
    const std::vector<adapter_address_t> addresses {
      {"Loopback", "127.0.0.1"},
      {"Loopback", "::1"},
      {"Down", "192.168.2.7", false},
      {"VPN", "10.8.0.2", true, true},
      {"Tentative", "192.168.3.9", true, false, false},
      {"APIPA", "169.254.10.20"},
      {"Link-local", "fe80::1%12"},
      {"Unspecified", "0.0.0.0"},
      {"Mapped", "::ffff:192.0.2.1"},
      {"Broken", "not-an-address"},
      {"Ethernet", "192.168.1.5"},
    };
    EXPECT_EQ(shown(addresses), std::vector<std::string> {"192.168.1.5"});
  }

  TEST(OfficeMainWindowTest, OrdersIpv4FirstStripsZonesAndDeduplicates) {
    const std::vector<adapter_address_t> addresses {
      {"Wi-Fi", "2001:DB8::1"},
      {"Ethernet", "192.168.1.5"},
      {"Wi-Fi", "192.168.1.6"},
      {"Hyper-V", "192.168.1.5"},
      {"Wi-Fi", "2001:db8::2%7"},
      {"Ethernet", "2001:db8::1"},
    };
    EXPECT_EQ(shown(addresses), (std::vector<std::string> {"192.168.1.5", "192.168.1.6", "2001:db8::1", "2001:db8::2"}));
    EXPECT_EQ(shown(addresses, 48000), (std::vector<std::string> {"192.168.1.5:48000", "192.168.1.6:48000", "[2001:db8::1]:48000", "[2001:db8::2]:48000"}));

    const auto result = office::connectable_addresses(addresses, office::default_http_port);
    ASSERT_EQ(result.size(), 4U);
    EXPECT_EQ(result.front().adapter, "Ethernet");
    EXPECT_FALSE(result.front().ipv6);
    EXPECT_EQ(result[2].address, "2001:db8::1");
    EXPECT_TRUE(result[2].ipv6);
  }

  TEST(OfficeMainWindowTest, PortHintListsFirewallPorts) {
    const auto standard = office::port_hint(office::default_http_port);
    for (const auto *part : {"47984", "47989", "48010", "47998-48000", "无需填写"}) {
      EXPECT_NE(standard.find(part), std::string::npos) << part;
    }
    const auto custom = office::port_hint(50000);
    for (const auto *part : {"非默认端口 50000", "49995", "50021", "50009-50011"}) {
      EXPECT_NE(custom.find(part), std::string::npos) << part;
    }
  }

  TEST(OfficeMainWindowTest, DeviceRowsLabelStateAndFallbackNames) {
    const auto clients = nlohmann::json::parse(R"([
      {"name": "Phone", "uuid": "u1", "enabled": true},
      {"name": "", "uuid": "u2", "enabled": false},
      {"name": "Lost", "uuid": ""},
      "not an object",
      {"name": "-", "uuid": "u3"}
    ])");
    const auto rows = office::device_rows(clients, {"u3"});
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].uuid, "u1");
    EXPECT_EQ(rows[0].label, "Phone");
    EXPECT_TRUE(rows[0].enabled);
    EXPECT_EQ(rows[1].name, "设备 2");
    EXPECT_EQ(rows[1].label, "设备 2（已禁用）");
    EXPECT_FALSE(rows[1].enabled);
    EXPECT_EQ(rows[2].name, "设备 3");
    EXPECT_TRUE(rows[2].enabled);
    EXPECT_TRUE(rows[2].streaming);
    EXPECT_EQ(rows[2].label, "设备 3 · 串流中");

    EXPECT_TRUE(office::device_rows(nlohmann::json::object(), {}).empty());
    EXPECT_EQ(office::device_display_name("Laptop", 4), "Laptop");
    EXPECT_EQ(office::device_display_name("", 4), "设备 5");
  }

  TEST(OfficeMainWindowTest, ServiceStatusNamesStreamingDevices) {
    auto rows = office::device_rows(nlohmann::json::parse(R"([{"name": "Phone", "uuid": "u1"}, {"name": "Tablet", "uuid": "u2"}])"), {});
    EXPECT_EQ(office::service_status(rows, 0), "服务运行中 · 当前无串流");
    EXPECT_EQ(office::service_status(rows, 2), "服务运行中 · 正在串流：其他会话 2 个");
    rows = office::device_rows(nlohmann::json::parse(R"([{"name": "Phone", "uuid": "u1"}, {"name": "Tablet", "uuid": "u2"}])"), {"u1", "u2"});
    EXPECT_EQ(office::service_status(rows, 1), "服务运行中 · 正在串流：Phone、Tablet、其他会话 1 个");
  }

  TEST(OfficeMainWindowTest, NormalizesAndRejectsDeviceNames) {
    EXPECT_EQ(office::normalize_device_name("  Office PC\t"), "Office PC");
    EXPECT_EQ(office::normalize_device_name("   "), "");
    EXPECT_EQ(office::normalize_device_name("会议室]"), "会议室]");
    EXPECT_EQ(office::normalize_device_name("a#b"), std::nullopt);
    EXPECT_EQ(office::normalize_device_name(" [desk"), std::nullopt);
    EXPECT_EQ(office::normalize_device_name("a\tb"), std::nullopt);
    EXPECT_EQ(office::normalize_device_name("a\nb"), std::nullopt);
    EXPECT_EQ(office::normalize_device_name("a\x7f"), std::nullopt);

    std::string longest;
    for (std::size_t i = 0; i < office::device_name_max_length; ++i) {
      longest += "办";
    }
    EXPECT_EQ(office::normalize_device_name(longest), longest);
    EXPECT_EQ(office::normalize_device_name(longest + "a"), std::nullopt);
  }

  TEST(OfficeMainWindowTest, RestartRulesForDeviceName) {
    EXPECT_TRUE(office::restart_required(office::device_name_key));
    EXPECT_FALSE(office::restart_required(office::confirm_connections_key));

    EXPECT_FALSE(office::device_name_pending(std::nullopt, "HOST", "HOST"));
    EXPECT_FALSE(office::device_name_pending(""s, "HOST", "HOST"));
    EXPECT_TRUE(office::device_name_pending("Desk"s, "HOST", "HOST"));
    EXPECT_FALSE(office::device_name_pending("Desk"s, "Desk", "HOST"));
    EXPECT_TRUE(office::device_name_pending(std::nullopt, "Desk", "HOST"));
  }

  TEST(OfficeMainWindowTest, PinNotificationPrefersNativeWindow) {
    EXPECT_EQ(office::pin_notification_target(true), office::pin_target_e::main_window);
    EXPECT_EQ(office::pin_notification_target(false), office::pin_target_e::web_ui);
  }

  TEST(OfficeMainWindowTest, StreamingCertsFollowStreamHooks) {
    const auto contains = [](const std::string &cert) {
      const auto certs = office::streaming_certs();
      return std::ranges::find(certs, cert) != certs.end();
    };
    const auto cert = "-----TEST MAIN WINDOW CERT-----"s;
    ASSERT_FALSE(contains(cert));
    office::stream_started(cert);
    office::stream_started(cert);
    EXPECT_TRUE(contains(cert));
    office::stream_stopped(cert);
    EXPECT_TRUE(contains(cert));
    office::stream_stopped(cert);
    EXPECT_FALSE(contains(cert));
    office::stream_stopped(cert);
    EXPECT_FALSE(contains(cert));
    office::connect_tracker().clear();
  }

  /**
   * @brief Points the configuration file at a temporary path.
   */
  class OfficeMainWindowConfigTest: public testing::Test {
  protected:
    /**
     * @brief Swap in a temporary configuration file.
     */
    void SetUp() override {
      original_config_file = config::sunshine.config_file;
      config_file = std::filesystem::temp_directory_path() / "sunshine_test_office_main_window.conf";  // NOSONAR(cpp:S5443): safe for tests
      std::filesystem::remove(config_file);
      config::sunshine.config_file = config_file.string();
    }

    /**
     * @brief Remove the temporary file and restore the configuration path.
     */
    void TearDown() override {
      std::filesystem::remove(config_file);
      config::sunshine.config_file = original_config_file;
    }

    /**
     * @brief Whether the temporary sibling of the configuration file still exists.
     * @return `true` if the atomic write left its temporary file behind.
     */
    bool temporary_left() const {
      const auto prefix = config_file.filename().string() + std::string {office::config_temporary_suffix};
      std::error_code error;
      for (const auto &entry : std::filesystem::directory_iterator(config_file.parent_path(), error)) {
        if (entry.path().filename().string().starts_with(prefix)) {
          return true;
        }
      }
      return false;
    }

    std::filesystem::path config_file;  ///< Temporary configuration file.

  private:
    std::string original_config_file;  ///< Restored configuration path.
  };

  TEST_F(OfficeMainWindowConfigTest, PersistSetsAndRemovesKeysAndKeepsOthers) {
    EXPECT_EQ(office::read_config_value(office::device_name_key), std::nullopt);
    ASSERT_EQ(file_handler::write_file(config_file.string().c_str(), "adapter_name = Test\nsunshine_name = Old\n"), 0);
    EXPECT_EQ(office::read_config_value(office::device_name_key), "Old");

    ASSERT_TRUE(office::persist_config_value(office::device_name_key, "会议室电脑"s));
    EXPECT_EQ(office::read_config_value(office::device_name_key), "会议室电脑");
    EXPECT_EQ(office::read_config_value("adapter_name"), "Test");
    EXPECT_FALSE(temporary_left());

    ASSERT_TRUE(office::persist_config_value(office::device_name_key, std::nullopt));
    EXPECT_EQ(office::read_config_value(office::device_name_key), std::nullopt);
    EXPECT_EQ(office::read_config_value("adapter_name"), "Test");
    EXPECT_FALSE(temporary_left());

    const auto vars = config::parse_config(file_handler::read_file(config_file.string().c_str()));
    EXPECT_EQ(vars.size(), 1U);
  }

  TEST_F(OfficeMainWindowConfigTest, AcceptedNamesSurviveWriteAndReadBack) {
    ASSERT_EQ(file_handler::write_file(config_file.string().c_str(), "adapter_name = Test\n"), 0);
    for (const auto &[input, expected] : std::vector<std::pair<std::string, std::string>> {
           {"a=b", "a=b"},
           {"name = value = x", "name = value = x"},
           {"\"Desk\" 'PC'", "\"Desk\" 'PC'"},
           {"会议室电脑", "会议室电脑"},
           {"  前台 PC  ", "前台 PC"},
           {"\tDesk\t", "Desk"},
           {"a]b", "a]b"},
         }) {
      const auto name = office::normalize_device_name(input);
      ASSERT_EQ(name, expected) << input;
      ASSERT_TRUE(office::persist_config_value(office::device_name_key, *name)) << input;
      EXPECT_EQ(office::read_config_value(office::device_name_key), expected) << input;
      EXPECT_EQ(office::read_config_value("adapter_name"), "Test") << input;
      EXPECT_FALSE(temporary_left()) << input;
    }
  }
}  // namespace

/**
 * @file tests/unit/test_office.cpp
 * @brief Office runtime catalog and cursor policy tests.
 */
#include "../tests_common.h"
#include "office/nvhttp_routes.h"
#include "office/policy.h"
#include "src/config.h"
#include "src/process.h"
#include "src/utility.h"

#include <map>
#include <string>

TEST(OfficePolicy, StrictCursorVisibility) {
  using query_t = std::multimap<std::string, std::string>;
  EXPECT_EQ(office::cursor_visibility(query_t {{"visible", "0"}}), false);
  EXPECT_EQ(office::cursor_visibility(query_t {{"visible", "1"}}), true);
  EXPECT_FALSE(office::cursor_visibility(query_t {}).has_value());
  EXPECT_FALSE(office::cursor_visibility(query_t {{"visible", "true"}}).has_value());
  EXPECT_FALSE(office::cursor_visibility(query_t {{"visible", "0"}, {"visible", "0"}}).has_value());
}

TEST(OfficePolicy, ServerinfoAdvertisesOfficeCapabilities) {
  boost::property_tree::ptree tree;
  tree.put("root.hostname", "office");
  office::add_serverinfo_capabilities(tree);
  EXPECT_EQ(tree.get<int>("root.OfficeCursorSet"), 1);
#ifdef _WIN32
  EXPECT_EQ(tree.get<int>("root.OfficeDisplayScale"), 1);
#else
  EXPECT_FALSE(tree.get_optional<int>("root.OfficeDisplayScale").has_value());
#endif
  EXPECT_EQ(tree.get<std::string>("root.hostname"), "office");
}

TEST(OfficePolicy, RuntimeCatalogNeverReadsUserFile) {
  const bool previous = office::desktop_catalog_only.exchange(true);
  auto restore = util::fail_guard([previous]() {
    office::desktop_catalog_only = previous;
  });
  auto catalog = proc::parse("nonexistent-office-test-apps.json");
  ASSERT_TRUE(catalog.has_value());
  const auto &apps = catalog->get_apps();
  ASSERT_EQ(apps.size(), 1);
  EXPECT_EQ(apps[0].name, "Desktop");
  EXPECT_TRUE(apps[0].cmd.empty());
  EXPECT_TRUE(apps[0].detached.empty());
}

TEST(OfficePolicy, AutomaticDisplayDefaultsRespectExplicitConfiguration) {
  const auto original_video = config::video;
  const auto original_audio = config::audio;
  const auto original_stream = config::stream;
  const auto original_nvhttp = config::nvhttp;
  const auto original_input = config::input;
  const auto original_sunshine = config::sunshine;
  const auto original_modified = config::modified_config_settings;
  auto restore = util::fail_guard([&]() {
    config::video = original_video;
    config::audio = original_audio;
    config::stream = original_stream;
    config::nvhttp = original_nvhttp;
    config::input = original_input;
    config::sunshine = original_sunshine;
    config::modified_config_settings = original_modified;
  });
  // An existing fixture file prevents configuration parsing from creating an app file.
  config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/test_office.cpp";
  using dd_t = config::video_t::dd_t;
  EXPECT_EQ(config::video.dd.configuration_option, dd_t::config_option_e::ensure_active);
  EXPECT_EQ(config::video.dd.resolution_option, dd_t::resolution_option_e::automatic);
  EXPECT_EQ(config::video.dd.refresh_rate_option, dd_t::refresh_rate_option_e::automatic);
  config::apply_config_for_test("");
  EXPECT_EQ(config::video.dd.configuration_option, dd_t::config_option_e::ensure_active);
  EXPECT_EQ(config::video.dd.resolution_option, dd_t::resolution_option_e::automatic);
  EXPECT_EQ(config::video.dd.refresh_rate_option, dd_t::refresh_rate_option_e::automatic);
  config::apply_config_for_test("dd_configuration_option = disabled\ndd_resolution_option = manual\ndd_refresh_rate_option = manual\n");
  EXPECT_EQ(config::video.dd.configuration_option, dd_t::config_option_e::disabled);
  EXPECT_EQ(config::video.dd.resolution_option, dd_t::resolution_option_e::manual);
  EXPECT_EQ(config::video.dd.refresh_rate_option, dd_t::refresh_rate_option_e::manual);
}

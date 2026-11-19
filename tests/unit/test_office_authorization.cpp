/**
 * @file tests/unit/test_office_authorization.cpp
 * @brief Tests for per-connection host approval, host prompt rules, and office configuration defaults.
 */
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "office/authorization.h"
#include "office/authorization_ui.h"
#include "office/connect_confirm.h"
#include "office/nvhttp_routes.h"
#include "office/policy.h"
#include "src/config.h"
#include "src/file_handler.h"

using namespace std::literals;

namespace {
  using office::connect_confirm_t;
  using office::connect_state_e;

  static_assert(office::default_encryption_mode == config::ENCRYPTION_MODE_MANDATORY);
  static_assert(office::connect_timeout_seconds == std::chrono::duration_cast<std::chrono::seconds>(connect_confirm_t::pending_lifetime).count());

  /**
   * @brief Connection tracker driven by a manual clock.
   */
  class ConnectConfirmTest: public testing::Test {
  protected:
    connect_confirm_t::clock_t::time_point now {};  ///< Current fake time.
    connect_confirm_t tracker {[this]() {
      return now;
    }};  ///< Tracker under test.

    /**
     * @brief Identifier of the only pending request.
     * @return Request identifier, or zero when the count is not one.
     */
    std::uint64_t only_pending_id() {
      const auto pending = tracker.pending();
      return pending.size() == 1 ? pending.front().id : 0;
    }
  };

  TEST_F(ConnectConfirmTest, StartIsIdempotentAndApprovalIsOneTimePermit) {
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
    EXPECT_FALSE(tracker.admit("a"));
    EXPECT_EQ(tracker.start("a", "Phone", "192.0.2.10").state, connect_state_e::pending);
    EXPECT_EQ(tracker.start("a", "Phone", "192.0.2.10").state, connect_state_e::pending);
    const auto pending = tracker.pending();
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending.front().name, "Phone");
    EXPECT_EQ(pending.front().address, "192.0.2.10");
    EXPECT_FALSE(tracker.admit("a"));

    ASSERT_TRUE(tracker.resolve(pending.front().id, true));
    EXPECT_FALSE(tracker.resolve(pending.front().id, true));
    EXPECT_TRUE(tracker.pending().empty());
    EXPECT_EQ(tracker.status("a"), connect_state_e::approved);
    EXPECT_EQ(tracker.start("a", "Phone", "192.0.2.10").state, connect_state_e::approved);
    EXPECT_FALSE(tracker.admit("b"));

    ASSERT_TRUE(tracker.admit("a"));
    tracker.quit("a");
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
    EXPECT_FALSE(tracker.admit("a"));
  }

  TEST_F(ConnectConfirmTest, PermitExpiresAfterThirtySeconds) {
    tracker.start("a", "Phone", "192.0.2.10");
    ASSERT_TRUE(tracker.resolve(only_pending_id(), true));
    now += connect_confirm_t::permit_lifetime;
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
    EXPECT_FALSE(tracker.admit("a"));
  }

  TEST_F(ConnectConfirmTest, PendingExpiresAndResultsAreRetained) {
    tracker.start("a", "Phone", "192.0.2.10");
    now += connect_confirm_t::pending_lifetime - 1s;
    EXPECT_EQ(tracker.status("a"), connect_state_e::pending);
    now += 1s;
    EXPECT_EQ(tracker.status("a"), connect_state_e::expired);
    EXPECT_TRUE(tracker.pending().empty());
    now += connect_confirm_t::result_retention - 1s;
    EXPECT_EQ(tracker.status("a"), connect_state_e::expired);
    now += 1s;
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);

    tracker.start("b", "Tablet", "192.0.2.11");
    ASSERT_TRUE(tracker.resolve(only_pending_id(), false));
    EXPECT_EQ(tracker.status("b"), connect_state_e::denied);
    EXPECT_FALSE(tracker.admit("b"));
    now += connect_confirm_t::result_retention;
    EXPECT_EQ(tracker.status("b"), connect_state_e::none);
  }

  TEST_F(ConnectConfirmTest, ExpiredRequestCannotBeResolved) {
    tracker.start("a", "Phone", "192.0.2.10");
    const auto id = only_pending_id();
    now += connect_confirm_t::pending_lifetime;
    EXPECT_FALSE(tracker.resolve(id, true));
    EXPECT_FALSE(tracker.admit("a"));
  }

  TEST_F(ConnectConfirmTest, GlobalPendingLimitAndCancel) {
    for (const auto *cert : {"a", "b", "c"}) {
      EXPECT_EQ(tracker.start(cert, cert, "192.0.2.10").state, connect_state_e::pending);
    }
    const auto limited = tracker.start("d", "d", "192.0.2.10");
    EXPECT_TRUE(limited.limited);
    EXPECT_EQ(limited.state, connect_state_e::none);
    EXPECT_EQ(tracker.status("d"), connect_state_e::none);
    EXPECT_FALSE(tracker.start("a", "a", "192.0.2.10").limited);

    EXPECT_EQ(tracker.cancel("a"), connect_state_e::none);
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
    EXPECT_EQ(tracker.start("d", "d", "192.0.2.10").state, connect_state_e::pending);
    EXPECT_EQ(tracker.pending().size(), connect_confirm_t::max_pending);
  }

  TEST_F(ConnectConfirmTest, GraceFollowsStreamStopAndEndsOnQuitRevokeOrTimeout) {
    tracker.start("a", "Phone", "192.0.2.10");
    ASSERT_TRUE(tracker.resolve(only_pending_id(), true));
    ASSERT_TRUE(tracker.admit("a"));
    EXPECT_EQ(tracker.status("a"), connect_state_e::approved);
    EXPECT_TRUE(tracker.admit("a"));

    tracker.stream_stopped("a");
    now += connect_confirm_t::grace_lifetime - 1s;
    EXPECT_EQ(tracker.status("a"), connect_state_e::approved);
    EXPECT_EQ(tracker.start("a", "Phone", "192.0.2.10").state, connect_state_e::approved);
    ASSERT_TRUE(tracker.admit("a"));

    tracker.stream_stopped("a");
    now += connect_confirm_t::grace_lifetime;
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
    EXPECT_FALSE(tracker.admit("a"));

    tracker.start("a", "Phone", "192.0.2.10");
    ASSERT_TRUE(tracker.resolve(only_pending_id(), true));
    ASSERT_TRUE(tracker.admit("a"));
    tracker.stream_stopped("a");
    tracker.quit("a");
    EXPECT_FALSE(tracker.admit("a"));

    tracker.start("a", "Phone", "192.0.2.10");
    ASSERT_TRUE(tracker.resolve(only_pending_id(), true));
    ASSERT_TRUE(tracker.admit("a"));
    tracker.revoke("a");
    tracker.stream_stopped("a");
    EXPECT_FALSE(tracker.admit("a"));
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
  }

  TEST_F(ConnectConfirmTest, AdmissionLapsesUnlessAStreamStarts) {
    tracker.start("a", "Phone", "192.0.2.10");
    ASSERT_TRUE(tracker.resolve(only_pending_id(), true));
    ASSERT_TRUE(tracker.admit("a"));
    now += connect_confirm_t::permit_lifetime - 1s;
    EXPECT_TRUE(tracker.admit("a"));
    now += 1s;
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
    EXPECT_FALSE(tracker.admit("a"));
    tracker.stream_stopped("a");
    EXPECT_FALSE(tracker.admit("a"));

    tracker.start("a", "Phone", "192.0.2.10");
    ASSERT_TRUE(tracker.resolve(only_pending_id(), true));
    ASSERT_TRUE(tracker.admit("a"));
    tracker.stream_started("a");
    now += connect_confirm_t::grace_lifetime * 10;
    EXPECT_EQ(tracker.status("a"), connect_state_e::approved);
    EXPECT_TRUE(tracker.admit("a"));
    tracker.stream_stopped("a");
    EXPECT_TRUE(tracker.admit("a"));
  }

  TEST_F(ConnectConfirmTest, StreamStartWithoutAdmissionGrantsNothing) {
    tracker.stream_started("a");
    EXPECT_FALSE(tracker.admit("a"));
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
  }

  TEST_F(ConnectConfirmTest, StreamStopWithoutAdmissionGrantsNothing) {
    tracker.stream_stopped("a");
    EXPECT_FALSE(tracker.admit("a"));
    tracker.start("a", "Phone", "192.0.2.10");
    tracker.clear();
    EXPECT_TRUE(tracker.pending().empty());
    EXPECT_EQ(tracker.status("a"), connect_state_e::none);
  }

  TEST(ConnectConfirmState, ProtocolTokens) {
    EXPECT_EQ(office::to_string(connect_state_e::not_required), "not_required");
    EXPECT_EQ(office::to_string(connect_state_e::pending), "pending");
    EXPECT_EQ(office::to_string(connect_state_e::approved), "approved");
    EXPECT_EQ(office::to_string(connect_state_e::denied), "denied");
    EXPECT_EQ(office::to_string(connect_state_e::expired), "expired");
    EXPECT_EQ(office::to_string(connect_state_e::none), "none");
  }

  /**
   * @brief Restore the global approval switches and tracker after each test.
   */
  class OfficeAuthorizationTest: public testing::Test {
  protected:
    /**
     * @brief Enable host approval as if the prompt thread were running.
     */
    void SetUp() override {
      original_confirm = office::confirm_connections.load();
      original_prompt = office::prompt_available.load();
      office::confirm_connections.store(true);
      office::prompt_available.store(true);
      office::connect_tracker().clear();
    }

    /**
     * @brief Restore the switches and drop every request.
     */
    void TearDown() override {
      office::connect_tracker().clear();
      office::confirm_connections.store(original_confirm);
      office::prompt_available.store(original_prompt);
    }

    /**
     * @brief Run a connect-request action.
     * @param action Single action value.
     * @return Response tree.
     */
    boost::property_tree::ptree request(const std::string &action) const {
      return office::connect_request({action}, client);
    }

    office::client_identity_t client {"cert-a", "Phone", "192.0.2.10"};  ///< Synthetic verified client.

  private:
    bool original_confirm {false};  ///< Configured switch restored at teardown.
    bool original_prompt {false};  ///< Prompt availability restored at teardown.
  };

  TEST_F(OfficeAuthorizationTest, EffectiveOnlyWithAvailablePrompts) {
    EXPECT_TRUE(office::confirm_effective());
    boost::property_tree::ptree tree;
    office::add_serverinfo_capabilities(tree);
    EXPECT_EQ(tree.get<int>("root.OfficeConnectConfirm"), 1);

    office::prompt_available.store(false);
    EXPECT_FALSE(office::confirm_effective());
    office::add_connect_capability(tree);
    EXPECT_EQ(tree.get<int>("root.OfficeConnectConfirm"), 0);
    EXPECT_EQ(request("start").get<std::string>("root.state"), "not_required");

    office::prompt_available.store(true);
    office::confirm_connections.store(false);
    office::add_connect_capability(tree);
    EXPECT_EQ(tree.get<int>("root.OfficeConnectConfirm"), 0);
    const auto response = request("start");
    EXPECT_EQ(response.get<int>("root.<xmlattr>.status_code"), 200);
    EXPECT_EQ(response.get<std::string>("root.state"), "not_required");
    EXPECT_EQ(response.get<int>("root.timeoutSeconds"), 60);
    boost::property_tree::ptree stream;
    EXPECT_FALSE(office::reject_unapproved_stream(stream, "root.gamesession", client));
    EXPECT_TRUE(office::connect_tracker().pending().empty());
  }

  TEST_F(OfficeAuthorizationTest, RejectsInvalidActions) {
    for (const auto &actions : std::vector<std::vector<std::string>> {{}, {"start", "start"}, {"start", "status"}, {"begin"}, {""}, {"START"}}) {
      const auto response = office::connect_request(actions, client);
      EXPECT_EQ(response.get<int>("root.<xmlattr>.status_code"), 400);
      EXPECT_FALSE(response.get_optional<std::string>("root.state").has_value());
    }
    office::confirm_connections.store(false);
    EXPECT_EQ(office::connect_request({}, client).get<int>("root.<xmlattr>.status_code"), 400);
    EXPECT_TRUE(office::connect_tracker().pending().empty());
  }

  TEST_F(OfficeAuthorizationTest, StartStatusCancelFlow) {
    EXPECT_EQ(request("status").get<std::string>("root.state"), "none");
    const auto started = request("start");
    EXPECT_EQ(started.get<int>("root.<xmlattr>.status_code"), 200);
    EXPECT_EQ(started.get<std::string>("root.state"), "pending");
    EXPECT_EQ(started.get<int>("root.timeoutSeconds"), 60);
    EXPECT_EQ(request("start").get<std::string>("root.state"), "pending");
    EXPECT_EQ(office::connect_tracker().pending().size(), 1U);
    EXPECT_EQ(request("status").get<std::string>("root.state"), "pending");
    EXPECT_EQ(request("cancel").get<std::string>("root.state"), "none");
    EXPECT_TRUE(office::connect_tracker().pending().empty());
  }

  TEST_F(OfficeAuthorizationTest, GlobalLimitReturnsServiceUnavailable) {
    for (const auto *cert : {"b", "c", "d"}) {
      ASSERT_EQ(office::connect_request({"start"}, {cert, cert, "192.0.2.20"}).get<std::string>("root.state"), "pending");
    }
    const auto limited = request("start");
    EXPECT_EQ(limited.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(limited.get<std::string>("root.state"), "none");
  }

  TEST_F(OfficeAuthorizationTest, UnapprovedStreamIsRejectedAndPermitIsConsumedOnce) {
    boost::property_tree::ptree tree;
    ASSERT_TRUE(office::reject_unapproved_stream(tree, "root.gamesession", client));
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 403);
    EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "Host approval required");
    EXPECT_EQ(tree.get<int>("root.gamesession"), 0);

    const auto pending = office::connect_tracker().pending();
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending.front().name, "Phone");
    EXPECT_EQ(pending.front().address, "192.0.2.10");
    ASSERT_TRUE(office::reject_unapproved_stream(tree, "root.resume", client));
    EXPECT_EQ(office::connect_tracker().pending().size(), 1U);

    ASSERT_TRUE(office::connect_tracker().resolve(pending.front().id, true));
    EXPECT_EQ(request("status").get<std::string>("root.state"), "approved");
    boost::property_tree::ptree other;
    EXPECT_TRUE(office::reject_unapproved_stream(other, "root.gamesession", {"cert-b", "Other", "192.0.2.11"}));

    boost::property_tree::ptree allowed;
    EXPECT_FALSE(office::reject_unapproved_stream(allowed, "root.gamesession", client));
    EXPECT_TRUE(allowed.empty());
    EXPECT_EQ(request("status").get<std::string>("root.state"), "approved");

    office::stream_stopped(client.cert);
    EXPECT_EQ(request("status").get<std::string>("root.state"), "approved");
    office::client_quit(client.cert);
    EXPECT_TRUE(office::reject_unapproved_stream(allowed, "root.resume", client));
    EXPECT_EQ(request("status").get<std::string>("root.state"), "pending");
    office::connect_tracker().clear();

    ASSERT_TRUE(office::reject_unapproved_stream(tree, "root.gamesession", client));
    ASSERT_TRUE(office::connect_tracker().resolve(office::connect_tracker().pending().front().id, true));
    ASSERT_FALSE(office::reject_unapproved_stream(allowed, "root.gamesession", client));
    office::stream_stopped(client.cert);
    EXPECT_FALSE(office::reject_unapproved_stream(allowed, "root.resume", client));
    office::stream_stopped(client.cert);
    office::client_quit(client.cert);
    EXPECT_TRUE(office::reject_unapproved_stream(allowed, "root.resume", client));
  }

  TEST_F(OfficeAuthorizationTest, EmptyCertificateHooksAreIgnored) {
    office::stream_started("");
    office::stream_stopped("");
    office::client_quit("");
    EXPECT_EQ(request("status").get<std::string>("root.state"), "none");
  }

  /**
   * @brief Fake prompt windows recording every show and close.
   */
  class fake_backend_t: public office::prompt_backend_t {
  public:
    /**
     * @brief Record an opened window.
     * @param prompt Request shown.
     */
    void show(const office::prompt_t &prompt) override {
      shown.push_back(prompt);
    }

    /**
     * @brief Record a closed window.
     * @param kind Prompt kind.
     * @param id Request identifier.
     */
    void close(const office::prompt_kind_e kind, const std::string &id) override {
      closed.emplace_back(kind, id);
    }

    std::vector<office::prompt_t> shown;  ///< Windows opened, in order.
    std::vector<std::pair<office::prompt_kind_e, std::string>> closed;  ///< Windows closed, in order.
  };

  /**
   * @brief Prompt manager wired to a fake backend and recording actions.
   */
  class PromptManagerTest: public testing::Test {
  protected:
    /**
     * @brief Build a request list.
     * @param ids Request identifiers.
     * @return Requests named after their identifiers.
     */
    static std::vector<office::prompt_t> requests(const std::vector<std::string> &ids) {
      std::vector<office::prompt_t> result;
      for (const auto &id : ids) {
        result.push_back({office::prompt_kind_e::pairing, id, "device " + id, "192.0.2.30"});
      }
      return result;
    }

    fake_backend_t backend;  ///< Fake windows.
    std::vector<std::string> paired;  ///< `id:pin:name` of completed pairings.
    std::vector<std::string> rejected;  ///< Cancelled pairing identifiers.
    std::vector<std::pair<std::string, bool>> answers;  ///< Connection answers.
    office::prompt_manager_t manager {backend, {
                                                 .pair = [this](const std::string &id, const std::string &pin, const std::string &name) {
                                                   paired.push_back(id + ":" + pin + ":" + name);
                                                 },
                                                 .reject_pair = [this](const std::string &id) {
                                                   rejected.push_back(id);
                                                 },
                                                 .answer_connect = [this](const std::string &id, const bool allow) {
                                                   answers.emplace_back(id, allow);
                                                 },
                                               }};  ///< Manager under test.
  };

  TEST_F(PromptManagerTest, OpensAtMostThreeWindowsPerKindAndNeverReopens) {
    manager.sync(office::prompt_kind_e::pairing, requests({"1", "2", "3", "4"}));
    ASSERT_EQ(backend.shown.size(), 3U);
    EXPECT_EQ(backend.shown[0].id, "1");
    EXPECT_EQ(backend.shown[2].id, "3");
    EXPECT_EQ(backend.shown[0].kind, office::prompt_kind_e::pairing);
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::pairing), 3U);

    manager.sync(office::prompt_kind_e::connect, {{office::prompt_kind_e::pairing, "1", "Phone", "192.0.2.40"}});
    ASSERT_EQ(backend.shown.size(), 4U);
    EXPECT_EQ(backend.shown[3].kind, office::prompt_kind_e::connect);
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::connect), 1U);

    manager.sync(office::prompt_kind_e::pairing, requests({"2", "3", "4", "5"}));
    ASSERT_EQ(backend.closed.size(), 1U);
    EXPECT_EQ(backend.closed[0].second, "1");
    EXPECT_EQ(backend.closed[0].first, office::prompt_kind_e::pairing);
    ASSERT_EQ(backend.shown.size(), 5U);
    EXPECT_EQ(backend.shown[4].id, "5");
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::pairing), 3U);
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::connect), 1U);

    manager.sync(office::prompt_kind_e::pairing, requests({"2", "3", "4", "5"}));
    EXPECT_EQ(backend.shown.size(), 5U);
  }

  TEST_F(PromptManagerTest, PairingNeedsFourDigitPin) {
    manager.sync(office::prompt_kind_e::pairing, requests({"1"}));
    for (const auto pin : {""sv, "123"sv, "12345"sv, "12a4"sv, " 123"sv}) {
      EXPECT_FALSE(manager.allow(office::prompt_kind_e::pairing, "1", pin));
    }
    EXPECT_TRUE(paired.empty());
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::pairing), 1U);

    EXPECT_TRUE(manager.allow(office::prompt_kind_e::pairing, "1", "0427"));
    ASSERT_EQ(paired.size(), 1U);
    EXPECT_EQ(paired[0], "1:0427:device 1");
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::pairing), 0U);
    EXPECT_FALSE(manager.allow(office::prompt_kind_e::pairing, "1", "0427"));
    EXPECT_EQ(paired.size(), 1U);

    manager.sync(office::prompt_kind_e::pairing, requests({"1"}));
    EXPECT_EQ(backend.shown.size(), 1U);
  }

  TEST_F(PromptManagerTest, DenyAndConnectAnswers) {
    manager.sync(office::prompt_kind_e::pairing, requests({"1"}));
    EXPECT_FALSE(manager.deny(office::prompt_kind_e::connect, "1"));
    EXPECT_TRUE(manager.deny(office::prompt_kind_e::pairing, "1"));
    EXPECT_EQ(rejected, std::vector<std::string> {"1"});
    EXPECT_FALSE(manager.deny(office::prompt_kind_e::pairing, "1"));

    manager.sync(office::prompt_kind_e::connect, {{office::prompt_kind_e::connect, "7", "Phone", "192.0.2.40"}, {office::prompt_kind_e::connect, "8", "Tablet", "192.0.2.41"}});
    EXPECT_TRUE(manager.allow(office::prompt_kind_e::connect, "7"));
    EXPECT_TRUE(manager.deny(office::prompt_kind_e::connect, "8"));
    ASSERT_EQ(answers.size(), 2U);
    EXPECT_EQ(answers[0], std::make_pair("7"s, true));
    EXPECT_EQ(answers[1], std::make_pair("8"s, false));
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::connect), 0U);
  }

  TEST_F(PromptManagerTest, CloseAllDoesNotAnswer) {
    manager.sync(office::prompt_kind_e::pairing, requests({"1", "2"}));
    manager.sync(office::prompt_kind_e::connect, {{office::prompt_kind_e::connect, "7", "Phone", "192.0.2.40"}});
    manager.close_all();
    EXPECT_EQ(backend.closed.size(), 3U);
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::pairing), 0U);
    EXPECT_EQ(manager.open_count(office::prompt_kind_e::connect), 0U);
    EXPECT_TRUE(paired.empty());
    EXPECT_TRUE(rejected.empty());
    EXPECT_TRUE(answers.empty());
  }

  TEST(PromptPin, AcceptsExactlyFourAsciiDigits) {
    EXPECT_TRUE(office::is_prompt_pin("0000"));
    EXPECT_TRUE(office::is_prompt_pin("9876"));
    EXPECT_FALSE(office::is_prompt_pin("987"));
    EXPECT_FALSE(office::is_prompt_pin("98765"));
    EXPECT_FALSE(office::is_prompt_pin("98-6"));
    EXPECT_FALSE(office::is_prompt_pin("\xd9\xa1\xd9\xa2"));
  }

  /**
   * @brief Isolate configuration parsing from the user's Sunshine configuration.
   */
  class OfficeConfigTest: public testing::Test {
  protected:
    /**
     * @brief Point configuration at temporary files and save the affected settings.
     */
    void SetUp() override {
      original_config_file = config::sunshine.config_file;
      original_apps_file = config::stream.file_apps;
      original_lan = config::stream.lan_encryption_mode;
      original_wan = config::stream.wan_encryption_mode;
      original_confirm = office::confirm_connections.load();
      config_file = std::filesystem::temp_directory_path() / "sunshine_test_office_authorization.conf";  // NOSONAR(cpp:S5443): safe for tests
      apps_file = std::filesystem::temp_directory_path() / "sunshine_test_office_authorization_apps.json";  // NOSONAR(cpp:S5443): safe for tests
      std::filesystem::remove(config_file);
      ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
      config::sunshine.config_file = config_file.string();
      config::stream.file_apps = apps_file.string();
    }

    /**
     * @brief Remove the temporary files and restore the saved settings.
     */
    void TearDown() override {
      std::filesystem::remove(config_file);
      std::filesystem::remove(apps_file);
      config::sunshine.config_file = original_config_file;
      config::stream.file_apps = original_apps_file;
      config::stream.lan_encryption_mode = original_lan;
      config::stream.wan_encryption_mode = original_wan;
      office::confirm_connections.store(original_confirm);
      office::connect_tracker().clear();
    }

    std::filesystem::path config_file;  ///< Temporary configuration file.
    std::filesystem::path apps_file;  ///< Temporary application list.

  private:
    std::string original_config_file;  ///< Restored configuration path.
    std::string original_apps_file;  ///< Restored application list path.
    int original_lan {};  ///< Restored LAN encryption mode.
    int original_wan {};  ///< Restored WAN encryption mode.
    bool original_confirm {false};  ///< Restored approval switch.
  };

  TEST_F(OfficeConfigTest, ConfirmSwitchParsesAndIsConsumed) {
    for (const auto &[value, expected] : std::vector<std::pair<std::string, bool>> {{"enabled", true}, {"disabled", false}, {"true", true}, {"off", false}, {"maybe", false}, {"", false}}) {
      office::confirm_connections.store(!expected);
      config::config_map_t vars {{std::string {office::confirm_connections_key}, value}, {"other", "1"}};
      office::apply_config(vars);
      EXPECT_EQ(office::confirm_connections.load(), expected) << value;
      EXPECT_FALSE(vars.contains(office::confirm_connections_key));
      EXPECT_TRUE(vars.contains("other"));
    }

    office::confirm_connections.store(true);
    config::config_map_t missing;
    office::apply_config(missing);
    EXPECT_FALSE(office::confirm_connections.load());
  }

  TEST_F(OfficeConfigTest, DisablingDropsPendingRequests) {
    office::connect_tracker().start("cert", "Phone", "192.0.2.10");
    config::config_map_t vars {{std::string {office::confirm_connections_key}, "disabled"}};
    office::apply_config(vars);
    EXPECT_TRUE(office::connect_tracker().pending().empty());
  }

  TEST_F(OfficeConfigTest, ApplyConfigReadsSwitchAndEncryptionModes) {
    config::apply_config_for_test("desk_confirm_connections = enabled\nlan_encryption_mode = 0\nwan_encryption_mode = 1\n");
    EXPECT_TRUE(office::confirm_connections.load());
    EXPECT_EQ(config::stream.lan_encryption_mode, config::ENCRYPTION_MODE_NEVER);
    EXPECT_EQ(config::stream.wan_encryption_mode, config::ENCRYPTION_MODE_OPPORTUNISTIC);

    config::apply_config_for_test("desk_confirm_connections = disabled\n");
    EXPECT_FALSE(office::confirm_connections.load());
  }

  TEST_F(OfficeConfigTest, PersistWritesSwitchAndKeepsOtherKeys) {
    ASSERT_EQ(file_handler::write_file(config_file.string().c_str(), "wan_encryption_mode = 1\nadapter_name = Test\n"), 0);
    ASSERT_TRUE(office::set_confirm_connections(true, true));
    EXPECT_TRUE(office::confirm_connections.load());
    auto vars = config::parse_config(file_handler::read_file(config_file.string().c_str()));
    EXPECT_EQ(vars["desk_confirm_connections"], "enabled");
    EXPECT_EQ(vars["wan_encryption_mode"], "1");
    EXPECT_EQ(vars["adapter_name"], "Test");

    office::connect_tracker().start("cert", "Phone", "192.0.2.10");
    ASSERT_TRUE(office::set_confirm_connections(false, true));
    EXPECT_FALSE(office::confirm_connections.load());
    EXPECT_TRUE(office::connect_tracker().pending().empty());
    vars = config::parse_config(file_handler::read_file(config_file.string().c_str()));
    EXPECT_EQ(vars["desk_confirm_connections"], "disabled");

    ASSERT_TRUE(office::set_confirm_connections(true, false));
    vars = config::parse_config(file_handler::read_file(config_file.string().c_str()));
    EXPECT_EQ(vars["desk_confirm_connections"], "disabled");
  }
}  // namespace

TEST(OfficeEncryptionDefaults, BothNetworksDefaultToMandatory) {
  // No unit test assigns these modes without restoring them, so they still hold the built-in defaults.
  EXPECT_EQ(config::stream.lan_encryption_mode, config::ENCRYPTION_MODE_MANDATORY);
  EXPECT_EQ(config::stream.wan_encryption_mode, config::ENCRYPTION_MODE_MANDATORY);
}

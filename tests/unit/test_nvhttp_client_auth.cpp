/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

// lib includes
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/client_http.hpp>
#include <Simple-Web-Server/client_https.hpp>

// local includes
#include <src/config.h>
#include <src/nvhttp.h>

namespace fs = std::filesystem;

/**
 * @brief Isolate paired-client authorization tests from the user's Sunshine state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code remove_error;
    fs::remove(state_file, remove_error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

private:
  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sunshine Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sunshine Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  ASSERT_FALSE(nvhttp::test_support::add_client("first", credentials.x509, true).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("second", credentials.x509, true).empty());

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

/**
 * @brief Exercise the cursor route through the production TLS gate on an ephemeral loopback port.
 */
class OfficeCursorTlsTest: public ClientAuthorizationTest {
protected:
  /**
   * @brief Create synthetic credentials and start the isolated HTTPS endpoint.
   */
  void SetUp() override {
    ClientAuthorizationTest::SetUp();
    original_cursor = display_cursor.load();
    directory = fs::path {SUNSHINE_TEST_BIN_DIR} / "office_cursor_tls";
    fs::create_directories(directory);
    const auto server_credentials = crypto::gen_creds("Office test server", 2048);
    const auto paired_credentials = crypto::gen_creds("Office paired test client", 2048);
    const auto unknown_credentials = crypto::gen_creds("Office unknown test client", 2048);
    write_credentials("server", server_credentials);
    write_credentials("paired", paired_credentials);
    write_credentials("unknown", unknown_credentials);
    paired_uuid = nvhttp::test_support::add_client("office test", paired_credentials.x509, true);
    ASSERT_FALSE(paired_uuid.empty());
    server = nvhttp::test_support::make_office_cursor_server(
      (directory / "server.crt").string(),
      (directory / "server.key").string()
    );
    server_thread = std::jthread([this]() {
      server->start([this](unsigned short assigned) {
        port.store(assigned);
      });
    });
    for (int attempt = 0; attempt < 200 && port.load() == 0; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
    ASSERT_NE(port.load(), 0);
    display_cursor.store(true);
  }

  /**
   * @brief Stop the endpoint and remove all synthetic credential files.
   */
  void TearDown() override {
    if (server) {
      server->stop();
    }
    if (server_thread.joinable()) {
      server_thread.join();
    }
    server.reset();
    display_cursor.store(original_cursor);
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    ClientAuthorizationTest::TearDown();
  }

  /**
   * @brief Write generated test credentials without printing their contents.
   * @param name Filename prefix inside the isolated test directory.
   * @param credentials Synthetic certificate and private key.
   */
  void write_credentials(const std::string &name, const crypto::creds_t &credentials) {
    std::ofstream(directory / (name + ".crt")) << credentials.x509;
    std::ofstream(directory / (name + ".key")) << credentials.pkey;
  }

  /**
   * @brief Request a GameStream XML response using a fresh client connection.
   * @param target Request path and query.
   * @param identity Synthetic client identity, or empty for no client certificate.
   * @return Response XML status code.
   */
  int request_status(const std::string &target, const std::string &identity = "paired") {
    SimpleWeb::Client<SimpleWeb::HTTPS> client {
      std::format("127.0.0.1:{}", port.load()),
      false,
      identity.empty() ? "" : (directory / (identity + ".crt")).string(),
      identity.empty() ? "" : (directory / (identity + ".key")).string()
    };
    client.config.timeout = 5;
    std::istringstream response {client.request("GET", target)->content.string()};
    boost::property_tree::ptree tree;
    boost::property_tree::read_xml(response, tree);
    return tree.get<int>("root.<xmlattr>.status_code");
  }

  fs::path directory;  ///< Synthetic credentials, removed after every test.
  std::string paired_uuid;  ///< Enabled client record for the isolated test server.
  std::atomic<unsigned short> port {0};  ///< Ephemeral loopback listener port.
  std::shared_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Production TLS backend used by the test.
  std::jthread server_thread;  ///< Listener thread stopped before credentials are removed.
  bool original_cursor {true};  ///< Global cursor setting restored at teardown.
};

TEST_F(OfficeCursorTlsTest, PairedSetIsIdempotentAndInvalidInputDoesNotMutateState) {
  EXPECT_EQ(request_status("/office/cursor?visible=0"), 200);
  EXPECT_FALSE(display_cursor.load());
  EXPECT_EQ(request_status("/office/cursor?visible=0"), 200);
  EXPECT_FALSE(display_cursor.load());
  EXPECT_EQ(request_status("/office/cursor?visible=0&visible=1"), 400);
  EXPECT_EQ(request_status("/office/cursor?visible=true"), 400);
  EXPECT_EQ(request_status("/office/cursor"), 400);
  EXPECT_FALSE(display_cursor.load());
  EXPECT_EQ(request_status("/office/cursor?visible=1"), 200);
  EXPECT_TRUE(display_cursor.load());
}

TEST_F(OfficeCursorTlsTest, UnknownDisabledAndUnpairedCertificatesCannotChangeCursor) {
  EXPECT_EQ(request_status("/office/cursor?visible=0", "unknown"), 401);
  EXPECT_TRUE(display_cursor.load());
  ASSERT_TRUE(nvhttp::set_client_enabled(paired_uuid, false));
  EXPECT_EQ(request_status("/office/cursor?visible=0"), 401);
  EXPECT_TRUE(display_cursor.load());
  ASSERT_TRUE(nvhttp::set_client_enabled(paired_uuid, true));
  EXPECT_EQ(request_status("/office/cursor?visible=0"), 200);
  EXPECT_FALSE(display_cursor.load());
  ASSERT_TRUE(nvhttp::unpair_client(paired_uuid));
  EXPECT_EQ(request_status("/office/cursor?visible=1"), 401);
  EXPECT_FALSE(display_cursor.load());
}

TEST_F(OfficeCursorTlsTest, MissingCertificateAndPlainHttpCannotReachTheHandler) {
  EXPECT_ANY_THROW(request_status("/office/cursor?visible=0", ""));
  EXPECT_TRUE(display_cursor.load());
  SimpleWeb::Client<SimpleWeb::HTTP> plaintext {std::format("127.0.0.1:{}", port.load())};
  plaintext.config.timeout = 5;
  EXPECT_ANY_THROW(plaintext.request("GET", "/office/cursor?visible=0"));
  EXPECT_TRUE(display_cursor.load());
}

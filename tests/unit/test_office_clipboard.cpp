/**
 * @file tests/unit/test_office_clipboard.cpp
 * @brief Tests for clipboard synchronization: line endings, encoding, limits, access, and configuration.
 */
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "office/authorization.h"
#include "office/clipboard.h"
#include "src/config.h"
#include "src/file_handler.h"

using namespace std::literals;

namespace {
  /** @brief Synthetic clipboard that records writes and returns scripted reads. */
  class fake_clipboard final: public office::clipboard_backend {
  public:
    /**
     * @brief Construct with shared observation state.
     * @param state Scripted read result, shared with the test body.
     * @param writes Receives the text of every write.
     */
    explicit fake_clipboard(std::shared_ptr<office::clipboard_read_t> state, std::shared_ptr<std::vector<std::string>> writes):
        state {std::move(state)},
        writes {std::move(writes)} {
    }

    /** @copydoc office::clipboard_backend::read */
    office::clipboard_read_t read(const std::size_t max_units) override {
      if (throw_next) {
        throw std::runtime_error("synthetic failure");
      }
      last_max_units = max_units;
      return *state;
    }

    /** @copydoc office::clipboard_backend::write */
    office::clipboard_write_t write(const std::string &text) override {
      writes->push_back(text);
      return {state->status, state->sequence + 1};
    }

    bool throw_next {false};  ///< Throw from the next read.
    std::size_t last_max_units {};  ///< Limit passed to the last read.

  private:
    std::shared_ptr<office::clipboard_read_t> state;  ///< Scripted read result and status.
    std::shared_ptr<std::vector<std::string>> writes;  ///< Text passed to every write.
  };

  /**
   * @brief Status code of a response tree.
   * @param tree Response.
   * @return `root.<xmlattr>.status_code`.
   */
  int status(const boost::property_tree::ptree &tree) {
    return tree.get<int>("root.<xmlattr>.status_code");
  }
}  // namespace

TEST(OfficeClipboardTest, LineEndingConversion) {
  EXPECT_EQ(office::lf_to_crlf(""), "");
  EXPECT_EQ(office::lf_to_crlf("a\nb"), "a\r\nb");
  EXPECT_EQ(office::lf_to_crlf("\nfirst"), "\r\nfirst");
  EXPECT_EQ(office::lf_to_crlf("a\r\nb\nc"), "a\r\nb\r\nc");
  EXPECT_EQ(office::lf_to_crlf("a\rb"), "a\rb");
  EXPECT_EQ(office::lf_to_crlf("\n\n"), "\r\n\r\n");

  EXPECT_EQ(office::crlf_to_lf(""), "");
  EXPECT_EQ(office::crlf_to_lf("a\r\nb\r\n"), "a\nb\n");
  EXPECT_EQ(office::crlf_to_lf("a\rb\nc"), "a\rb\nc");
  EXPECT_EQ(office::crlf_to_lf("a\r\r\nb"), "a\r\nb");
  EXPECT_EQ(office::crlf_to_lf("end\r"), "end\r");

  const auto text = "line 1\nline 2\n\nline 4"s;
  EXPECT_EQ(office::crlf_to_lf(office::lf_to_crlf(text)), text);
}

TEST(OfficeClipboardTest, Base64MatchesRfc4648Vectors) {
  EXPECT_EQ(office::base64_encode(""), "");
  EXPECT_EQ(office::base64_encode("f"), "Zg==");
  EXPECT_EQ(office::base64_encode("fo"), "Zm8=");
  EXPECT_EQ(office::base64_encode("foo"), "Zm9v");
  EXPECT_EQ(office::base64_encode("foob"), "Zm9vYg==");
  EXPECT_EQ(office::base64_encode("fooba"), "Zm9vYmE=");
  EXPECT_EQ(office::base64_encode("foobar"), "Zm9vYmFy");
  EXPECT_EQ(office::base64_encode("\xff\xfe\x00"sv), "//4A");
  EXPECT_EQ(office::base64_encode("\xe4\xbd\xa0\xe5\xa5\xbd"sv), "5L2g5aW9");
}

TEST(OfficeClipboardTest, Utf8ValidationIsStrict) {
  EXPECT_TRUE(office::valid_utf8(""));
  EXPECT_TRUE(office::valid_utf8("plain ASCII\n"));
  EXPECT_TRUE(office::valid_utf8("\xc2\x80"));
  EXPECT_TRUE(office::valid_utf8("\xe4\xbd\xa0\xe5\xa5\xbd"));
  EXPECT_TRUE(office::valid_utf8("\xed\x9f\xbf"));  // U+D7FF
  EXPECT_TRUE(office::valid_utf8("\xf0\x9f\x98\x80"));  // U+1F600
  EXPECT_TRUE(office::valid_utf8("\xf4\x8f\xbf\xbf"));  // U+10FFFF
  EXPECT_TRUE(office::valid_utf8("nul\0inside"sv));

  EXPECT_FALSE(office::valid_utf8("\x80"));  // stray continuation
  EXPECT_FALSE(office::valid_utf8("\xc0\xaf"));  // overlong '/'
  EXPECT_FALSE(office::valid_utf8("\xc1\xbf"));  // overlong
  EXPECT_FALSE(office::valid_utf8("\xe0\x80\xaf"));  // overlong
  EXPECT_FALSE(office::valid_utf8("\xf0\x80\x80\xaf"));  // overlong
  EXPECT_FALSE(office::valid_utf8("\xed\xa0\x80"));  // U+D800
  EXPECT_FALSE(office::valid_utf8("\xed\xbf\xbf"));  // U+DFFF
  EXPECT_FALSE(office::valid_utf8("\xf4\x90\x80\x80"));  // above U+10FFFF
  EXPECT_FALSE(office::valid_utf8("\xf5\x80\x80\x80"));
  EXPECT_FALSE(office::valid_utf8("\xff"));
  EXPECT_FALSE(office::valid_utf8("\xe4\xbd"));  // truncated
  EXPECT_FALSE(office::valid_utf8("ok\xf0\x9f\x98"));  // truncated at end
  EXPECT_FALSE(office::valid_utf8("\xe4\x41\xa0"));  // bad continuation
}

TEST(OfficeClipboardTest, AccessRequiresStreamingClientAndSwitch) {
  const std::vector<std::string> streaming {"cert-a", "cert-c"};

  auto access = office::clipboard_access(true, "", streaming);
  EXPECT_EQ(access.status, 403);
  EXPECT_FALSE(access.message.empty());

  access = office::clipboard_access(true, "cert-b", streaming);
  EXPECT_EQ(access.status, 403);
  EXPECT_NE(access.message.find("active stream"), std::string::npos);

  access = office::clipboard_access(true, "cert-a", {});
  EXPECT_EQ(access.status, 403);

  access = office::clipboard_access(false, "cert-a", streaming);
  EXPECT_EQ(access.status, 403);
  EXPECT_NE(access.message.find("disabled"), std::string::npos);

  access = office::clipboard_access(true, "cert-c", streaming);
  EXPECT_EQ(access.status, 200);
  EXPECT_TRUE(access.message.empty());
}

/**
 * @brief Installs a synthetic backend and a running stream for one client.
 */
class OfficeClipboardRequestTest: public testing::Test {
protected:
  /**
   * @brief Install the synthetic backend and mark the test client as streaming.
   */
  void SetUp() override {
    original_sync = office::clipboard_sync.load();
    office::clipboard_sync.store(true);
    state->status = 200;
    state->sequence = 41;
    auto backend = std::make_unique<fake_clipboard>(state, writes);
    clipboard = backend.get();
    office::set_clipboard_backend_for_test(std::move(backend));
    office::stream_started(cert);
  }

  /**
   * @brief Stop the stream and remove the synthetic backend.
   */
  void TearDown() override {
    office::stream_stopped(cert);
    office::set_clipboard_backend_for_test(nullptr);
    office::clipboard_sync.store(original_sync);
    office::connect_tracker().clear();
  }

  const std::string cert {"-----TEST CLIPBOARD CERT-----"};  ///< Streaming client.
  std::shared_ptr<office::clipboard_read_t> state {std::make_shared<office::clipboard_read_t>()};  ///< Scripted backend state.
  std::shared_ptr<std::vector<std::string>> writes {std::make_shared<std::vector<std::string>>()};  ///< Text written to the backend.
  fake_clipboard *clipboard {};  ///< Installed backend, owned by the service.

private:
  bool original_sync {true};  ///< Restored switch.
};

TEST_F(OfficeClipboardRequestTest, ReadReturnsBase64WithLfLineEndings) {
  state->has_text = true;
  state->text = "a\r\nb";
  const auto tree = office::clipboard_request(false, true, "", cert);
  ASSERT_EQ(status(tree), 200);
  EXPECT_EQ(tree.get<std::uint32_t>("root.sequence"), 41U);
  EXPECT_EQ(tree.get<int>("root.hasText"), 1);
  EXPECT_EQ(tree.get<std::string>("root.text"), office::base64_encode("a\nb"));
  EXPECT_FALSE(tree.get_optional<int>("root.tooLarge"));
  EXPECT_EQ(clipboard->last_max_units, office::clipboard_max_bytes * 2);
}

TEST_F(OfficeClipboardRequestTest, ReadWithoutTextOmitsText) {
  auto tree = office::clipboard_request(false, true, "", cert);
  ASSERT_EQ(status(tree), 200);
  EXPECT_EQ(tree.get<int>("root.hasText"), 0);
  EXPECT_FALSE(tree.get_optional<std::string>("root.text"));

  state->has_text = true;
  state->text.clear();
  tree = office::clipboard_request(false, true, "", cert);
  EXPECT_EQ(tree.get<int>("root.hasText"), 0);
}

TEST_F(OfficeClipboardRequestTest, ReadReportsTooLargeText) {
  state->has_text = true;
  state->too_large = true;
  auto tree = office::clipboard_request(false, true, "", cert);
  ASSERT_EQ(status(tree), 200);
  EXPECT_EQ(tree.get<int>("root.hasText"), 0);
  EXPECT_EQ(tree.get<int>("root.tooLarge"), 1);
  EXPECT_FALSE(tree.get_optional<std::string>("root.text"));

  // Within the UTF-16 bound but over 1 MiB once encoded.
  state->too_large = false;
  state->text = std::string(office::clipboard_max_bytes + 1, 'x');
  tree = office::clipboard_request(false, true, "", cert);
  EXPECT_EQ(tree.get<int>("root.tooLarge"), 1);

  // Exactly 1 MiB after CRLF to LF conversion is allowed.
  state->text = std::string(office::clipboard_max_bytes - 1, 'x') + "\r\n";
  tree = office::clipboard_request(false, true, "", cert);
  EXPECT_EQ(tree.get<int>("root.hasText"), 1);
  EXPECT_FALSE(tree.get_optional<int>("root.tooLarge"));
}

TEST_F(OfficeClipboardRequestTest, ReadBusyOrFailingBackendIs503) {
  state->status = 503;
  auto tree = office::clipboard_request(false, true, "", cert);
  EXPECT_EQ(status(tree), 503);
  EXPECT_TRUE(tree.get_optional<std::string>("root.<xmlattr>.status_message"));

  state->status = 200;
  clipboard->throw_next = true;
  tree = office::clipboard_request(false, true, "", cert);
  EXPECT_EQ(status(tree), 503);
}

TEST_F(OfficeClipboardRequestTest, WriteConvertsToCrlf) {
  const auto tree = office::clipboard_request(true, true, "a\nb\r\nc", cert);
  ASSERT_EQ(status(tree), 200);
  EXPECT_EQ(tree.get<std::uint32_t>("root.sequence"), 42U);
  ASSERT_EQ(writes->size(), 1U);
  EXPECT_EQ(writes->front(), "a\r\nb\r\nc");
}

TEST_F(OfficeClipboardRequestTest, WriteValidatesBody) {
  EXPECT_EQ(status(office::clipboard_request(true, true, "", cert)), 400);
  EXPECT_EQ(status(office::clipboard_request(true, true, "\xc0\xaf", cert)), 400);
  EXPECT_EQ(status(office::clipboard_request(true, true, std::string(office::clipboard_max_bytes + 1, 'x'), cert)), 413);
  EXPECT_TRUE(writes->empty());

  EXPECT_EQ(status(office::clipboard_request(true, true, std::string(office::clipboard_max_bytes, 'x'), cert)), 200);
  EXPECT_EQ(writes->size(), 1U);

  state->status = 503;
  EXPECT_EQ(status(office::clipboard_request(true, true, "text", cert)), 503);
}

TEST_F(OfficeClipboardRequestTest, RejectsQueryAndGetBody) {
  EXPECT_EQ(status(office::clipboard_request(false, false, "", cert)), 400);
  EXPECT_EQ(status(office::clipboard_request(true, false, "text", cert)), 400);
  EXPECT_EQ(status(office::clipboard_request(false, true, "body", cert)), 400);
  EXPECT_TRUE(writes->empty());
}

TEST_F(OfficeClipboardRequestTest, RefusesOtherClientsAndDisabledSwitch) {
  auto tree = office::clipboard_request(false, true, "", "-----OTHER CERT-----");
  EXPECT_EQ(status(tree), 403);
  EXPECT_TRUE(tree.get_optional<std::string>("root.<xmlattr>.status_message"));
  EXPECT_EQ(status(office::clipboard_request(true, true, "text", "")), 403);

  office::clipboard_sync.store(false);
  EXPECT_EQ(status(office::clipboard_request(false, true, "", cert)), 403);
  EXPECT_EQ(status(office::clipboard_request(true, true, "text", cert)), 403);
  EXPECT_TRUE(writes->empty());

  office::clipboard_sync.store(true);
  office::stream_stopped(cert);
  EXPECT_EQ(status(office::clipboard_request(false, true, "", cert)), 403);
  office::stream_started(cert);
}

/**
 * @brief Points the configuration file at a temporary path.
 */
class OfficeClipboardConfigTest: public testing::Test {
protected:
  /**
   * @brief Swap in a temporary configuration file.
   */
  void SetUp() override {
    original_config_file = config::sunshine.config_file;
    original_sync = office::clipboard_sync.load();
    config_file = std::filesystem::temp_directory_path() / "sunshine_test_office_clipboard.conf";  // NOSONAR(cpp:S5443): safe for tests
    std::filesystem::remove(config_file);
    config::sunshine.config_file = config_file.string();
  }

  /**
   * @brief Remove the temporary file and restore the saved settings.
   */
  void TearDown() override {
    std::filesystem::remove(config_file);
    config::sunshine.config_file = original_config_file;
    office::clipboard_sync.store(original_sync);
  }

  std::filesystem::path config_file;  ///< Temporary configuration file.

private:
  std::string original_config_file;  ///< Restored configuration path.
  bool original_sync {true};  ///< Restored switch.
};

TEST_F(OfficeClipboardConfigTest, SwitchParsesDefaultsOnAndIsConsumed) {
  for (const auto &[value, expected] : std::vector<std::pair<std::string, bool>> {{"enabled", true}, {"disabled", false}, {"false", false}, {"on", true}, {"maybe", true}, {"", true}}) {
    office::clipboard_sync.store(!expected);
    config::config_map_t vars {{std::string {office::clipboard_sync_key}, value}, {"other", "1"}};
    office::apply_clipboard_config(vars);
    EXPECT_EQ(office::clipboard_sync.load(), expected) << value;
    EXPECT_FALSE(vars.contains(office::clipboard_sync_key));
    EXPECT_TRUE(vars.contains("other"));
  }

  office::clipboard_sync.store(false);
  config::config_map_t missing;
  office::apply_clipboard_config(missing);
  EXPECT_TRUE(office::clipboard_sync.load());

  config::apply_config_for_test("desk_clipboard_sync = disabled\n");
  EXPECT_FALSE(office::clipboard_sync.load());
  config::apply_config_for_test("");
  EXPECT_TRUE(office::clipboard_sync.load());
}

TEST_F(OfficeClipboardConfigTest, PersistWritesSwitchAndKeepsOtherKeys) {
  ASSERT_EQ(file_handler::write_file(config_file.string().c_str(), "adapter_name = Test\n"), 0);
  ASSERT_TRUE(office::set_clipboard_sync(false, true));
  EXPECT_FALSE(office::clipboard_sync.load());
  auto vars = config::parse_config(file_handler::read_file(config_file.string().c_str()));
  EXPECT_EQ(vars["desk_clipboard_sync"], "disabled");
  EXPECT_EQ(vars["adapter_name"], "Test");

  ASSERT_TRUE(office::set_clipboard_sync(true, true));
  vars = config::parse_config(file_handler::read_file(config_file.string().c_str()));
  EXPECT_EQ(vars["desk_clipboard_sync"], "enabled");

  ASSERT_TRUE(office::set_clipboard_sync(false, false));
  EXPECT_FALSE(office::clipboard_sync.load());
  vars = config::parse_config(file_handler::read_file(config_file.string().c_str()));
  EXPECT_EQ(vars["desk_clipboard_sync"], "enabled");
}

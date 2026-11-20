/**
 * @file office/clipboard.cpp
 * @brief Text clipboard synchronization: request policy, line endings, encoding, and configuration.
 */
// standard includes
#include <algorithm>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>

// local includes
#include "office/authorization.h"
#include "office/clipboard.h"
#include "src/logging.h"

using namespace std::literals;

namespace office {
  namespace {
    namespace pt = boost::property_tree;

    /** @brief Request state protected as one unit so reads and writes never interleave. */
    struct clipboard_service {
      std::mutex mutex;  ///< Serializes requests and backend replacement.
      std::unique_ptr<clipboard_backend> backend;  ///< Lazily constructed platform backend.
    };

    /** @brief Get process-local service state. @return Shared state for all clients. */
    clipboard_service &service() {
      static clipboard_service value;
      return value;
    }

    /**
     * @brief Serializes runtime switches so memory and configuration file stay in step.
     * @return Process-wide switch mutex.
     */
    std::mutex &switch_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    /**
     * @brief Build a failure response.
     * @param status GameStream status code.
     * @param message Reason shown to the client.
     * @return Tree with `status_code` and `status_message` set.
     */
    pt::ptree failure(const int status, const std::string_view message) {
      pt::ptree tree;
      tree.put("root.<xmlattr>.status_code", status);
      tree.put("root.<xmlattr>.status_message", std::string {message});
      return tree;
    }

    /**
     * @brief Get the backend, constructing the platform one on first use.
     * @param state Locked service state.
     * @return Backend, or nullptr on platforms without clipboard support.
     */
    clipboard_backend *backend(clipboard_service &state) {
#ifdef _WIN32
      if (!state.backend) {
        state.backend = make_windows_clipboard_backend();
      }
#endif
      return state.backend.get();
    }

    /**
     * @brief Serve a validated GET.
     * @param clipboard Platform backend.
     * @return Response tree.
     */
    pt::ptree read_clipboard(clipboard_backend &clipboard) {
      // Every LF byte on the wire costs at most two UTF-16 units (CR LF) and every other byte at
      // least half a unit, so longer text is certainly over the limit and is not converted.
      auto result = clipboard.read(clipboard_max_bytes * 2);
      if (result.status != 200) {
        return failure(result.status, "The clipboard is busy; try again"sv);
      }
      std::string text;
      bool too_large = result.too_large;
      if (result.has_text && !too_large) {
        text = crlf_to_lf(result.text);
        too_large = text.size() > clipboard_max_bytes;
      }
      pt::ptree tree;
      tree.put("root.<xmlattr>.status_code", 200);
      tree.put("root.sequence", result.sequence);
      if (too_large) {
        BOOST_LOG(warning) << "Clipboard text exceeds "sv << clipboard_max_bytes << " bytes; not sent to the client"sv;
        tree.put("root.hasText", 0);
        tree.put("root.tooLarge", 1);
      } else if (result.has_text && !text.empty()) {
        tree.put("root.hasText", 1);
        tree.put("root.text", base64_encode(text));
        BOOST_LOG(debug) << "Clipboard read: "sv << text.size() << " bytes"sv;
      } else {
        tree.put("root.hasText", 0);
      }
      return tree;
    }

    /**
     * @brief Serve a validated POST.
     * @param clipboard Platform backend.
     * @param body Valid, non-empty UTF-8 text with LF line endings.
     * @return Response tree.
     */
    pt::ptree write_clipboard(clipboard_backend &clipboard, const std::string_view body) {
      const auto result = clipboard.write(lf_to_crlf(body));
      if (result.status != 200) {
        return failure(result.status, "The clipboard is busy; try again"sv);
      }
      BOOST_LOG(debug) << "Clipboard written: "sv << body.size() << " bytes"sv;
      pt::ptree tree;
      tree.put("root.<xmlattr>.status_code", 200);
      tree.put("root.sequence", result.sequence);
      return tree;
    }
  }  // namespace

  std::string lf_to_crlf(const std::string_view text) {
    std::string result;
    result.reserve(text.size() + static_cast<std::size_t>(std::ranges::count(text, '\n')));
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
        result += '\r';
      }
      result += text[i];
    }
    return result;
  }

  std::string crlf_to_lf(const std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        continue;
      }
      result += text[i];
    }
    return result;
  }

  std::string base64_encode(const std::string_view data) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto byte = [&data](const std::size_t index) -> std::uint32_t {
      return index < data.size() ? static_cast<unsigned char>(data[index]) : 0U;
    };
    std::string result;
    result.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
      const std::uint32_t chunk = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
      const auto rest = data.size() - i;
      result += alphabet[(chunk >> 18) & 0x3FU];
      result += alphabet[(chunk >> 12) & 0x3FU];
      result += rest > 1 ? alphabet[(chunk >> 6) & 0x3FU] : '=';
      result += rest > 2 ? alphabet[chunk & 0x3FU] : '=';
    }
    return result;
  }

  bool valid_utf8(const std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
      const auto lead = static_cast<unsigned char>(text[i]);
      if (lead < 0x80) {
        ++i;
        continue;
      }
      std::size_t length = 0;
      unsigned char low = 0x80;
      unsigned char high = 0xBF;
      if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
      } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        if (lead == 0xE0) {
          low = 0xA0;  // overlong
        } else if (lead == 0xED) {
          high = 0x9F;  // surrogates
        }
      } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        if (lead == 0xF0) {
          low = 0x90;  // overlong
        } else if (lead == 0xF4) {
          high = 0x8F;  // above U+10FFFF
        }
      } else {
        return false;
      }
      if (text.size() - i < length) {
        return false;
      }
      for (std::size_t k = 1; k < length; ++k) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if (next < (k == 1 ? low : 0x80) || next > (k == 1 ? high : 0xBF)) {
          return false;
        }
      }
      i += length;
    }
    return true;
  }

  clipboard_access_t clipboard_access(const bool enabled, const std::string &cert, const std::vector<std::string> &streaming) {
    if (cert.empty()) {
      return {403, "The client is not paired or has been disabled"};
    }
    if (!std::ranges::binary_search(streaming, cert)) {
      return {403, "Clipboard synchronization requires an active stream from this client"};
    }
    if (!enabled) {
      return {403, "Clipboard synchronization is disabled on the host"};
    }
    return {200, {}};
  }

  pt::ptree clipboard_request(const bool post, const bool query_empty, const std::string_view body, const std::string &cert) {
    if (const auto access = clipboard_access(clipboard_sync.load(), cert, streaming_certs()); access.status != 200) {
      return failure(access.status, access.message);
    }
    if (!query_empty) {
      return failure(400, "Clipboard requests take no query parameters"sv);
    }
    if (!post && !body.empty()) {
      return failure(400, "GET requires an empty body"sv);
    }
    if (post) {
      if (body.empty()) {
        return failure(400, "The clipboard text is empty"sv);
      }
      if (body.size() > clipboard_max_bytes) {
        return failure(413, "The clipboard text exceeds 1 MiB"sv);
      }
      if (!valid_utf8(body)) {
        return failure(400, "The clipboard text is not valid UTF-8"sv);
      }
    }

    auto &state = service();
    const std::scoped_lock lock {state.mutex};
    auto *clipboard = backend(state);
    if (!clipboard) {
      return failure(501, "Clipboard synchronization is supported only on Windows"sv);
    }
    try {
      return post ? write_clipboard(*clipboard, body) : read_clipboard(*clipboard);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Clipboard request failed: "sv << e.what();
      return failure(503, "The clipboard is temporarily unavailable"sv);
    }
  }

  void apply_clipboard_config(config::config_map_t &vars) {
    bool enabled = true;
    if (const auto it = vars.find(clipboard_sync_key); it != vars.end()) {
      if (const auto parsed = parse_config_switch(it->second)) {
        enabled = *parsed;
      } else {
        BOOST_LOG(warning) << "Invalid value for "sv << clipboard_sync_key << ": "sv << it->second << "; using enabled"sv;
      }
      vars.erase(it);
    }
    clipboard_sync.store(enabled);
  }

  bool set_clipboard_sync(const bool enabled, const bool persist) {
    const std::scoped_lock lock {switch_mutex()};
    clipboard_sync.store(enabled);
    BOOST_LOG(info) << "Clipboard synchronization "sv << (enabled ? "enabled"sv : "disabled"sv);
    return !persist || persist_config_value(clipboard_sync_key, enabled ? "enabled"s : "disabled"s);
  }

#ifdef SUNSHINE_TESTS
  void set_clipboard_backend_for_test(std::unique_ptr<clipboard_backend> backend) {
    auto &state = service();
    const std::scoped_lock lock {state.mutex};
    state.backend = std::move(backend);
  }
#endif
}  // namespace office

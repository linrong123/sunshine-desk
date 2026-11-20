/**
 * @file office/clipboard.h
 * @brief Text clipboard synchronization for the streaming paired client.
 *
 * The protocol is specified in `office/CLIPBOARD.md`. Text on the wire is UTF-8 with LF line
 * endings; the Windows clipboard holds CRLF text. Only the paired client that is currently
 * streaming may read or write, and the host can turn the feature off.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "src/config.h"

namespace office {
  inline constexpr std::string_view clipboard_sync_key = "desk_clipboard_sync";  ///< Configuration key, `enabled` (default) or `disabled`.
  inline constexpr std::size_t clipboard_max_bytes = std::size_t {1} << 20;  ///< Largest clipboard text exchanged, in UTF-8 bytes with LF line endings.

  /**
   * @brief Whether the host allows clipboard synchronization.
   *
   * Read by the request handler; written by `apply_clipboard_config` and `set_clipboard_sync`.
   */
  inline std::atomic_bool clipboard_sync {true};

  /** @brief Outcome of the access check that precedes every clipboard request. */
  struct clipboard_access_t {
    int status {403};  ///< 200 when allowed, otherwise 403.
    std::string message;  ///< Reason for a refusal; empty when allowed.
  };

  /** @brief Result of reading the system clipboard. */
  struct clipboard_read_t {
    int status {503};  ///< 200 on success; 503 when the clipboard stays busy.
    std::uint32_t sequence {};  ///< `GetClipboardSequenceNumber` while the clipboard was open.
    bool has_text {};  ///< Whether Unicode text was available.
    bool too_large {};  ///< Whether the text exceeded the limit; `text` is then empty.
    std::string text;  ///< UTF-8 text exactly as stored, normally with CRLF line endings.
  };

  /** @brief Result of writing the system clipboard. */
  struct clipboard_write_t {
    int status {503};  ///< 200 on success; 503 when the clipboard stays busy or rejects the data.
    std::uint32_t sequence {};  ///< `GetClipboardSequenceNumber` after the write.
  };

  /** @brief Injectable operating-system boundary; tests install a synthetic implementation. */
  class clipboard_backend {
  public:
    /** @brief Destroy the backend. */
    virtual ~clipboard_backend() = default;

    /**
     * @brief Read Unicode text from the system clipboard.
     * @param max_units Text longer than this many UTF-16 code units is reported as too large without being converted.
     * @return Text, sequence number, or a busy failure.
     */
    virtual clipboard_read_t read(std::size_t max_units) = 0;

    /**
     * @brief Replace the system clipboard with Unicode text.
     * @param text Valid, non-empty UTF-8 text with CRLF line endings.
     * @return Sequence number after the write, or a busy failure.
     */
    virtual clipboard_write_t write(const std::string &text) = 0;
  };

  /**
   * @brief Convert line endings for the Windows clipboard.
   * @param text Text with LF, CRLF, or mixed line endings.
   * @return Text in which every LF is preceded by CR; existing CRLF pairs are kept as they are.
   */
  std::string lf_to_crlf(std::string_view text);

  /**
   * @brief Convert line endings for the wire.
   * @param text Text with CRLF line endings.
   * @return Text in which every CRLF pair became LF; a lone CR is kept.
   */
  std::string crlf_to_lf(std::string_view text);

  /**
   * @brief Standard base64 with padding (RFC 4648, section 4).
   * @param data Bytes to encode.
   * @return Encoded text without line breaks.
   */
  std::string base64_encode(std::string_view data);

  /**
   * @brief Strict UTF-8 validation.
   * @param text Bytes to check.
   * @return `false` for truncated sequences, overlong forms, surrogates, and code points above U+10FFFF.
   */
  bool valid_utf8(std::string_view text);

  /**
   * @brief Decide whether a verified client may use the clipboard.
   * @param enabled Value of `clipboard_sync`.
   * @param cert Verified client certificate PEM; empty when the client is unknown or disabled.
   * @param streaming Sorted certificates of the clients with a running stream (`streaming_certs`).
   * @return 200, or 403 with the reason.
   */
  clipboard_access_t clipboard_access(bool enabled, const std::string &cert, const std::vector<std::string> &streaming);

  /**
   * @brief Validate and serve one clipboard request; requests are serialized.
   * @param post `true` for POST (write), `false` for GET (read).
   * @param query_empty Whether the request carried no query parameters.
   * @param body Raw request body.
   * @param cert Verified client certificate PEM; empty when the client is unknown or disabled.
   * @return GameStream XML tree with `root.<xmlattr>.status_code` and the protocol fields.
   */
  boost::property_tree::ptree clipboard_request(bool post, bool query_empty, std::string_view body, const std::string &cert);

  /**
   * @brief Parse and consume `clipboard_sync_key` from a configuration map.
   * @param vars Parsed configuration entries; the key is erased when present.
   */
  void apply_clipboard_config(config::config_map_t &vars);

  /**
   * @brief Turn clipboard synchronization on or off at runtime.
   * @param enabled New setting.
   * @param persist `true` to rewrite `clipboard_sync_key` in the configuration file.
   * @return `false` if persisting was requested and the configuration file could not be written.
   */
  bool set_clipboard_sync(bool enabled, bool persist);

#ifdef _WIN32
  /**
   * @brief Construct the Win32 clipboard implementation.
   * @return Backend that opens the clipboard of the process window station.
   */
  std::unique_ptr<clipboard_backend> make_windows_clipboard_backend();
#endif

#ifdef SUNSHINE_TESTS
  /**
   * @brief Replace the backend while holding the request mutex.
   * @param backend Synthetic backend, or nullptr to restore the production platform backend.
   */
  void set_clipboard_backend_for_test(std::unique_ptr<clipboard_backend> backend);
#endif
}  // namespace office

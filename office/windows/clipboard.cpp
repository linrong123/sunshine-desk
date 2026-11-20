/**
 * @file office/windows/clipboard.cpp
 * @brief Win32 text clipboard backend for clipboard synchronization.
 */
#ifdef _WIN32

  #include "office/clipboard.h"

  #include "src/logging.h"
  #include "src/platform/windows/utf_utils.h"

  #include <cstring>
  #include <cwchar>
  #include <windows.h>

using namespace std::literals;

namespace office {
  namespace {
    constexpr int open_attempts = 5;  ///< `OpenClipboard` attempts before reporting the clipboard as busy.
    constexpr DWORD open_retry_ms = 30;  ///< Pause between attempts; another application holds the clipboard only briefly.

    /**
     * @brief Open the clipboard, retrying while another application holds it.
     * @param owner Window that becomes the clipboard owner after `EmptyClipboard`, or nullptr to read.
     * @return `true` once the clipboard is open.
     */
    bool open_clipboard(HWND owner) {
      for (int attempt = 0; attempt < open_attempts; ++attempt) {
        if (attempt > 0) {
          Sleep(open_retry_ms);
        }
        if (OpenClipboard(owner)) {
          return true;
        }
      }
      return false;
    }

    /** @brief Close the clipboard when leaving scope. */
    struct clipboard_guard {
      /** @brief Close the clipboard opened by `open_clipboard`. */
      ~clipboard_guard() {
        CloseClipboard();
      }
    };

    /**
     * @brief Convert UTF-16 to UTF-8, replacing unpaired surrogates with U+FFFD.
     * @param text UTF-16 code units.
     * @param length Number of code units.
     * @return Valid UTF-8 text, empty on failure.
     */
    std::string to_utf8(const wchar_t *text, const std::size_t length) {
      if (length == 0) {
        return {};
      }
      const auto units = static_cast<int>(length);
      const auto size = WideCharToMultiByte(CP_UTF8, 0, text, units, nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        return {};
      }
      std::string result(static_cast<std::size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, text, units, result.data(), size, nullptr, nullptr);
      return result;
    }

    /** @brief Clipboard of the window station the Sunshine process runs in. */
    class windows_clipboard final: public clipboard_backend {
    public:
      /** @copydoc clipboard_backend::read */
      clipboard_read_t read(const std::size_t max_units) override {
        clipboard_read_t result;
        if (!open_clipboard(nullptr)) {
          BOOST_LOG(warning) << "Clipboard read: OpenClipboard failed: "sv << GetLastError();
          return result;
        }
        clipboard_guard guard;
        result.status = 200;
        result.sequence = GetClipboardSequenceNumber();
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
          return result;
        }
        const auto memory = GetClipboardData(CF_UNICODETEXT);
        if (!memory) {
          return result;
        }
        const auto *const data = static_cast<const wchar_t *>(GlobalLock(memory));
        if (!data) {
          return result;
        }
        // The clipboard owner controls the memory; never read past its allocation.
        const auto length = wcsnlen(data, GlobalSize(memory) / sizeof(wchar_t));
        if (length > max_units) {
          result.has_text = true;
          result.too_large = true;
        } else {
          result.text = to_utf8(data, length);
          result.has_text = !result.text.empty();
        }
        GlobalUnlock(memory);
        return result;
      }

      /** @copydoc clipboard_backend::write */
      clipboard_write_t write(const std::string &text) override {
        clipboard_write_t result;
        const auto wide = utf_utils::from_utf8(text);
        if (wide.empty()) {
          result.status = 400;
          return result;
        }
        // SetClipboardData fails after EmptyClipboard when the clipboard was opened without an owner window.
        const auto owner = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!owner) {
          BOOST_LOG(warning) << "Clipboard write: CreateWindowExW failed: "sv << GetLastError();
          return result;
        }
        bool written = false;
        if (open_clipboard(owner)) {
          clipboard_guard guard;
          const auto bytes = (wide.size() + 1) * sizeof(wchar_t);
          if (const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (auto *const target = GlobalLock(memory)) {
              std::memcpy(target, wide.c_str(), bytes);
              GlobalUnlock(memory);
              written = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;  // The clipboard owns the memory on success.
            }
            if (!written) {
              GlobalFree(memory);
            }
          }
          if (!written) {
            BOOST_LOG(warning) << "Clipboard write: SetClipboardData failed: "sv << GetLastError();
          }
        } else {
          BOOST_LOG(warning) << "Clipboard write: OpenClipboard failed: "sv << GetLastError();
        }
        DestroyWindow(owner);
        if (written) {
          result.status = 200;
          result.sequence = GetClipboardSequenceNumber();
        }
        return result;
      }
    };
  }  // namespace

  std::unique_ptr<clipboard_backend> make_windows_clipboard_backend() {
    return std::make_unique<windows_clipboard>();
  }
}  // namespace office

#endif

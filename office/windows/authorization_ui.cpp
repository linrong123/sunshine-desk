/**
 * @file office/windows/authorization_ui.cpp
 * @brief Windows host prompts for pairing and connection requests, and the tray device list.
 *
 * One dedicated UI thread owns every prompt window and polls the pending pairing sessions and
 * connection requests every 500 ms. Answers that may block (pairing, client changes, the delete
 * confirmation box) run on short-lived worker threads so neither the UI thread nor the tray thread
 * waits on network or file I/O. See office/CONNECTION.md for the behaviour and its limits.
 */
#ifdef _WIN32

  // standard includes
  #include <algorithm>
  #include <array>
  #include <charconv>
  #include <cstdint>
  #include <format>
  #include <future>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <utility>
  #include <vector>

  // local includes
  #include "office/authorization.h"
  #include "office/authorization_ui.h"
  #include "src/logging.h"
  #include "src/nvhttp.h"
  #include "src/platform/windows/utf_utils.h"
  #if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
    #include "office/tray_devices.h"
    #include "src/system_tray.h"
  #endif

  // platform includes
  #include <windows.h>

using namespace std::literals;

namespace office {
  namespace {
    constexpr auto prompt_class_name = L"SunshineDeskPrompt";  ///< Window class of every prompt window.
    constexpr UINT tick_interval_ms = 500;  ///< Polling interval for pending requests.
    constexpr int pin_control_id = 100;  ///< PIN edit control.
    constexpr int hint_control_id = 101;  ///< PIN validation hint.
    constexpr auto fallback_device_name = "Moonlight"sv;  ///< Name used when a client reports no valid name.

    class prompt_ui_t;

    /**
     * @brief Per-window data stored in `GWLP_USERDATA`.
     */
    struct window_data_t {
      prompt_ui_t *ui {};  ///< Owning UI state.
      prompt_kind_e kind {prompt_kind_e::pairing};  ///< Prompt kind.
      std::string id;  ///< Request identifier.
      HWND pin {};  ///< PIN edit control for pairing prompts.
      HWND hint {};  ///< PIN validation hint for pairing prompts.
    };

    /**
     * @brief Pairing name passed to @ref nvhttp::pin and shown to the host user.
     * @param name Name reported by the client.
     * @return The reported name if valid, otherwise "Moonlight".
     */
    std::string pairing_name(const std::string &name) {
      return nvhttp::is_valid_pairing_name(name) ? name : std::string {fallback_device_name};
    }

    /**
     * @brief Approval actions wired to nvhttp and the connection tracker.
     * @return Actions for @ref prompt_manager_t.
     */
    prompt_actions_t make_actions() {
      return {
        .pair = [](const std::string &id, const std::string &pin, const std::string &name) {
          // nvhttp::pin waits for the pairing exchange, so it must not block the UI thread.
          std::thread([id, pin, name]() {
            if (!nvhttp::pin(id, pin, pairing_name(name))) {
              BOOST_LOG(warning) << "Pairing approved on the host did not complete"sv;
            }
          }).detach();
        },
        .reject_pair = [](const std::string &id) {
          nvhttp::cancel_pairing(id);
          BOOST_LOG(info) << "Pairing request denied on the host"sv;
        },
        .answer_connect = [](const std::string &id, const bool allow) {
          std::uint64_t value {};
          const auto [_, error] = std::from_chars(id.data(), id.data() + id.size(), value);
          if (error == std::errc {} && connect_tracker().resolve(value, allow)) {
            BOOST_LOG(info) << "Connection request "sv << (allow ? "approved"sv : "denied"sv) << " on the host"sv;
          }
        },
      };
    }

    /**
     * @brief Prompt windows and polling state owned by the UI thread.
     */
    class prompt_ui_t final: public prompt_backend_t {
    public:
      /**
       * @brief Create the UI state on the UI thread.
       * @param instance Module instance used for window creation.
       */
      explicit prompt_ui_t(HINSTANCE instance):
          instance_ {instance} {
        NONCLIENTMETRICSW metrics {};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
          font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        }
        if (const auto screen = GetDC(nullptr)) {
          dpi_ = GetDeviceCaps(screen, LOGPIXELSY);
          ReleaseDC(nullptr, screen);
        }
      }

      /**
       * @brief Close every prompt window and release the font.
       */
      ~prompt_ui_t() override {
        manager_.close_all();
        if (font_) {
          DeleteObject(font_);
        }
      }

      prompt_ui_t(const prompt_ui_t &) = delete;
      prompt_ui_t &operator=(const prompt_ui_t &) = delete;

      /**
       * @brief Open a prompt window for a request.
       * @param prompt Request to show.
       */
      void show(const prompt_t &prompt) override {
        const bool pairing = prompt.kind == prompt_kind_e::pairing;
        const auto title = pairing ? L"Sunshine 配对请求"s : L"Sunshine 连接请求"s;
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        constexpr DWORD ex_style = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;

        RECT frame {0, 0, scale(360), scale(pairing ? 180 : 116)};
        AdjustWindowRectEx(&frame, style, FALSE, ex_style);
        const auto width = frame.right - frame.left;
        const auto height = frame.bottom - frame.top;
        RECT work {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const auto offset = scale(28) * static_cast<int>(windows_.size());
        const auto x = work.left + (work.right - work.left - width) / 2 + offset;
        const auto y = work.top + (work.bottom - work.top - height) / 3 + offset;

        auto data = std::make_unique<window_data_t>(window_data_t {this, prompt.kind, prompt.id});
        const auto hwnd = CreateWindowExW(ex_style, prompt_class_name, title.c_str(), style, x, y, width, height, nullptr, nullptr, instance_, data.get());
        if (!hwnd) {
          BOOST_LOG(warning) << "Failed to create host prompt window: "sv << GetLastError();
          return;
        }
        auto *const window = data.release();  // Owned by the window; freed on WM_NCDESTROY.

        const auto headline = pairing ? std::format("「{}」请求配对", prompt.name) : std::format("「{}」请求连接此电脑", prompt.name);
        add_child(hwnd, L"STATIC", utf_utils::from_utf8(headline), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 16, 16, 328, 22, 0);
        add_child(hwnd, L"STATIC", utf_utils::from_utf8(std::format("IP：{}", prompt.address)), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 16, 42, 328, 20, 0);

        auto buttons_y = 72;
        if (pairing) {
          add_child(hwnd, L"STATIC", L"输入设备上显示的 4 位 PIN：", SS_LEFT | SS_NOPREFIX, 16, 72, 328, 20, 0);
          window->pin = add_child(hwnd, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, 16, 96, 120, 26, pin_control_id);
          window->hint = add_child(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 148, 100, 196, 20, hint_control_id);
          SendMessageW(window->pin, EM_SETLIMITTEXT, 4, 0);
          buttons_y = 136;
        }
        add_child(hwnd, L"BUTTON", L"允许", WS_TABSTOP | BS_DEFPUSHBUTTON, 168, buttons_y, 84, 30, IDOK);
        add_child(hwnd, L"BUTTON", L"拒绝", WS_TABSTOP | BS_PUSHBUTTON, 260, buttons_y, 84, 30, IDCANCEL);

        windows_.insert_or_assign({prompt.kind, prompt.id}, hwnd);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        SetFocus(window->pin ? window->pin : GetDlgItem(hwnd, IDOK));
      }

      /**
       * @brief Destroy the prompt window of a request, if it is open.
       * @param kind Prompt kind.
       * @param id Request identifier.
       */
      void close(const prompt_kind_e kind, const std::string &id) override {
        const auto it = windows_.find({kind, id});
        if (it == windows_.end()) {
          return;
        }
        const auto hwnd = it->second;
        windows_.erase(it);
        DestroyWindow(hwnd);
      }

      /**
       * @brief Handle the allow button of a window.
       * @param window Data of the window.
       */
      void on_allow(const window_data_t &window) {
        const auto kind = window.kind;
        const auto id = window.id;
        std::string pin;
        if (window.pin) {
          std::array<wchar_t, 8> text {};
          GetWindowTextW(window.pin, text.data(), static_cast<int>(text.size()));
          pin = utf_utils::to_utf8(text.data());
        }
        const auto hint = window.hint;
        const auto edit = window.pin;
        if (!manager_.allow(kind, id, pin) && hint) {
          SetWindowTextW(hint, L"请输入 4 位数字 PIN");
          SetFocus(edit);
        }
      }

      /**
       * @brief Handle the deny button or the close box of a window.
       * @param window Data of the window.
       */
      void on_deny(const window_data_t &window) {
        const auto kind = window.kind;
        const auto id = window.id;
        manager_.deny(kind, id);
      }

      /**
       * @brief Poll pending requests and the paired-device list.
       */
      void tick() {
        std::vector<prompt_t> pairings;
        for (const auto &pairing : nvhttp::get_pending_pairings()) {
          pairings.push_back({prompt_kind_e::pairing, pairing.id, pairing_name(pairing.name), pairing.address});
        }
        manager_.sync(prompt_kind_e::pairing, pairings);

        std::vector<prompt_t> connects;
        if (confirm_effective()) {
          for (const auto &request : connect_tracker().pending()) {
            connects.push_back({prompt_kind_e::connect, std::to_string(request.id), request.name, request.address});
          }
        }
        manager_.sync(prompt_kind_e::connect, connects);

  #if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
        std::vector<tray_device_t> devices;
        for (const auto &client : nvhttp::get_all_clients()) {
          devices.push_back({client.value("uuid", ""s), client.value("name", ""s), client.value("enabled", true)});
        }
        const auto confirm = confirm_connections.load();
        if (!tray_synced_ || devices != tray_devices_ || confirm != tray_confirm_) {
          set_tray_devices(devices);
          system_tray::refresh_office_menu();
          tray_devices_ = std::move(devices);
          tray_confirm_ = confirm;
          tray_synced_ = true;
        }
  #endif
      }

    private:
      /**
       * @brief Scale a 96-DPI length to the screen DPI.
       * @param value Length at 96 DPI.
       * @return Length in pixels.
       */
      int scale(const int value) const {
        return MulDiv(value, dpi_, 96);
      }

      /**
       * @brief Create a child control.
       * @param parent Prompt window.
       * @param class_name Control class.
       * @param text Initial text.
       * @param style Control-specific style.
       * @param x Left edge at 96 DPI.
       * @param y Top edge at 96 DPI.
       * @param width Width at 96 DPI.
       * @param height Height at 96 DPI.
       * @param id Control identifier, or 0.
       * @return Control handle.
       */
      HWND add_child(HWND parent, const wchar_t *class_name, const std::wstring &text, const DWORD style, const int x, const int y, const int width, const int height, const int id) const {
        const auto control = CreateWindowExW(0, class_name, text.c_str(), WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(width), scale(height), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (control && font_) {
          SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        }
        return control;
      }

      HINSTANCE instance_ {};  ///< Module instance.
      HFONT font_ {};  ///< Message font of the current theme.
      int dpi_ {96};  ///< Screen DPI.
      std::map<std::pair<prompt_kind_e, std::string>, HWND> windows_;  ///< Open prompt windows.
      prompt_manager_t manager_ {*this, make_actions()};  ///< Prompt rules.
  #if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
      std::vector<tray_device_t> tray_devices_;  ///< Device list last published to the tray.
      bool tray_confirm_ {false};  ///< Approval switch last published to the tray.
      bool tray_synced_ {false};  ///< Whether the tray was published at least once.
  #endif
    };

    /**
     * @brief Window procedure shared by every prompt window.
     * @param hwnd Window.
     * @param message Message identifier.
     * @param wparam First message parameter.
     * @param lparam Second message parameter.
     * @return Message result.
     */
    LRESULT CALLBACK prompt_window_proc(HWND hwnd, const UINT message, const WPARAM wparam, const LPARAM lparam) {
      if (message == WM_NCCREATE) {
        const auto *const create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      }
      auto *const window = reinterpret_cast<window_data_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

      switch (message) {
        case WM_COMMAND:
          if (window && HIWORD(wparam) == BN_CLICKED) {
            if (LOWORD(wparam) == IDOK) {
              window->ui->on_allow(*window);
              return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
              window->ui->on_deny(*window);
              return 0;
            }
          }
          break;
        case WM_CLOSE:
          if (window) {
            // Closing the window counts as a denial; the request must not linger unanswered.
            window->ui->on_deny(*window);
            return 0;
          }
          break;
        case WM_NCDESTROY:
          SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
          delete window;
          break;
        default:
          break;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    /**
     * @brief Body of the UI thread.
     * @param ready Receives the thread identifier once the message queue exists, or 0 on failure.
     */
    void run_prompt_thread(std::promise<DWORD> &ready) {
      MSG message {};
      PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // Create the message queue.

      const auto instance = GetModuleHandleW(nullptr);
      WNDCLASSEXW window_class {};
      window_class.cbSize = sizeof(window_class);
      window_class.lpfnWndProc = prompt_window_proc;
      window_class.hInstance = instance;
      window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
      window_class.lpszClassName = prompt_class_name;
      if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        BOOST_LOG(error) << "Failed to register the host prompt window class: "sv << GetLastError();
        ready.set_value(0);
        return;
      }

      const auto timer = SetTimer(nullptr, 0, tick_interval_ms, nullptr);
      if (!timer) {
        BOOST_LOG(error) << "Failed to start the host prompt timer: "sv << GetLastError();
        ready.set_value(0);
        return;
      }

      {
        prompt_ui_t ui {instance};
        prompt_available.store(true);
        ready.set_value(GetCurrentThreadId());
        ui.tick();

        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          if (message.message == WM_TIMER && message.hwnd == nullptr) {
            ui.tick();
            continue;
          }
          if (const auto root = message.hwnd ? GetAncestor(message.hwnd, GA_ROOT) : nullptr; root && IsDialogMessageW(root, &message)) {
            continue;
          }
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        prompt_available.store(false);
      }
      KillTimer(nullptr, timer);
    }
  }  // namespace

  /**
   * @brief Windows prompt thread state.
   */
  struct authorization_ui_t::impl_t {
    std::thread thread;  ///< UI thread.
    DWORD thread_id {};  ///< UI thread identifier for posting `WM_QUIT`.
  };

  authorization_ui_t::authorization_ui_t():
      impl_ {std::make_unique<impl_t>()} {
    std::promise<DWORD> ready;
    auto started = ready.get_future();
    impl_->thread = std::thread([ready = std::move(ready)]() mutable {
      run_prompt_thread(ready);
    });
    impl_->thread_id = started.get();
    if (impl_->thread_id == 0) {
      impl_->thread.join();
      BOOST_LOG(warning) << "Host prompts are unavailable; per-connection approval stays off"sv;
    }
  }

  authorization_ui_t::~authorization_ui_t() {
    if (impl_->thread_id != 0) {
      PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    }
    if (impl_->thread.joinable()) {
      impl_->thread.join();
    }
  }

  #if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
  struct tray_menu tray_devices_menu[tray_devices_menu_size] {};

  namespace {
    /**
     * @brief Device bound to one tray slot; read by the tray callbacks.
     */
    struct device_slot_t {
      std::string uuid;  ///< Paired client identifier; empty for an unused slot.
      std::string name;  ///< Paired device name.
      bool enabled {true};  ///< Whether the client may connect.
    };

    /**
     * @brief Mutex for the published device list and the slots.
     * @return Process-wide mutex.
     */
    std::mutex &tray_devices_mutex() {
      static std::mutex mutex;
      return mutex;
    }

    /**
     * @brief Device list published by the UI thread.
     * @return Storage guarded by @ref tray_devices_mutex.
     */
    std::vector<tray_device_t> &published_devices() {
      static std::vector<tray_device_t> devices;
      return devices;
    }

    /**
     * @brief Devices bound to tray slots.
     * @return Storage guarded by @ref tray_devices_mutex.
     */
    std::array<device_slot_t, tray_device_slots> &device_slots() {
      static std::array<device_slot_t, tray_device_slots> slots;
      return slots;
    }

    /**
     * @brief Label storage of the device entries; written only under the tray state mutex.
     * @return Label storage.
     */
    std::array<std::string, tray_device_slots> &device_labels() {
      static std::array<std::string, tray_device_slots> labels;
      return labels;
    }

    /**
     * @brief Per-device submenus: toggle, delete, terminator.
     * @return Submenu storage.
     */
    std::array<std::array<struct tray_menu, 3>, tray_device_slots> &device_menus() {
      static std::array<std::array<struct tray_menu, 3>, tray_device_slots> menus {};
      return menus;
    }

    /**
     * @brief Copy the device bound to a tray item.
     * @param item Clicked menu item whose context points to a slot.
     * @return Copy of the slot.
     */
    device_slot_t slot_of(const struct tray_menu *item) {
      const std::scoped_lock lock {tray_devices_mutex()};
      if (!item || !item->context) {
        return {};
      }
      return *static_cast<const device_slot_t *>(item->context);
    }

    /**
     * @brief Tray callback that disables or enables a paired device.
     * @param item Clicked menu item.
     */
    void tray_toggle_device_cb(struct tray_menu *item) {
      auto slot = slot_of(item);
      if (slot.uuid.empty()) {
        return;
      }
      std::thread([uuid = std::move(slot.uuid), enable = !slot.enabled]() {
        set_client_access(uuid, enable);
      }).detach();
    }

    /**
     * @brief Tray callback that deletes a paired device after confirmation.
     * @param item Clicked menu item.
     */
    void tray_delete_device_cb(struct tray_menu *item) {
      auto slot = slot_of(item);
      if (slot.uuid.empty()) {
        return;
      }
      std::thread([slot = std::move(slot)]() {
        const auto text = utf_utils::from_utf8(std::format("删除「{}」的授权？\n删除后该设备需要重新配对才能连接。", slot.name));
        if (MessageBoxW(nullptr, text.c_str(), L"Sunshine", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) {
          delete_client(slot.uuid);
        }
      }).detach();
    }
  }  // namespace

  void set_tray_devices(std::vector<tray_device_t> devices) {
    const std::scoped_lock lock {tray_devices_mutex()};
    published_devices() = std::move(devices);
  }

  void rebuild_tray_devices() {
    auto &menus = device_menus();
    auto &slots = device_slots();
    auto &labels = device_labels();
    static bool wired = false;  // Guarded by the tray state mutex held by the caller.
    if (!wired) {
      for (std::size_t i = 0; i < tray_device_slots; ++i) {
        menus[i][0].cb = tray_toggle_device_cb;
        menus[i][0].context = &slots[i];
        menus[i][1].text = "删除授权";
        menus[i][1].cb = tray_delete_device_cb;
        menus[i][1].context = &slots[i];
      }
      wired = true;
    }

    const std::scoped_lock lock {tray_devices_mutex()};
    const auto &devices = published_devices();
    const auto shown = std::min(devices.size(), tray_device_slots);
    for (std::size_t i = 0; i < tray_device_slots; ++i) {
      if (i >= shown) {
        slots[i] = {};
        continue;
      }
      const auto &device = devices[i];
      slots[i] = {device.uuid, device.name, device.enabled};
      const auto name = device.name.empty() || device.name == "-" ? std::format("设备 {}", i + 1) : device.name;
      labels[i] = device.enabled ? name : std::format("{}（已禁用）", name);
      menus[i][0].text = device.enabled ? "禁用" : "启用";
      tray_devices_menu[i].text = labels[i].c_str();
      tray_devices_menu[i].disabled = 0;
      tray_devices_menu[i].submenu = menus[i].data();
    }

    auto next = shown;
    if (devices.empty()) {
      tray_devices_menu[next].text = "无已授权设备";
      tray_devices_menu[next].disabled = 1;
      tray_devices_menu[next].submenu = nullptr;
      ++next;
    } else if (devices.size() > tray_device_slots) {
      tray_devices_menu[next].text = "更多设备请在 Web 界面管理";
      tray_devices_menu[next].disabled = 1;
      tray_devices_menu[next].submenu = nullptr;
      ++next;
    }
    for (; next < tray_devices_menu_size; ++next) {
      tray_devices_menu[next].text = nullptr;
      tray_devices_menu[next].disabled = 0;
      tray_devices_menu[next].submenu = nullptr;
    }
  }

  void tray_confirm_cb([[maybe_unused]] struct tray_menu *item) {
    std::thread([enable = !confirm_connections.load()]() {
      set_confirm_connections(enable, true);
    }).detach();
  }
  #endif
}  // namespace office
#endif

/**
 * @file office/windows/main_window.cpp
 * @brief Windows Sunshine Desk main window and the device actions shared with the tray.
 *
 * The window lives on the host prompt thread (see office/windows/authorization_ui.cpp) and is
 * refreshed by its 500 ms timer while it is open. What it shows is decided by the
 * platform-independent helpers in office/main_window.h. See office/MAIN_WINDOW.md.
 */
#ifdef _WIN32

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <cstring>
  #include <format>
  #include <optional>
  #include <set>
  #include <string>
  #include <thread>
  #include <utility>
  #include <vector>

  // lib includes
  #include <boost/asio/ip/address.hpp>

  // local includes
  #include "office/authorization.h"
  #include "office/main_window.h"
  #include "office/windows/main_window.h"
  #include "src/config.h"
  #include "src/entry_handler.h"
  #include "src/logging.h"
  #include "src/network.h"
  #include "src/nvhttp.h"
  #include "src/platform/common.h"
  #include "src/platform/windows/utf_utils.h"

  // platform includes
  #include <iphlpapi.h>
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>

using namespace std::literals;

namespace office {
  namespace {
    constexpr auto main_class_name = L"SunshineDeskMain";  ///< Window class of the main window.
    constexpr auto icon_resource_name = L"SuperDuperAmazing";  ///< Application icon in src/platform/windows/windows.rc.
    constexpr int client_width = 480;  ///< Client width at 96 DPI.
    constexpr int client_height = 484;  ///< Client height at 96 DPI.
    constexpr unsigned name_refresh_ticks = 4;  ///< Ticks between re-reading the saved device name.
    constexpr unsigned address_refresh_ticks = 10;  ///< Ticks between re-enumerating network addresses.
    constexpr unsigned copied_feedback_ticks = 4;  ///< Ticks the "已复制" button text stays.

    /**
     * @brief Control identifiers of the main window.
     */
    enum control_id_e : int {
      addresses_id = 200,  ///< Address list.
      copy_id,  ///< Copy address button.
      name_id,  ///< Device name edit.
      apply_id,  ///< Apply and restart button.
      devices_id,  ///< Paired device list.
      toggle_id,  ///< Disable / enable button.
      remove_id,  ///< Delete authorization button.
      confirm_id,  ///< Per-connection approval check box.
      advanced_id,  ///< Open the web UI button.
    };

    std::atomic_bool name_change_running {false};  ///< Whether a device name change is in progress on a worker thread.

    /**
     * @brief Text of a window as UTF-8.
     * @param hwnd Window or control.
     * @return Window text.
     */
    std::string window_text(HWND hwnd) {
      const auto length = GetWindowTextLengthW(hwnd);
      if (length <= 0) {
        return {};
      }
      std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
      text.resize(static_cast<std::size_t>(GetWindowTextW(hwnd, text.data(), length + 1)));
      return utf_utils::to_utf8(text);
    }

    /**
     * @brief Replace the text of a control only when it changed, to avoid flicker.
     * @param hwnd Control.
     * @param text New UTF-8 text.
     */
    void set_text(HWND hwnd, const std::string &text) {
      if (window_text(hwnd) != text) {
        SetWindowTextW(hwnd, utf_utils::from_utf8(text).c_str());
      }
    }

    /**
     * @brief Put text on the clipboard.
     * @param owner Window that opens the clipboard.
     * @param text UTF-8 text.
     * @return `true` if the clipboard now holds the text.
     */
    bool copy_to_clipboard(HWND owner, const std::string &text) {
      const auto wide = utf_utils::from_utf8(text);
      if (!OpenClipboard(owner)) {
        return false;
      }
      EmptyClipboard();
      const auto bytes = (wide.size() + 1) * sizeof(wchar_t);
      bool copied = false;
      if (const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (auto *const target = GlobalLock(memory)) {
          std::memcpy(target, wide.c_str(), bytes);
          GlobalUnlock(memory);
          copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;  // The clipboard owns the memory on success.
        }
        if (!copied) {
          GlobalFree(memory);
        }
      }
      CloseClipboard();
      return copied;
    }

    /**
     * @brief Unicast addresses of every network adapter.
     * @return Addresses with adapter state, or an empty list if enumeration fails.
     */
    std::vector<adapter_address_t> adapter_addresses() {
      constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
      ULONG size = 16 * 1024;
      std::vector<ULONGLONG> buffer;  // ULONGLONG keeps the adapter structures aligned.
      ULONG result = ERROR_BUFFER_OVERFLOW;
      for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size / sizeof(ULONGLONG) + 1);
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
      }
      if (result != NO_ERROR) {
        if (result != ERROR_NO_DATA) {
          BOOST_LOG(warning) << "Failed to enumerate network addresses: "sv << result;
        }
        return {};
      }

      std::vector<adapter_address_t> addresses;
      for (auto adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter; adapter = adapter->Next) {
        const bool up = adapter->OperStatus == IfOperStatusUp;
        const bool tunnel = adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        const auto name = adapter->FriendlyName ? utf_utils::to_utf8(adapter->FriendlyName) : std::string {};
        for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
          const auto *const socket_address = unicast->Address.lpSockaddr;
          if (!socket_address) {
            continue;
          }
          std::string text;
          if (socket_address->sa_family == AF_INET) {
            boost::asio::ip::address_v4::bytes_type bytes;
            std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in *>(socket_address)->sin_addr, bytes.size());
            text = boost::asio::ip::address_v4 {bytes}.to_string();
          } else if (socket_address->sa_family == AF_INET6) {
            boost::asio::ip::address_v6::bytes_type bytes;
            std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in6 *>(socket_address)->sin6_addr, bytes.size());
            text = boost::asio::ip::address_v6 {bytes}.to_string();
          } else {
            continue;
          }
          addresses.push_back({name, std::move(text), up, tunnel, unicast->DadState == IpDadStatePreferred});
        }
      }
      return addresses;
    }

    /**
     * @brief Whether two device lists would render identically.
     * @param a First list.
     * @param b Second list.
     * @return `true` if every row has the same identifier, label, and state.
     */
    bool same_rows(const std::vector<device_row_t> &a, const std::vector<device_row_t> &b) {
      return std::ranges::equal(a, b, [](const device_row_t &x, const device_row_t &y) {
        return x.uuid == y.uuid && x.label == y.label && x.enabled == y.enabled;
      });
    }
  }  // namespace

  void request_client_access(std::string uuid, const bool enabled) {
    std::thread([uuid = std::move(uuid), enabled]() {
      set_client_access(uuid, enabled);
    }).detach();
  }

  void request_delete_client(std::string uuid, std::string name) {
    std::thread([uuid = std::move(uuid), name = std::move(name)]() {
      const auto text = utf_utils::from_utf8(std::format("删除「{}」的授权？\n删除后该设备需要重新配对才能连接。", name));
      if (MessageBoxW(nullptr, text.c_str(), L"Sunshine", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) {
        delete_client(uuid);
      }
    }).detach();
  }

  void request_confirm_connections(const bool enabled) {
    std::thread([enabled]() {
      set_confirm_connections(enabled, true);
    }).detach();
  }

  /**
   * @brief Main window state; touched only on the prompt thread.
   */
  struct main_window_t::impl_t {
    HINSTANCE instance {};  ///< Module instance.
    HFONT font {};  ///< Message font of the current theme.
    HFONT bold {};  ///< Section heading font.
    int dpi {96};  ///< Screen DPI.
    bool registered {false};  ///< Whether the window class is registered.

    HWND hwnd {};  ///< Main window; null while closed.
    HWND addresses {};  ///< Address list.
    HWND copy {};  ///< Copy address button.
    HWND name {};  ///< Device name edit.
    HWND apply {};  ///< Apply and restart button.
    HWND name_hint {};  ///< Device name hint.
    HWND devices {};  ///< Paired device list.
    HWND toggle {};  ///< Disable / enable button.
    HWND remove {};  ///< Delete authorization button.
    HWND confirm {};  ///< Per-connection approval check box.
    HWND status {};  ///< Service status line.

    int http_port {default_http_port};  ///< GameStream HTTP port.
    std::vector<host_address_t> host_addresses;  ///< Addresses in the list.
    std::vector<device_row_t> rows;  ///< Devices in the list.
    std::optional<std::string> saved_name;  ///< `sunshine_name` in the configuration file.
    std::string host_name;  ///< Computer name used when no device name is saved.
    unsigned ticks {0};  ///< Ticks since the window opened.
    unsigned copied_ticks {0};  ///< Remaining ticks of the "已复制" feedback.

    /**
     * @brief Scale a 96-DPI length to the screen DPI.
     * @param value Length at 96 DPI.
     * @return Length in pixels.
     */
    int scale(const int value) const {
      return MulDiv(value, dpi, 96);
    }

    /**
     * @brief Create a child control of the main window.
     * @param class_name Control class.
     * @param text Initial text.
     * @param style Control-specific style.
     * @param x Left edge at 96 DPI.
     * @param y Top edge at 96 DPI.
     * @param width Width at 96 DPI.
     * @param height Height at 96 DPI.
     * @param id Control identifier, or 0.
     * @param heading `true` to use the heading font.
     * @return Control handle.
     */
    HWND add_child(const wchar_t *class_name, const std::wstring &text, const DWORD style, const int x, const int y, const int width, const int height, const int id = 0, const bool heading = false) const {
      const DWORD ex_style = (style & WS_BORDER) ? static_cast<DWORD>(WS_EX_CLIENTEDGE) : 0;
      const auto control = CreateWindowExW(ex_style, class_name, text.c_str(), WS_CHILD | WS_VISIBLE | (style & ~WS_BORDER), scale(x), scale(y), scale(width), scale(height), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
      if (const auto control_font = heading && bold ? bold : font; control && control_font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(control_font), FALSE);
      }
      return control;
    }

    /**
     * @brief Create the window and its controls.
     * @return `true` if the window exists afterwards.
     */
    bool create() {
      constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
      constexpr DWORD ex_style = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT;
      RECT frame {0, 0, scale(client_width), scale(client_height)};
      AdjustWindowRectEx(&frame, style, FALSE, ex_style);
      const auto width = frame.right - frame.left;
      const auto height = frame.bottom - frame.top;
      RECT work {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
      SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
      const auto x = work.left + std::max(0L, (work.right - work.left - width) / 2);
      const auto y = work.top + std::max(0L, (work.bottom - work.top - height) / 2);

      hwnd = CreateWindowExW(ex_style, main_class_name, L"Sunshine Desk", style, x, y, width, height, nullptr, nullptr, instance, this);
      if (!hwnd) {
        BOOST_LOG(warning) << "Failed to create the Sunshine Desk main window: "sv << GetLastError();
        return false;
      }

      constexpr DWORD label = SS_LEFT | SS_NOPREFIX;
      constexpr DWORD list = WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT;
      add_child(L"STATIC", L"本机地址", label, 16, 12, 448, 20, 0, true);
      addresses = add_child(L"LISTBOX", L"", list, 16, 34, 340, 72, addresses_id);
      copy = add_child(L"BUTTON", L"复制地址", WS_TABSTOP | BS_PUSHBUTTON, 368, 34, 96, 28, copy_id);
      const auto hint = add_child(L"STATIC", L"", label, 16, 110, 448, 40, 0);

      add_child(L"STATIC", L"设备名称", label, 16, 156, 448, 20, 0, true);
      name = add_child(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16, 179, 240, 24, name_id);
      apply = add_child(L"BUTTON", L"应用并重启服务", WS_TABSTOP | BS_PUSHBUTTON, 268, 177, 140, 28, apply_id);
      name_hint = add_child(L"STATIC", L"", label | SS_ENDELLIPSIS, 16, 208, 448, 20, 0);

      add_child(L"STATIC", L"已授权设备", label, 16, 236, 448, 20, 0, true);
      devices = add_child(L"LISTBOX", L"", list, 16, 258, 340, 96, devices_id);
      toggle = add_child(L"BUTTON", L"禁用", WS_TABSTOP | BS_PUSHBUTTON, 368, 258, 96, 28, toggle_id);
      remove = add_child(L"BUTTON", L"删除授权", WS_TABSTOP | BS_PUSHBUTTON, 368, 292, 96, 28, remove_id);
      confirm = add_child(L"BUTTON", L"每次连接需本机确认", WS_TABSTOP | BS_CHECKBOX, 16, 362, 448, 22, confirm_id);

      add_child(L"STATIC", L"服务状态", label, 16, 392, 448, 20, 0, true);
      status = add_child(L"STATIC", L"", label | SS_ENDELLIPSIS, 16, 414, 448, 20, 0);

      add_child(L"BUTTON", L"打开高级设置（网页）", WS_TABSTOP | BS_PUSHBUTTON, 16, 444, 180, 30, advanced_id);
      add_child(L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, 384, 444, 80, 30, IDCANCEL);

      http_port = net::map_port(nvhttp::PORT_HTTP);
      set_text(hint, port_hint(http_port));
      host_name = platf::get_host_name();
      saved_name = read_config_value(device_name_key);
      set_text(name, saved_name && !saved_name->empty() ? *saved_name : config::nvhttp.sunshine_name);
      SendMessageW(name, EM_SETLIMITTEXT, 4 * device_name_max_length, 0);

      ticks = 0;
      copied_ticks = 0;
      host_addresses.clear();
      rows.clear();
      refresh_addresses(true);
      refresh_devices(true);
      update_name_hint();

      ShowWindow(hwnd, SW_SHOWNORMAL);
      SetForegroundWindow(hwnd);
      SetFocus(addresses);
      return true;
    }

    /**
     * @brief Re-enumerate the network addresses and update the list when they changed.
     * @param force `true` to rebuild the list even if nothing changed.
     */
    void refresh_addresses(const bool force) {
      auto current = connectable_addresses(adapter_addresses(), http_port);
      const auto same = std::ranges::equal(current, host_addresses, [](const host_address_t &a, const host_address_t &b) {
        return a.adapter == b.adapter && a.connect_text == b.connect_text;
      });
      if (same && !force) {
        return;
      }
      const auto selected = static_cast<int>(SendMessageW(addresses, LB_GETCURSEL, 0, 0));
      const auto previous = selected >= 0 && static_cast<std::size_t>(selected) < host_addresses.size() ? host_addresses[selected].connect_text : std::string {};
      host_addresses = std::move(current);

      SendMessageW(addresses, WM_SETREDRAW, FALSE, 0);
      SendMessageW(addresses, LB_RESETCONTENT, 0, 0);
      int reselect = host_addresses.empty() ? -1 : 0;
      for (std::size_t i = 0; i < host_addresses.size(); ++i) {
        const auto &entry = host_addresses[i];
        const auto text = utf_utils::from_utf8(entry.adapter.empty() ? entry.connect_text : std::format("{}    {}", entry.connect_text, entry.adapter));
        SendMessageW(addresses, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (entry.connect_text == previous) {
          reselect = static_cast<int>(i);
        }
      }
      if (host_addresses.empty()) {
        SendMessageW(addresses, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未找到可用的网络地址，请检查网络连接"));
      }
      SendMessageW(addresses, LB_SETCURSEL, static_cast<WPARAM>(reselect), 0);
      SendMessageW(addresses, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(addresses, nullptr, TRUE);
      EnableWindow(copy, !host_addresses.empty());
    }

    /**
     * @brief Re-read paired devices, running streams, and the approval switch.
     * @param force `true` to rebuild the list even if nothing changed.
     */
    void refresh_devices(const bool force) {
      const auto clients = nvhttp::get_all_clients();
      const auto certs = streaming_certs();
      std::set<std::string, std::less<>> streaming;
      std::set<std::string, std::less<>> matched;
      if (!certs.empty() && clients.is_array()) {
        for (const auto &client : clients) {
          if (!client.is_object()) {
            continue;
          }
          const auto uuid = client.value("uuid", ""s);
          const auto cert = uuid.empty() ? std::string {} : nvhttp::get_cert_by_uuid(uuid);
          if (!cert.empty() && std::ranges::binary_search(certs, cert)) {
            streaming.insert(uuid);
            matched.insert(cert);
          }
        }
      }
      auto current = device_rows(clients, streaming);
      set_text(status, service_status(current, certs.size() - matched.size()));

      const auto checked = SendMessageW(confirm, BM_GETCHECK, 0, 0) == BST_CHECKED;
      if (const auto enabled = confirm_connections.load(); checked != enabled) {
        SendMessageW(confirm, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
      }

      if (!force && same_rows(current, rows)) {
        return;
      }
      const auto selected = selected_row();
      const auto previous = selected ? selected->uuid : std::string {};
      rows = std::move(current);

      SendMessageW(devices, WM_SETREDRAW, FALSE, 0);
      SendMessageW(devices, LB_RESETCONTENT, 0, 0);
      int reselect = -1;
      for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto text = utf_utils::from_utf8(rows[i].label);
        SendMessageW(devices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (!previous.empty() && rows[i].uuid == previous) {
          reselect = static_cast<int>(i);
        }
      }
      if (rows.empty()) {
        SendMessageW(devices, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"暂无已授权设备"));
      }
      SendMessageW(devices, LB_SETCURSEL, static_cast<WPARAM>(reselect), 0);
      SendMessageW(devices, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(devices, nullptr, TRUE);
      update_device_buttons();
    }

    /**
     * @brief Device row selected in the list.
     * @return Pointer into @ref rows, or null when nothing valid is selected.
     */
    const device_row_t *selected_row() const {
      if (!devices) {
        return nullptr;
      }
      const auto index = static_cast<int>(SendMessageW(devices, LB_GETCURSEL, 0, 0));
      return index >= 0 && static_cast<std::size_t>(index) < rows.size() ? &rows[index] : nullptr;
    }

    /**
     * @brief Enable the device buttons for the selection and label the toggle button.
     */
    void update_device_buttons() const {
      const auto *const row = selected_row();
      EnableWindow(toggle, row != nullptr);
      EnableWindow(remove, row != nullptr);
      set_text(toggle, row && !row->enabled ? "启用"s : "禁用"s);
    }

    /**
     * @brief Device name change that the apply button would perform.
     */
    struct name_change_t {
      std::optional<std::string> value;  ///< Normalized input; empty means the computer name; no value if invalid.
      std::string effective;  ///< Name the service would advertise.
      bool write {false};  ///< Whether the configuration file must change.
      bool restart {false};  ///< Whether the service must restart.
    };

    /**
     * @brief Compare the typed name with the saved and the running name.
     * @return Planned change.
     */
    name_change_t plan_name_change() const {
      name_change_t change;
      change.value = normalize_device_name(window_text(name));
      if (!change.value) {
        return change;
      }
      change.effective = change.value->empty() ? host_name : *change.value;
      const auto saved = saved_name.value_or(std::string {});
      // With no saved name the box shows the computer name; keeping it is not a change.
      change.write = *change.value != saved && !(saved.empty() && *change.value == host_name);
      change.restart = restart_required(device_name_key) && change.effective != config::nvhttp.sunshine_name;
      return change;
    }

    /**
     * @brief Update the device name hint and the apply button.
     */
    void update_name_hint() const {
      const auto change = plan_name_change();
      std::string hint;
      if (!change.value) {
        hint = std::format("名称不能包含 #、控制字符或以 [ 开头，最多 {} 个字符", device_name_max_length);
      } else if (name_change_running.load()) {
        hint = "正在应用…";
      } else if (change.restart && !change.write) {
        hint = std::format("已保存「{}」，重启服务后生效", change.effective);
      } else if (change.restart) {
        hint = "修改后需重启服务，正在进行的串流会中断";
      } else if (change.write) {
        hint = "与当前名称相同，保存后无需重启";
      } else {
        hint = change.value->empty() ? std::format("留空时使用计算机名「{}」", host_name) : "客户端看到的就是这个名称"s;
      }
      set_text(name_hint, hint);
      EnableWindow(apply, change.value && (change.write || change.restart) && !name_change_running.load());
      set_text(apply, change.value && change.write && !change.restart ? "保存"s : "应用并重启服务"s);
    }

    /**
     * @brief Apply the typed device name on a worker thread.
     */
    void apply_name() {
      const auto change = plan_name_change();
      if (!change.value || !(change.write || change.restart) || name_change_running.exchange(true)) {
        return;
      }
      update_name_hint();
      std::thread([change]() {
        if (change.restart) {
          const auto text = utf_utils::from_utf8(std::format("将设备名称改为「{}」并重启 Sunshine 服务？\n正在进行的串流会中断，客户端需要稍后重新连接。", change.effective));
          if (MessageBoxW(nullptr, text.c_str(), L"Sunshine Desk", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) != IDYES) {
            name_change_running.store(false);
            return;
          }
        }
        if (change.write) {
          const auto value = change.value->empty() ? std::nullopt : change.value;
          if (!persist_config_value(device_name_key, value)) {
            name_change_running.store(false);
            MessageBoxW(nullptr, L"保存设备名称失败，详情见 Sunshine 日志。", L"Sunshine Desk", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
            return;
          }
        }
        if (change.restart) {
          BOOST_LOG(info) << "Restarting to apply the new device name"sv;
          platf::restart();
          return;  // The flag stays set; the process is exiting.
        }
        name_change_running.store(false);
      }).detach();
    }

    /**
     * @brief Copy the selected (or the first) connect address.
     */
    void copy_address() {
      if (host_addresses.empty()) {
        return;
      }
      const auto selected = static_cast<int>(SendMessageW(addresses, LB_GETCURSEL, 0, 0));
      const auto &entry = selected >= 0 && static_cast<std::size_t>(selected) < host_addresses.size() ? host_addresses[selected] : host_addresses.front();
      if (copy_to_clipboard(hwnd, entry.connect_text)) {
        set_text(copy, "已复制");
        copied_ticks = copied_feedback_ticks;
      } else {
        BOOST_LOG(warning) << "Failed to copy the address to the clipboard: "sv << GetLastError();
      }
    }

    /**
     * @brief Handle `WM_COMMAND`.
     * @param id Control identifier.
     * @param code Notification code.
     */
    void on_command(const int id, const int code) {
      switch (id) {
        case IDCANCEL:
          DestroyWindow(hwnd);
          break;
        case IDOK:
          // Enter from IsDialogMessage: apply when typing a name, otherwise ignore.
          if (GetFocus() == name) {
            apply_name();
          }
          break;
        case addresses_id:
          if (code == LBN_DBLCLK) {
            copy_address();
          }
          break;
        case copy_id:
          copy_address();
          break;
        case name_id:
          if (code == EN_CHANGE) {
            update_name_hint();
          }
          break;
        case apply_id:
          apply_name();
          break;
        case devices_id:
          if (code == LBN_SELCHANGE) {
            update_device_buttons();
          }
          break;
        case toggle_id:
          if (const auto *const row = selected_row()) {
            request_client_access(row->uuid, !row->enabled);
          }
          break;
        case remove_id:
          if (const auto *const row = selected_row()) {
            request_delete_client(row->uuid, row->name);
          }
          break;
        case confirm_id:
          request_confirm_connections(!confirm_connections.load());
          break;
        case advanced_id:
          std::thread([]() {
            launch_ui();
          }).detach();
          break;
        default:
          break;
      }
    }

    /**
     * @brief Forget every control handle after the window is destroyed.
     */
    void on_destroyed() {
      hwnd = addresses = copy = name = apply = name_hint = devices = toggle = remove = confirm = status = nullptr;
    }

    /**
     * @brief Window procedure of the main window.
     * @param window Window.
     * @param message Message identifier.
     * @param wparam First message parameter.
     * @param lparam Second message parameter.
     * @return Message result.
     */
    static LRESULT CALLBACK window_proc(HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
      if (message == WM_NCCREATE) {
        const auto *const create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      }
      auto *const self = reinterpret_cast<impl_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      switch (message) {
        case WM_COMMAND:
          if (self) {
            self->on_command(LOWORD(wparam), HIWORD(wparam));
            return 0;
          }
          break;
        case WM_CLOSE:
          DestroyWindow(window);
          return 0;
        case WM_DESTROY:
          if (self) {
            self->on_destroyed();
          }
          break;
        case WM_NCDESTROY:
          SetWindowLongPtrW(window, GWLP_USERDATA, 0);
          break;
        default:
          break;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }
  };

  main_window_t::main_window_t():
      impl_ {std::make_unique<impl_t>()} {
    auto &w = *impl_;
    w.instance = GetModuleHandleW(nullptr);
    NONCLIENTMETRICSW metrics {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
      w.font = CreateFontIndirectW(&metrics.lfMessageFont);
      auto heading = metrics.lfMessageFont;
      heading.lfWeight = FW_BOLD;
      w.bold = CreateFontIndirectW(&heading);
    }
    if (const auto screen = GetDC(nullptr)) {
      w.dpi = GetDeviceCaps(screen, LOGPIXELSY);
      ReleaseDC(nullptr, screen);
    }

    WNDCLASSEXW window_class {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = impl_t::window_proc;
    window_class.hInstance = w.instance;
    window_class.hIcon = LoadIconW(w.instance, icon_resource_name);
    window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);  // IDC_ARROW is a narrow resource id without UNICODE.
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    window_class.lpszClassName = main_class_name;
    w.registered = RegisterClassExW(&window_class) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    if (!w.registered) {
      BOOST_LOG(warning) << "Failed to register the Sunshine Desk main window class: "sv << GetLastError();
    }
  }

  main_window_t::~main_window_t() {
    if (impl_->hwnd) {
      DestroyWindow(impl_->hwnd);
    }
    if (impl_->bold) {
      DeleteObject(impl_->bold);
    }
    if (impl_->font) {
      DeleteObject(impl_->font);
    }
  }

  void main_window_t::show() {
    auto &w = *impl_;
    if (w.hwnd) {
      if (IsIconic(w.hwnd)) {
        ShowWindow(w.hwnd, SW_RESTORE);
      }
      SetForegroundWindow(w.hwnd);
      return;
    }
    if (!w.registered || !w.create()) {
      // The request was already accepted, so fall back to the web UI rather than doing nothing.
      std::thread([]() {
        launch_ui();
      }).detach();
    }
  }

  void main_window_t::tick() {
    auto &w = *impl_;
    if (!w.hwnd) {
      return;
    }
    ++w.ticks;
    w.refresh_devices(false);
    if (w.ticks % name_refresh_ticks == 0) {
      w.saved_name = read_config_value(device_name_key);
      w.update_name_hint();
    }
    if (w.ticks % address_refresh_ticks == 0) {
      w.refresh_addresses(false);
    }
    if (w.copied_ticks > 0 && --w.copied_ticks == 0) {
      set_text(w.copy, "复制地址");
    }
  }
}  // namespace office
#endif

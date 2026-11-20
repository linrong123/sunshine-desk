/**
 * @file office/windows/main_window.h
 * @brief Windows Sunshine Desk main window and the device actions shared with the tray.
 *
 * Internal to the Windows build. The window is created and used only on the host prompt thread
 * started by @ref office::authorization_ui_t; other threads ask for it with
 * @ref office::open_main_window, which posts @ref office::open_main_window_message to that thread.
 * The header avoids `<windows.h>` so it can be included before the Winsock headers.
 */
#pragma once
#ifdef _WIN32

  // standard includes
  #include <memory>
  #include <string>

namespace office {
  inline constexpr unsigned int open_main_window_message = 0x8000 + 1;  ///< `WM_APP + 1`, posted to the prompt thread to open the main window.

  /**
   * @brief Enable or disable a paired device on a worker thread.
   * @param uuid Paired client identifier.
   * @param enabled `true` to allow the device to connect again.
   */
  void request_client_access(std::string uuid, bool enabled);

  /**
   * @brief Ask the host user to confirm, then delete a paired device, on a worker thread.
   * @param uuid Paired client identifier.
   * @param name Device name shown in the confirmation box.
   */
  void request_delete_client(std::string uuid, std::string name);

  /**
   * @brief Turn per-connection host approval on or off and persist it, on a worker thread.
   * @param enabled New value of `desk_confirm_connections`.
   */
  void request_confirm_connections(bool enabled);

  /**
   * @brief The Sunshine Desk main window.
   *
   * Single instance: @ref show creates the window or brings the existing one to the front. Every
   * member function must be called on the prompt thread. Blocking work (confirmation boxes,
   * configuration writes, the service restart, opening the browser) runs on worker threads.
   */
  class main_window_t {
  public:
    /**
     * @brief Prepare fonts and the window class; no window is created yet.
     */
    main_window_t();

    /**
     * @brief Destroy the window if it is open and release the fonts.
     */
    ~main_window_t();

    main_window_t(const main_window_t &) = delete;
    main_window_t &operator=(const main_window_t &) = delete;

    /**
     * @brief Open the window, or restore it and bring it to the front.
     */
    void show();

    /**
     * @brief Refresh the window contents; does nothing while the window is closed.
     */
    void tick();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Window state.
  };
}  // namespace office
#endif

/**
 * @file office/tray_devices.h
 * @brief Tray entries for paired-device management and the per-connection approval switch.
 *
 * Only Windows builds with a tray use these entries. Menu storage is a namespace-scope array so the
 * upstream static tray definition can reference it; it is rewritten only while the caller holds the
 * tray state mutex, and the callbacks never take that mutex.
 */
#pragma once

#if defined(_WIN32) && defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
  // standard includes
  #include <cstddef>
  #include <string>
  #include <vector>

  // lib includes
  #include <tray.h>

namespace office {
  inline constexpr std::size_t tray_device_slots = 16;  ///< Paired devices listed in the tray; the main window lists all of them.
  inline constexpr std::size_t tray_devices_menu_size = tray_device_slots + 2;  ///< Device entries, an overflow or empty note, and the terminator.

  /**
   * @brief Paired device listed in the tray.
   */
  struct tray_device_t {
    std::string uuid;  ///< Paired client identifier.
    std::string name;  ///< Paired device name.
    bool enabled {true};  ///< Whether the client may connect.

    /**
     * @brief Compare every field.
     * @param other Device to compare with.
     * @return `true` if both devices are identical.
     */
    bool operator==(const tray_device_t &other) const = default;
  };

  extern struct tray_menu tray_devices_menu[tray_devices_menu_size];  ///< Submenu storage of the "已授权设备" tray entry.

  /**
   * @brief Publish the paired-device list for the next @ref rebuild_tray_devices.
   * @param devices Paired devices in display order.
   */
  void set_tray_devices(std::vector<tray_device_t> devices);

  /**
   * @brief Rewrite @ref tray_devices_menu from the published device list.
   *
   * The caller holds the tray state mutex and calls `tray_update` afterwards.
   */
  void rebuild_tray_devices();

  /**
   * @brief Tray callback that toggles per-connection host approval and persists the setting.
   * @param item Clicked menu item.
   */
  void tray_confirm_cb(struct tray_menu *item);

  /**
   * @brief Tray left-click callback that opens the Sunshine Desk main window.
   *
   * Runs on the tray thread and only posts a request to the prompt thread; falls back to the web
   * UI when the main window is unavailable. The right-click menu is unaffected.
   *
   * @param tray Clicked tray.
   */
  void tray_open_main_window_cb(struct tray *tray);
}  // namespace office
#endif

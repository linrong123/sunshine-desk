/**
 * @file office/authorization_ui.h
 * @brief Host prompts for pairing and connection requests, and the tray device list.
 *
 * @ref office::prompt_manager_t holds the platform-independent prompt rules so they can be
 * tested without a desktop. The Windows implementation of @ref office::authorization_ui_t
 * lives in `office/windows/authorization_ui.cpp`; other platforms get a no-op stub.
 */
#pragma once

// standard includes
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace office {
  /**
   * @brief Kind of host prompt.
   */
  enum class prompt_kind_e {
    pairing,  ///< A new device wants to pair; the host user enters the PIN shown on the device.
    connect,  ///< A paired device wants to start or resume a stream.
  };

  /**
   * @brief Request shown in one host prompt window.
   */
  struct prompt_t {
    prompt_kind_e kind {prompt_kind_e::pairing};  ///< Prompt kind.
    std::string id;  ///< Pairing identifier or connection request identifier.
    std::string name;  ///< Device name reported by the client.
    std::string address;  ///< Source address of the request.
  };

  /**
   * @brief Platform window operations used by @ref prompt_manager_t.
   */
  class prompt_backend_t {
  public:
    virtual ~prompt_backend_t() = default;

    /**
     * @brief Open a window for a request.
     * @param prompt Request to show.
     */
    virtual void show(const prompt_t &prompt) = 0;

    /**
     * @brief Close the window of a request.
     * @param kind Prompt kind.
     * @param id Request identifier.
     */
    virtual void close(prompt_kind_e kind, const std::string &id) = 0;
  };

  /**
   * @brief Operations performed when the host user answers a prompt.
   */
  struct prompt_actions_t {
    std::function<void(const std::string &id, const std::string &pin, const std::string &name)> pair;  ///< Complete pairing with a PIN.
    std::function<void(const std::string &id)> reject_pair;  ///< Cancel a pairing request.
    std::function<void(const std::string &id, bool allow)> answer_connect;  ///< Approve or deny a connection request.
  };

  /**
   * @brief Decide which requests get a window and dispatch the host user's answers.
   *
   * At most @ref max_open windows exist per kind. A request that arrives while the limit is reached
   * never gets a window; pairing requests remain available from the tray notification and the web
   * PIN page. A window closes when its request disappears (completed, cancelled, or expired) or when
   * it is answered. Not thread-safe: the owning UI thread makes every call.
   */
  class prompt_manager_t {
  public:
    static constexpr std::size_t max_open = 3;  ///< Window limit per prompt kind.

    /**
     * @brief Create a manager.
     * @param backend Window operations.
     * @param actions Answer handlers.
     */
    prompt_manager_t(prompt_backend_t &backend, prompt_actions_t actions);

    /**
     * @brief Reconcile windows of one kind with the requests that are currently pending.
     * @param kind Prompt kind of every element in `current`.
     * @param current Pending requests, oldest first.
     */
    void sync(prompt_kind_e kind, const std::vector<prompt_t> &current);

    /**
     * @brief Accept a request and close its window.
     * @param kind Prompt kind.
     * @param id Request identifier.
     * @param pin Four-digit PIN for pairing; ignored for connection requests.
     * @return `false` if no window is open for the request or the pairing PIN is invalid.
     */
    bool allow(prompt_kind_e kind, const std::string &id, std::string_view pin = {});

    /**
     * @brief Reject a request and close its window.
     * @param kind Prompt kind.
     * @param id Request identifier.
     * @return `false` if no window is open for the request.
     */
    bool deny(prompt_kind_e kind, const std::string &id);

    /**
     * @brief Count open windows of one kind.
     * @param kind Prompt kind.
     * @return Number of open windows.
     */
    std::size_t open_count(prompt_kind_e kind) const;

    /**
     * @brief Close every window without answering, for shutdown.
     */
    void close_all();

  private:
    using key_t = std::pair<prompt_kind_e, std::string>;  ///< Window key.

    /**
     * @brief Remove an open window entry and close it.
     * @param key Window key.
     * @return Request of the closed window, or no value if it was not open.
     */
    std::optional<prompt_t> take(const key_t &key);

    prompt_backend_t &backend_;  ///< Window operations.
    prompt_actions_t actions_;  ///< Answer handlers.
    std::map<key_t, prompt_t> open_;  ///< Requests that currently have a window.
    std::set<key_t> seen_;  ///< Pending requests already shown, answered, or skipped because of the limit.
  };

  /**
   * @brief Whether a pairing PIN has the four ASCII digits Moonlight displays.
   * @param pin Text entered by the host user.
   * @return `true` for exactly four ASCII digits.
   */
  bool is_prompt_pin(std::string_view pin);

  /**
   * @brief Owner of the host prompt thread and the tray device list.
   *
   * On Windows the constructor starts a UI thread that shows prompts and keeps the tray
   * "已授权设备" submenu current; the destructor closes every window and joins the thread.
   * Elsewhere it does nothing.
   */
  class authorization_ui_t {
  public:
    /**
     * @brief Start the host prompt thread where supported.
     */
    authorization_ui_t();

    /**
     * @brief Stop the host prompt thread.
     */
    ~authorization_ui_t();

    authorization_ui_t(const authorization_ui_t &) = delete;
    authorization_ui_t &operator=(const authorization_ui_t &) = delete;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Platform state; null where unsupported.
  };
}  // namespace office

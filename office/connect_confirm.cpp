/**
 * @file office/connect_confirm.cpp
 * @brief Per-connection host approval state machine for paired clients.
 */
// standard includes
#include <algorithm>
#include <utility>

// local includes
#include "office/connect_confirm.h"

namespace office {
  std::string_view to_string(const connect_state_e state) {
    using enum connect_state_e;
    switch (state) {
      case not_required:
        return "not_required";
      case pending:
        return "pending";
      case approved:
        return "approved";
      case denied:
        return "denied";
      case expired:
        return "expired";
      case none:
        break;
    }
    return "none";
  }

  connect_confirm_t::connect_confirm_t(now_fn_t now):
      now_ {std::move(now)} {
  }

  void connect_confirm_t::prune(const clock_t::time_point now) {
    for (auto it = requests_.begin(); it != requests_.end();) {
      auto &request = it->second;
      if (request.deadline > now) {
        ++it;
        continue;
      }
      if (request.state == connect_state_e::pending) {
        request.state = connect_state_e::expired;
        request.deadline += result_retention;
        if (request.deadline > now) {
          ++it;
          continue;
        }
      }
      it = requests_.erase(it);
    }
    std::erase_if(grace_, [now](const auto &entry) {
      return entry.second <= now;
    });
    std::erase_if(active_, [now](const auto &entry) {
      return entry.second && *entry.second <= now;
    });
  }

  connect_start_t connect_confirm_t::start(const std::string &cert, const std::string &name, const std::string &address) {
    const std::scoped_lock lock {mutex_};
    const auto now = now_();
    prune(now);

    const auto existing = requests_.find(cert);
    if (existing != requests_.end() && (existing->second.state == connect_state_e::pending || existing->second.state == connect_state_e::approved)) {
      return {existing->second.state};
    }
    if (active_.contains(cert) || grace_.contains(cert)) {
      return {connect_state_e::approved};
    }
    if (existing != requests_.end()) {
      // A retained denied or expired result is a cooldown: no new prompt until it lapses.
      return {existing->second.state};
    }

    const auto waiting = std::ranges::count(requests_, connect_state_e::pending, [](const auto &entry) {
      return entry.second.state;
    });
    if (static_cast<std::size_t>(waiting) >= max_pending) {
      return {connect_state_e::none, true};
    }

    requests_.insert_or_assign(cert, request_t {connect_state_e::pending, next_id_++, name, address, now + pending_lifetime});
    return {connect_state_e::pending};
  }

  connect_state_e connect_confirm_t::status(const std::string &cert) {
    const std::scoped_lock lock {mutex_};
    prune(now_());
    if (const auto it = requests_.find(cert); it != requests_.end()) {
      return it->second.state;
    }
    return active_.contains(cert) || grace_.contains(cert) ? connect_state_e::approved : connect_state_e::none;
  }

  connect_state_e connect_confirm_t::cancel(const std::string &cert) {
    const std::scoped_lock lock {mutex_};
    prune(now_());
    const auto it = requests_.find(cert);
    if (it == requests_.end()) {
      return connect_state_e::none;
    }
    if (it->second.state == connect_state_e::pending || it->second.state == connect_state_e::approved) {
      requests_.erase(it);
      return connect_state_e::none;
    }
    // Cancelling must not clear the cooldown of a denied or expired result.
    return it->second.state;
  }

  bool connect_confirm_t::resolve(const std::uint64_t id, const bool allow) {
    const std::scoped_lock lock {mutex_};
    const auto now = now_();
    prune(now);
    const auto it = std::ranges::find_if(requests_, [id](const auto &entry) {
      return entry.second.id == id && entry.second.state == connect_state_e::pending;
    });
    if (it == requests_.end()) {
      return false;
    }
    it->second.state = allow ? connect_state_e::approved : connect_state_e::denied;
    it->second.deadline = now + (allow ? permit_lifetime : result_retention);
    return true;
  }

  bool connect_confirm_t::admit(const std::string &cert) {
    const std::scoped_lock lock {mutex_};
    const auto now = now_();
    prune(now);
    if (active_.contains(cert)) {
      return true;
    }
    if (grace_.erase(cert) > 0) {
      active_.insert_or_assign(cert, now + permit_lifetime);
      return true;
    }
    const auto it = requests_.find(cert);
    if (it == requests_.end() || it->second.state != connect_state_e::approved) {
      return false;
    }
    requests_.erase(it);
    active_.insert_or_assign(cert, now + permit_lifetime);
    return true;
  }

  void connect_confirm_t::stream_started(const std::string &cert) {
    const std::scoped_lock lock {mutex_};
    if (const auto it = active_.find(cert); it != active_.end()) {
      it->second.reset();
    }
  }

  void connect_confirm_t::stream_stopped(const std::string &cert) {
    const std::scoped_lock lock {mutex_};
    if (active_.erase(cert) > 0) {
      grace_.insert_or_assign(cert, now_() + grace_lifetime);
    }
  }

  void connect_confirm_t::quit(const std::string &cert) {
    const std::scoped_lock lock {mutex_};
    active_.erase(cert);
    grace_.erase(cert);
  }

  void connect_confirm_t::revoke(const std::string &cert) {
    const std::scoped_lock lock {mutex_};
    active_.erase(cert);
    grace_.erase(cert);
    requests_.erase(cert);
  }

  std::vector<connect_prompt_t> connect_confirm_t::pending() {
    const std::scoped_lock lock {mutex_};
    prune(now_());
    std::vector<connect_prompt_t> result;
    for (const auto &[_, request] : requests_) {
      if (request.state == connect_state_e::pending) {
        result.push_back({request.id, request.name, request.address});
      }
    }
    std::ranges::sort(result, {}, &connect_prompt_t::id);
    return result;
  }

  void connect_confirm_t::clear() {
    const std::scoped_lock lock {mutex_};
    requests_.clear();
    grace_.clear();
    active_.clear();
  }
}  // namespace office

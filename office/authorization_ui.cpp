/**
 * @file office/authorization_ui.cpp
 * @brief Platform-independent host prompt rules, plus the no-op prompt owner for non-Windows builds.
 */
// standard includes
#include <algorithm>
#include <set>
#include <utility>

// local includes
#include "office/authorization_ui.h"

namespace office {
  bool is_prompt_pin(const std::string_view pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](const char c) {
             return c >= '0' && c <= '9';
           });
  }

  prompt_manager_t::prompt_manager_t(prompt_backend_t &backend, prompt_actions_t actions):
      backend_ {backend},
      actions_ {std::move(actions)} {
  }

  void prompt_manager_t::sync(const prompt_kind_e kind, const std::vector<prompt_t> &current) {
    std::set<std::string, std::less<>> ids;
    for (const auto &prompt : current) {
      ids.insert(prompt.id);
    }

    for (auto it = open_.begin(); it != open_.end();) {
      if (it->first.first == kind && !ids.contains(it->first.second)) {
        backend_.close(kind, it->first.second);
        it = open_.erase(it);
      } else {
        ++it;
      }
    }
    std::erase_if(seen_, [&](const key_t &key) {
      return key.first == kind && !ids.contains(key.second);
    });

    for (const auto &prompt : current) {
      key_t key {kind, prompt.id};
      if (!seen_.insert(key).second) {
        continue;
      }
      if (open_count(kind) >= max_open) {
        continue;
      }
      auto shown = prompt;
      shown.kind = kind;
      open_.emplace(std::move(key), shown);
      backend_.show(shown);
    }
  }

  std::optional<prompt_t> prompt_manager_t::take(const key_t &key) {
    const auto it = open_.find(key);
    if (it == open_.end()) {
      return std::nullopt;
    }
    auto prompt = std::move(it->second);
    open_.erase(it);
    backend_.close(key.first, key.second);
    return prompt;
  }

  bool prompt_manager_t::allow(const prompt_kind_e kind, const std::string &id, const std::string_view pin) {
    const key_t key {kind, id};
    if (!open_.contains(key) || (kind == prompt_kind_e::pairing && !is_prompt_pin(pin))) {
      return false;
    }
    const auto prompt = take(key);
    if (kind == prompt_kind_e::pairing) {
      if (actions_.pair) {
        actions_.pair(prompt->id, std::string {pin}, prompt->name);
      }
    } else if (actions_.answer_connect) {
      actions_.answer_connect(prompt->id, true);
    }
    return true;
  }

  bool prompt_manager_t::deny(const prompt_kind_e kind, const std::string &id) {
    const auto prompt = take({kind, id});
    if (!prompt) {
      return false;
    }
    if (kind == prompt_kind_e::pairing) {
      if (actions_.reject_pair) {
        actions_.reject_pair(prompt->id);
      }
    } else if (actions_.answer_connect) {
      actions_.answer_connect(prompt->id, false);
    }
    return true;
  }

  std::size_t prompt_manager_t::open_count(const prompt_kind_e kind) const {
    return static_cast<std::size_t>(std::ranges::count_if(open_, [kind](const auto &entry) {
      return entry.first.first == kind;
    }));
  }

  void prompt_manager_t::close_all() {
    for (const auto &[key, _] : open_) {
      backend_.close(key.first, key.second);
    }
    open_.clear();
  }

#ifndef _WIN32
  /**
   * @brief Placeholder platform state; host prompts exist only on Windows.
   */
  struct authorization_ui_t::impl_t {};

  authorization_ui_t::authorization_ui_t() = default;

  authorization_ui_t::~authorization_ui_t() = default;
#endif
}  // namespace office

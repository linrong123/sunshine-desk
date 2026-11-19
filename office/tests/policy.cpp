/**
 * @file office/tests/policy.cpp
 * @brief Standalone Office cursor-policy checks without platform dependencies.
 */
#include "office/policy.h"

#include <atomic>
#include <cassert>
#include <map>
#include <string>

/**
 * @brief Exercise accepted, rejected, repeated, and duplicate cursor requests.
 * @return Zero when all checks pass.
 */
int main() {
  using query_t = std::multimap<std::string, std::string>;
  assert(!office::cursor_visibility(query_t {}).has_value());
  assert(!office::cursor_visibility(query_t {{"visible", "true"}}).has_value());
  assert(!office::cursor_visibility(query_t {{"visible", ""}}).has_value());
  assert(!office::cursor_visibility(query_t {{"visible", "0"}, {"visible", "1"}}).has_value());
  std::atomic_bool cursor {true};
  for (int i = 0; i < 2; ++i) {
    auto hide = office::cursor_visibility(query_t {{"visible", "0"}});
    assert(hide.has_value() && !*hide);
    cursor.store(*hide);
    assert(!cursor.load());
  }
  auto show = office::cursor_visibility(query_t {{"visible", "1"}, {"uniqueid", "test"}});
  assert(show.has_value() && *show);
  cursor.store(*show);
  assert(cursor.load());
}

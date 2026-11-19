/**
 * @file tests/display_scale_test_utils.h
 * @brief Synthetic display scaling backend; never invokes Windows display APIs.
 */
#pragma once

#include "office/display_scale.h"

/** @brief Controllable fake for policy and production TLS authorization tests. */
class FakeDisplayScaleBackend final: public office::display_scale_backend {
public:
  office::display_scale_snapshot state {"synthetic-source-A", {100, 100, {100, 125, 150, 175, 200}}};  ///< Synthetic system state only.
  int reads {};  ///< Number of backend queries.
  int writes {};  ///< Number of accepted synthetic writes.
  int read_status {200};  ///< Simulated query outcome.
  int write_status {200};  ///< Simulated operating-system write outcome.
  bool apply {true};  ///< Whether a successful synthetic write changes the returned scale.
  bool change_before_write {};  ///< Simulate an external target replacement inside the OS boundary.
  bool change_after_write {};  ///< Simulate a changed topology before readback.
  bool fail_readback {};  ///< Simulate loss of query access immediately after a write.

  /** @copydoc office::display_scale_backend::read */
  office::display_scale_result read() override {
    ++reads;
    return {read_status, read_status == 200 ? "" : "Synthetic query failure", state, {}};
  }

  /** @copydoc office::display_scale_backend::write */
  office::display_scale_result write(const office::display_scale_snapshot &expected, int scale) override {
    if (change_before_write) {
      state.identity = "synthetic-source-B";
    }
    if (expected.identity != state.identity) {
      return {409, "Synthetic target changed", {}, {}};
    }
    ++writes;
    if (write_status != 200) {
      return {write_status, "Synthetic write failure", {}, {}};
    }
    if (apply) {
      state.values.current = scale;
    }
    if (change_after_write) {
      state.identity = "synthetic-source-B";
    }
    if (fail_readback) {
      read_status = 503;
    }
    return {200, {}, {}, {}};
  }
};

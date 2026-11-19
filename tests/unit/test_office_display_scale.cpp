/**
 * @file tests/unit/test_office_display_scale.cpp
 * @brief Strict scale validation, opaque target binding, and verified-write tests.
 */
#include "../display_scale_test_utils.h"

#include <gtest/gtest.h>
#include <limits>
#include <memory>

/** @brief Isolate every test from production display APIs and previous target tokens. */
class OfficeDisplayScaleTest: public testing::Test {
protected:
  /** @brief Install a synthetic backend before any request is issued. */
  void SetUp() override {
    auto backend = std::make_unique<FakeDisplayScaleBackend>();
    fake = backend.get();
    office::set_display_scale_backend_for_test(std::move(backend));
  }

  /** @brief Restore lazy platform selection and invalidate all synthetic tokens. */
  void TearDown() override {
    office::set_display_scale_backend_for_test(nullptr);
  }

  /** @brief Read the synthetic system. @return Protocol result with an opaque token. */
  office::display_scale_result read() {
    return office::request_display_scale(false, {});
  }

  /**
   * @brief Submit a synthetic target-bound write.
   * @param scale Canonical percentage string.
   * @param token Token returned by the previous read.
   * @return Verified write outcome.
   */
  office::display_scale_result write(const std::string &scale, const std::string &token) {
    return office::request_display_scale(true, {{"scale", scale}, {"displayToken", token}});
  }

  FakeDisplayScaleBackend *fake {};  ///< Non-owning pointer to the installed synthetic backend.
};

/** @brief Verify strict bounds and known scale table. */
TEST(OfficeDisplayScaleIndices, StrictBoundsAndKnownScaleTable) {
  const auto result = office::decode_display_scale(-3, 1, 8);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->current, 200);
  EXPECT_EQ(result->recommended, 175);
  EXPECT_EQ(result->supported, (std::vector<int> {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500}));
  EXPECT_EQ(office::decode_display_scale(-3, -3, 8)->current, 100);
  EXPECT_EQ(office::decode_display_scale(-3, 8, 8)->current, 500);
  EXPECT_EQ(office::decode_display_scale(0, 0, 0)->current, 100);
  EXPECT_FALSE(office::decode_display_scale(1, 1, 1));
  EXPECT_FALSE(office::decode_display_scale(-1, -2, 2));
  EXPECT_FALSE(office::decode_display_scale(-1, 3, 2));
  EXPECT_FALSE(office::decode_display_scale(-1, 0, -1));
  EXPECT_FALSE(office::decode_display_scale(-12, 0, 0));
  EXPECT_FALSE(office::decode_display_scale(-3, 0, 9));
  EXPECT_FALSE(office::decode_display_scale(std::numeric_limits<std::int32_t>::min(), 0, std::numeric_limits<std::int32_t>::max()));
  EXPECT_FALSE(office::decode_display_scale(0, std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max()));
}

/** @brief Verify get reads actual state without writing. */
TEST_F(OfficeDisplayScaleTest, GetReadsActualStateWithoutWriting) {
  fake->state.values.current = 150;
  const auto result = read();
  ASSERT_EQ(result.status, 200);
  ASSERT_TRUE(result.snapshot);
  EXPECT_EQ(result.snapshot->values.current, 150);
  EXPECT_EQ(result.token.size(), 32u);
  EXPECT_EQ(result.token.find("synthetic"), std::string::npos);
  EXPECT_EQ(read().token, result.token);
  EXPECT_EQ(fake->writes, 0);
}

/** @brief Verify invalid parameters never reach backend. */
TEST_F(OfficeDisplayScaleTest, InvalidParametersNeverReachBackend) {
  EXPECT_EQ(office::request_display_scale(false, {{"scale", "150"}}).status, 400);
  EXPECT_EQ(office::request_display_scale(true, {}).status, 400);
  for (const auto *value : {"", "150.0", "0150", "+150", " 150", "150 ", "150x", "999999999999999"}) {
    EXPECT_EQ(write(value, "token").status, 400) << value;
  }
  EXPECT_EQ(write("150", "").status, 400);
  EXPECT_EQ(office::request_display_scale(true, {{"scale", "150"}, {"scale", "150"}}).status, 400);
  EXPECT_EQ(office::request_display_scale(true, {{"displayToken", "a"}, {"displayToken", "b"}}).status, 400);
  EXPECT_EQ(office::request_display_scale(true, {{"scale", "150"}, {"other", "a"}}).status, 400);
  EXPECT_EQ(office::request_display_scale(true, {{"scale", "150"}, {"displayToken", "a"}, {"other", "b"}}).status, 400);
  EXPECT_EQ(fake->reads, 0);
  EXPECT_EQ(fake->writes, 0);
}

/** @brief Verify writes exact scale and confirms readback. */
TEST_F(OfficeDisplayScaleTest, WritesExactScaleAndConfirmsReadback) {
  const auto before = read();
  const auto after = write("150", before.token);
  ASSERT_EQ(after.status, 200);
  ASSERT_TRUE(after.snapshot);
  EXPECT_EQ(after.snapshot->values.current, 150);
  EXPECT_EQ(after.token, before.token);
  EXPECT_EQ(fake->writes, 1);
  EXPECT_EQ(write("150", before.token).status, 200);
  EXPECT_EQ(fake->writes, 1);
  EXPECT_EQ(fake->reads, 5);
}

/** @brief Verify unsupported scale is not clamped. */
TEST_F(OfficeDisplayScaleTest, UnsupportedScaleIsNotClamped) {
  const auto token = read().token;
  for (const auto *value : {"-1", "0", "149", "225", "600"}) {
    EXPECT_EQ(write(value, token).status, 422);
  }
  EXPECT_EQ(fake->state.values.current, 100);
  EXPECT_EQ(fake->writes, 0);
}

/** @brief Verify target replacement invalidates old token. */
TEST_F(OfficeDisplayScaleTest, TargetReplacementInvalidatesOldToken) {
  const auto token = read().token;
  fake->state.identity = "synthetic-source-B";
  EXPECT_EQ(write("150", token).status, 409);
  const auto new_token = read().token;
  EXPECT_NE(new_token, token);
  EXPECT_EQ(fake->writes, 0);
  EXPECT_EQ(write("150", new_token).status, 200);
}

/** @brief Verify platform revalidation prevents topology race. */
TEST_F(OfficeDisplayScaleTest, PlatformRevalidationPreventsTopologyRace) {
  const auto token = read().token;
  fake->change_before_write = true;
  EXPECT_EQ(write("150", token).status, 409);
  EXPECT_EQ(fake->writes, 0);
}

/** @brief Verify write failure does not report success or invent state. */
TEST_F(OfficeDisplayScaleTest, WriteFailureDoesNotReportSuccessOrInventState) {
  const auto token = read().token;
  fake->write_status = 503;
  const auto failed = write("150", token);
  EXPECT_EQ(failed.status, 503);
  EXPECT_FALSE(failed.snapshot);
  EXPECT_TRUE(failed.token.empty());
  EXPECT_EQ(fake->state.values.current, 100);
}

/** @brief Verify successful api without applied scale is failure. */
TEST_F(OfficeDisplayScaleTest, SuccessfulApiWithoutAppliedScaleIsFailure) {
  const auto token = read().token;
  fake->apply = false;
  const auto failed = write("150", token);
  EXPECT_EQ(failed.status, 409);
  EXPECT_FALSE(failed.snapshot);
}

/** @brief Verify changed target after write is failure. */
TEST_F(OfficeDisplayScaleTest, ChangedTargetAfterWriteIsFailure) {
  const auto token = read().token;
  fake->change_after_write = true;
  EXPECT_EQ(write("150", token).status, 409);
}

/** @brief Verify query failure after write is failure. */
TEST_F(OfficeDisplayScaleTest, QueryFailureAfterWriteIsFailure) {
  const auto token = read().token;
  fake->fail_readback = true;
  EXPECT_EQ(write("150", token).status, 503);
}

/** @brief Verify failed query invalidates token and does not leak backend state. */
TEST_F(OfficeDisplayScaleTest, FailedQueryInvalidatesTokenAndDoesNotLeakBackendState) {
  const auto token = read().token;
  fake->read_status = 503;
  const auto failed = read();
  EXPECT_EQ(failed.status, 503);
  EXPECT_FALSE(failed.snapshot);
  EXPECT_TRUE(failed.token.empty());
  fake->read_status = 200;
  EXPECT_NE(read().token, token);
  EXPECT_EQ(write("150", token).status, 409);
}

/** @brief Verify malformed or custom backend values are rejected. */
TEST_F(OfficeDisplayScaleTest, MalformedOrCustomBackendValuesAreRejected) {
  const auto original = fake->state;
  for (const auto &values : std::vector<office::display_scale_values> {
         {123, 100, {100, 125}},
         {100, 123, {100, 125}},
         {100, 100, {}},
         {100, 100, {125, 100}},
         {100, 100, {100, 100}},
         {100, 100, {100, 123}}
       }) {
    fake->state.values = values;
    EXPECT_EQ(read().status, 503);
  }
  fake->state = original;
  fake->state.identity.clear();
  EXPECT_EQ(read().status, 503);
}

#ifndef _WIN32
/** @brief Verify unsupported platform returns explicit failure. */
TEST(OfficeDisplayScalePlatform, UnsupportedPlatformReturnsExplicitFailure) {
  office::set_display_scale_backend_for_test(nullptr);
  const auto result = office::request_display_scale(false, {});
  EXPECT_EQ(result.status, 501);
  EXPECT_FALSE(result.snapshot);
  EXPECT_TRUE(result.token.empty());
}
#endif

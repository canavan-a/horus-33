#include "clip_policy.h"

#include <catch2/catch_test_macros.hpp>

using namespace capture_eye;

TEST_CASE("no clip starts while nobody is ever detected") {
  ClipPolicy policy;
  for (int i = 0; i < 10; ++i) {
    const auto decision = policy.update(false);
    CHECK_FALSE(decision.start_clip);
    CHECK_FALSE(decision.stop_clip);
    CHECK_FALSE(decision.recording);
  }
}

TEST_CASE("a clip starts on the first present tick") {
  ClipPolicy policy;
  const auto decision = policy.update(true);
  CHECK(decision.start_clip);
  CHECK_FALSE(decision.stop_clip);
  CHECK(decision.recording);
}

TEST_CASE("recording continues without re-starting on subsequent present ticks") {
  ClipPolicy policy;
  REQUIRE(policy.update(true).start_clip);
  for (int i = 0; i < 5; ++i) {
    const auto decision = policy.update(true);
    CHECK_FALSE(decision.start_clip);
    CHECK_FALSE(decision.stop_clip);
    CHECK(decision.recording);
  }
}

TEST_CASE("a clip does not stop before stop_after_ticks consecutive absent ticks") {
  ClipPolicy policy;
  policy.stop_after_ticks = 5;
  REQUIRE(policy.update(true).start_clip);

  for (int i = 0; i < 4; ++i) {
    const auto decision = policy.update(false);
    CHECK_FALSE(decision.stop_clip);
    CHECK(decision.recording);
  }
}

TEST_CASE("a clip stops exactly at the Nth consecutive absent tick") {
  ClipPolicy policy;
  policy.stop_after_ticks = 5;
  REQUIRE(policy.update(true).start_clip);

  for (int i = 0; i < 4; ++i) {
    CHECK_FALSE(policy.update(false).stop_clip);
  }
  const auto decision = policy.update(false);
  CHECK(decision.stop_clip);
  CHECK_FALSE(decision.recording);
}

TEST_CASE("a present tick mid-countdown resets it to the full length") {
  ClipPolicy policy;
  policy.stop_after_ticks = 5;
  REQUIRE(policy.update(true).start_clip);

  for (int i = 0; i < 4; ++i) (void)policy.update(false);  // one tick away from stopping
  REQUIRE_FALSE(policy.update(true).start_clip);      // still recording, not a new clip

  // The countdown must have reset: another 4 absent ticks should not stop it.
  for (int i = 0; i < 4; ++i) {
    CHECK_FALSE(policy.update(false).stop_clip);
  }
  CHECK(policy.update(false).stop_clip);
}

TEST_CASE("a new clip can start again after a stop") {
  ClipPolicy policy;
  policy.stop_after_ticks = 2;
  REQUIRE(policy.update(true).start_clip);
  (void)policy.update(false);
  REQUIRE(policy.update(false).stop_clip);

  const auto decision = policy.update(true);
  CHECK(decision.start_clip);
  CHECK(decision.recording);
}

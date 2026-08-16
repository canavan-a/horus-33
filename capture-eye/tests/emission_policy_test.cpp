#include "emission_policy.h"

#include <catch2/catch_test_macros.hpp>

using namespace capture_eye;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace {

const Clock::time_point kStart{};

TrackMessage target(float x = 0.1f) {
  TrackMessage m;
  m.x = x;
  m.c = 0.9f;
  return m;
}

} // namespace

TEST_CASE("nothing is sent before the first ever detection") {
  EmissionPolicy policy;
  for (int i = 0; i < 20; ++i) {
    const auto decision = policy.update(std::nullopt, kStart + milliseconds{i * 100});
    CHECK_FALSE(decision.message.has_value());
  }
}

TEST_CASE("a target is emitted immediately") {
  EmissionPolicy policy;
  const auto decision = policy.update(target(), kStart);
  REQUIRE(decision.message.has_value());
  CHECK_FALSE(decision.message->lost);
}

TEST_CASE("the rate cap throttles a fast producer") {
  EmissionPolicy policy;
  policy.max_hz = 10;  // one every 100ms

  CHECK(policy.update(target(), kStart).message.has_value());
  CHECK_FALSE(policy.update(target(), kStart + milliseconds{50}).message.has_value());
  CHECK(policy.update(target(), kStart + milliseconds{100}).message.has_value());
}

TEST_CASE("a single missed inference does not retire the target") {
  EmissionPolicy policy;
  policy.lost_grace = milliseconds{200};

  REQUIRE(policy.update(target(), kStart).message.has_value());
  const auto blip = policy.update(std::nullopt, kStart + milliseconds{40});
  CHECK_FALSE(blip.message.has_value());
  CHECK_FALSE(blip.forget_target);
}

TEST_CASE("lost fires once as soon as the grace period expires") {
  EmissionPolicy policy;
  policy.lost_grace = milliseconds{200};

  REQUIRE(policy.update(target(), kStart).message.has_value());

  const auto lost = policy.update(std::nullopt, kStart + milliseconds{250});
  REQUIRE(lost.message.has_value());
  CHECK(lost.message->lost);
  CHECK(lost.forget_target);  // the selector must drop its lock

  // Immediately after, it does not spam.
  CHECK_FALSE(policy.update(std::nullopt, kStart + milliseconds{260}).message.has_value());
}

TEST_CASE("lost repeats slowly while the target stays gone") {
  EmissionPolicy policy;
  policy.lost_grace = milliseconds{200};
  policy.lost_repeat_hz = 5;  // every 200ms

  REQUIRE(policy.update(target(), kStart).message.has_value());
  REQUIRE(policy.update(std::nullopt, kStart + milliseconds{250}).message.has_value());

  CHECK_FALSE(policy.update(std::nullopt, kStart + milliseconds{400}).message.has_value());

  const auto repeat = policy.update(std::nullopt, kStart + milliseconds{460});
  REQUIRE(repeat.message.has_value());
  CHECK(repeat.message->lost);
  // Only the first loss tells the selector to forget.
  CHECK_FALSE(repeat.forget_target);
}

TEST_CASE("reacquisition resumes normal emission") {
  EmissionPolicy policy;
  policy.lost_grace = milliseconds{200};

  REQUIRE(policy.update(target(), kStart).message.has_value());
  REQUIRE(policy.update(std::nullopt, kStart + milliseconds{250}).message->lost);

  const auto back = policy.update(target(0.5f), kStart + milliseconds{300});
  REQUIRE(back.message.has_value());
  CHECK_FALSE(back.message->lost);

  // And a later loss declares itself again rather than staying latched.
  CHECK_FALSE(policy.update(std::nullopt, kStart + milliseconds{400}).message.has_value());
  const auto lost_again = policy.update(std::nullopt, kStart + milliseconds{600});
  REQUIRE(lost_again.message.has_value());
  CHECK(lost_again.message->lost);
  CHECK(lost_again.forget_target);
}

TEST_CASE("an explicit lost target is treated as no target") {
  EmissionPolicy policy;
  policy.lost_grace = milliseconds{200};
  REQUIRE(policy.update(target(), kStart).message.has_value());

  const auto decision = policy.update(TrackMessage{.lost = true, .seq = std::nullopt}, kStart + milliseconds{250});
  REQUIRE(decision.message.has_value());
  CHECK(decision.message->lost);
}

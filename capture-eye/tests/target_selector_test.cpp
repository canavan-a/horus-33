#include "target_selector.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace capture_eye;

namespace {

Detection person(BoxF box, float confidence) {
  return Detection{.box = box, .confidence = confidence, .class_id = kPersonClass};
}

} // namespace

TEST_CASE("no detections means no target") {
  TargetSelector selector;
  CHECK_FALSE(selector.select({}).has_value());
}

TEST_CASE("largest_area picks the biggest box") {
  TargetSelector selector;
  selector.policy = TargetPolicy::largest_area;
  const std::vector<Detection> people{
      person(BoxF{0, 0, 10, 10}, 0.99f),
      person(BoxF{100, 100, 200, 200}, 0.40f),
  };
  const auto chosen = selector.select(people);
  REQUIRE(chosen.has_value());
  CHECK(chosen->box.x1 == 100.0f);
}

TEST_CASE("most_confident ignores size") {
  TargetSelector selector;
  selector.policy = TargetPolicy::most_confident;
  const std::vector<Detection> people{
      person(BoxF{0, 0, 10, 10}, 0.99f),
      person(BoxF{100, 100, 200, 200}, 0.40f),
  };
  const auto chosen = selector.select(people);
  REQUIRE(chosen.has_value());
  CHECK(chosen->confidence == 0.99f);
}

TEST_CASE("sticky keeps following the same person across frames") {
  TargetSelector selector;  // sticky_largest is the default

  const std::vector<Detection> first{person(BoxF{100, 100, 200, 300}, 0.9f)};
  const auto initial = selector.select(first);
  REQUIRE(initial.has_value());
  CHECK(initial->box.x1 == 100.0f);

  // A bigger person walks in. Sticky must not jump to them.
  const std::vector<Detection> second{
      person(BoxF{105, 100, 205, 300}, 0.9f),  // same person, moved slightly
      person(BoxF{400, 50, 700, 600}, 0.9f),   // larger newcomer
  };
  const auto kept = selector.select(second);
  REQUIRE(kept.has_value());
  CHECK(kept->box.x1 == 105.0f);
}

TEST_CASE("sticky falls back to largest when the locked target vanishes") {
  TargetSelector selector;

  const std::vector<Detection> first{person(BoxF{100, 100, 200, 300}, 0.9f)};
  REQUIRE(selector.select(first).has_value());

  // The tracked person is gone; only a distant, non-overlapping one remains.
  const std::vector<Detection> second{
      person(BoxF{800, 100, 900, 300}, 0.9f),
      person(BoxF{400, 50, 700, 600}, 0.5f),
  };
  const auto rechosen = selector.select(second);
  REQUIRE(rechosen.has_value());
  CHECK(rechosen->box.x1 == 400.0f);  // the larger of the two
}

TEST_CASE("a weak overlap does not count as the same person") {
  TargetSelector selector;
  selector.lock_iou = 0.5f;

  const std::vector<Detection> first{person(BoxF{0, 0, 100, 100}, 0.9f)};
  REQUIRE(selector.select(first).has_value());

  // Overlaps slightly, well under the lock threshold, and a bigger box exists.
  const std::vector<Detection> second{
      person(BoxF{90, 90, 190, 190}, 0.9f),
      person(BoxF{300, 0, 600, 400}, 0.9f),
  };
  const auto chosen = selector.select(second);
  REQUIRE(chosen.has_value());
  CHECK(chosen->box.x1 == 300.0f);
}

TEST_CASE("forget drops the lock so reacquisition starts clean") {
  TargetSelector selector;

  const std::vector<Detection> first{person(BoxF{100, 100, 200, 300}, 0.9f)};
  REQUIRE(selector.select(first).has_value());
  selector.forget();
  CHECK_FALSE(selector.previous.has_value());

  const std::vector<Detection> second{
      person(BoxF{105, 100, 205, 300}, 0.9f),
      person(BoxF{400, 50, 700, 600}, 0.9f),
  };
  const auto chosen = selector.select(second);
  REQUIRE(chosen.has_value());
  CHECK(chosen->box.x1 == 400.0f);  // no lock, so largest wins
}

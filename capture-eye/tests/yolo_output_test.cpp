#include "yolo_output.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace capture_eye;
using Catch::Approx;

namespace {

constexpr std::size_t kQueries = 8;
constexpr std::size_t kClasses = 80;

// The model emits raw logits, so tests must speak in logit space.
float logit(float probability) { return std::log(probability / (1.0f - probability)); }

struct Fixture {
  std::vector<float> logits = std::vector<float>(kQueries * kClasses, -20.0f);
  std::vector<float> boxes = std::vector<float>(kQueries * 4, 0.0f);

  void set(std::size_t query, int class_id, float probability, float cx, float cy, float w,
           float h) {
    logits[query * kClasses + static_cast<std::size_t>(class_id)] = logit(probability);
    boxes[query * 4 + 0] = cx;
    boxes[query * 4 + 1] = cy;
    boxes[query * 4 + 2] = w;
    boxes[query * 4 + 3] = h;
  }

  [[nodiscard]] std::vector<Detection> parse(float threshold = 0.35f,
                                             int class_id = kPersonClass) const {
    return parse_yolo_output(logits, boxes, kQueries, kClasses,
                             letterbox_transform(640, 640, 640), threshold, class_id);
  }
};

} // namespace

TEST_CASE("an all-background output yields nothing") {
  const Fixture fixture;
  CHECK(fixture.parse().empty());
}

TEST_CASE("raw logits are passed through a sigmoid") {
  Fixture fixture;
  fixture.set(0, kPersonClass, 0.92f, 0.5f, 0.5f, 0.2f, 0.4f);
  const auto found = fixture.parse();
  REQUIRE(found.size() == 1);
  // If the sigmoid were skipped, confidence would be the raw logit (~2.4).
  CHECK(found[0].confidence == Approx(0.92f).margin(0.01));
}

TEST_CASE("only the requested class is reported") {
  Fixture fixture;
  fixture.set(0, kPersonClass, 0.9f, 0.3f, 0.5f, 0.1f, 0.2f);
  fixture.set(1, 5, 0.95f, 0.7f, 0.5f, 0.3f, 0.3f);  // a bus, scoring higher

  const auto people = fixture.parse();
  REQUIRE(people.size() == 1);
  CHECK(people[0].class_id == kPersonClass);
  CHECK(people[0].confidence == Approx(0.9f).margin(0.01));

  const auto buses = fixture.parse(0.35f, 5);
  REQUIRE(buses.size() == 1);
  CHECK(buses[0].confidence == Approx(0.95f).margin(0.01));
}

// Classes are scored independently, so one query can plausibly carry a person
// score even when another class scores higher on the same query.
TEST_CASE("a query is judged on the requested class alone") {
  Fixture fixture;
  fixture.set(0, 5, 0.99f, 0.5f, 0.5f, 0.2f, 0.4f);
  fixture.logits[0 * kClasses + kPersonClass] = logit(0.80f);

  const auto people = fixture.parse();
  REQUIRE(people.size() == 1);
  CHECK(people[0].confidence == Approx(0.80f).margin(0.01));
}

TEST_CASE("the threshold excludes weak detections") {
  Fixture fixture;
  fixture.set(0, kPersonClass, 0.90f, 0.3f, 0.5f, 0.1f, 0.2f);
  fixture.set(1, kPersonClass, 0.40f, 0.6f, 0.5f, 0.1f, 0.2f);

  CHECK(fixture.parse(0.35f).size() == 2);
  CHECK(fixture.parse(0.50f).size() == 1);
  CHECK(fixture.parse(0.95f).empty());
}

// Queries are not guaranteed to be score-ordered, so parsing must not stop at
// the first sub-threshold row.
TEST_CASE("detections are found in later queries too") {
  Fixture fixture;
  fixture.set(kQueries - 1, kPersonClass, 0.88f, 0.5f, 0.5f, 0.2f, 0.4f);
  const auto found = fixture.parse();
  REQUIRE(found.size() == 1);
  CHECK(found[0].confidence == Approx(0.88f).margin(0.01));
}

TEST_CASE("degenerate boxes are dropped even when confident") {
  Fixture fixture;
  fixture.set(0, kPersonClass, 0.99f, 0.5f, 0.5f, 0.0f, 0.0f);
  CHECK(fixture.parse().empty());
}

TEST_CASE("boxes are converted to source pixels") {
  Fixture fixture;
  fixture.set(0, kPersonClass, 0.9f, 0.5f, 0.5f, 0.25f, 0.5f);
  const auto found = fixture.parse();
  REQUIRE(found.size() == 1);
  // 640x640 source with no padding: normalised maps straight to pixels.
  CHECK(found[0].box.center_x() == Approx(320.0f).margin(1.0));
  CHECK(found[0].box.width() == Approx(160.0f).margin(1.0));
  CHECK(found[0].box.height() == Approx(320.0f).margin(1.0));
}

TEST_CASE("an out-of-range class is refused rather than read out of bounds") {
  const Fixture fixture;
  CHECK(fixture.parse(0.35f, -1).empty());
  CHECK(fixture.parse(0.35f, 80).empty());
}

TEST_CASE("undersized tensors are refused rather than read out of bounds") {
  const std::vector<float> logits(10, 0.0f);
  const std::vector<float> boxes(10, 0.0f);
  CHECK(parse_yolo_output(logits, boxes, kQueries, kClasses,
                          letterbox_transform(640, 640, 640), 0.35f, kPersonClass)
            .empty());
}

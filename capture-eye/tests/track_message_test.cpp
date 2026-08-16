#include "track_message.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace capture_eye;
using Catch::Approx;

namespace {
constexpr int kW = 1280;
constexpr int kH = 720;
} // namespace

TEST_CASE("a centred box maps to the origin") {
  const auto m = track_from_box(BoxF{620, 340, 660, 380}, kW, kH, 0.9f);
  CHECK_FALSE(m.lost);
  CHECK(m.x == Approx(0.0f).margin(1e-6));
  CHECK(m.y == Approx(0.0f).margin(1e-6));
}

// The single most important sign convention in the project: the protocol's +y is
// the TOP of the frame, while pixel rows count downward. Inverting this steers
// the gimbal away from the target.
TEST_CASE("the Y axis is flipped relative to pixel rows") {
  const auto top = track_from_box(BoxF{600, 0, 680, 80}, kW, kH, 0.9f);
  CHECK(top.y > 0.0f);

  const auto bottom = track_from_box(BoxF{600, kH - 80, 680, kH}, kW, kH, 0.9f);
  CHECK(bottom.y < 0.0f);
}

TEST_CASE("the X axis is not flipped") {
  const auto right = track_from_box(BoxF{kW - 80, 340, kW, 380}, kW, kH, 0.9f);
  CHECK(right.x > 0.0f);

  const auto left = track_from_box(BoxF{0, 340, 80, 380}, kW, kH, 0.9f);
  CHECK(left.x < 0.0f);
}

TEST_CASE("corners reach the full range") {
  const auto top_right = track_from_box(BoxF{kW - 2, 0, kW, 2}, kW, kH, 1.0f);
  CHECK(top_right.x == Approx(1.0f).margin(0.01));
  CHECK(top_right.y == Approx(1.0f).margin(0.01));
}

TEST_CASE("size is expressed as a fraction of the frame") {
  const auto m = track_from_box(BoxF{0, 0, kW / 2.0f, kH / 4.0f}, kW, kH, 0.5f);
  CHECK(m.w == Approx(0.5f));
  CHECK(m.h == Approx(0.25f));
}

TEST_CASE("out-of-frame boxes are clamped, not passed through") {
  const auto m = track_from_box(BoxF{-500, -500, kW + 500.0f, kH + 500.0f}, kW, kH, 2.0f);
  CHECK(m.x >= -1.0f);
  CHECK(m.x <= 1.0f);
  CHECK(m.y >= -1.0f);
  CHECK(m.y <= 1.0f);
  CHECK(m.w <= 1.0f);
  CHECK(m.c <= 1.0f);
}

TEST_CASE("a non-finite box becomes lost rather than reaching the wire") {
  const auto nan_box = track_from_box(BoxF{std::nanf(""), 0, 10, 10}, kW, kH, 0.9f);
  CHECK(nan_box.lost);

  const auto inf_conf =
      track_from_box(BoxF{0, 0, 10, 10}, kW, kH, std::numeric_limits<float>::infinity());
  // Confidence is clamped before the finiteness check, so this stays a real target.
  CHECK_FALSE(inf_conf.lost);
  CHECK(inf_conf.c == Approx(1.0f));
}

TEST_CASE("a zero-sized frame is lost rather than a division by zero") {
  CHECK(track_from_box(BoxF{0, 0, 10, 10}, 0, 0, 0.9f).lost);
}

TEST_CASE("encoded lines match the protocol byte for byte") {
  TrackMessage m;
  m.x = -0.42f;
  m.y = 0.18f;
  m.w = 0.12f;
  m.h = 0.20f;
  m.c = 0.86f;
  CHECK(encode_track(m) == "{\"t\":\"track\",\"x\":-0.4200,\"y\":0.1800,\"w\":0.1200,"
                           "\"h\":0.2000,\"c\":0.8600}\n");

  CHECK(encode_track(TrackMessage{.lost = true, .seq = std::nullopt}) == "{\"t\":\"track\",\"lost\":true}\n");
}

// Omitting `c` makes the device default it to 1.0, silently bypassing its own
// min_conf gate (firmware/src/main.cpp:110).
TEST_CASE("confidence is always present, even at zero") {
  TrackMessage m;
  m.c = 0.0f;
  CHECK(encode_track(m).find("\"c\":0.0000") != std::string::npos);
}

// A non-zero seq makes the device ack every message, doubling link traffic at
// frame rate (firmware/src/main.cpp:113-115), so it must stay opt-in.
TEST_CASE("seq is absent unless explicitly requested") {
  CHECK(encode_track(TrackMessage{}).find("seq") == std::string::npos);
  CHECK(encode_track(TrackMessage{.lost = true, .seq = std::nullopt}).find("seq") == std::string::npos);
}

TEST_CASE("an explicit seq is emitted for both message shapes") {
  TrackMessage m;
  m.seq = 42;
  CHECK(encode_track(m) == "{\"t\":\"track\",\"x\":0.0000,\"y\":0.0000,\"w\":0.0000,"
                           "\"h\":0.0000,\"c\":0.0000,\"seq\":42}\n");

  CHECK(encode_track(TrackMessage{.lost = true, .seq = 7}) ==
        "{\"t\":\"track\",\"lost\":true,\"seq\":7}\n");
}

TEST_CASE("lines stay well inside the firmware's 511 byte limit") {
  TrackMessage m;
  m.x = -0.987654f;
  m.y = -0.987654f;
  m.w = 0.987654f;
  m.h = 0.987654f;
  m.c = 0.987654f;
  const auto line = encode_track(m);
  CHECK(line.size() < 128);
  CHECK(line.back() == '\n');
  // Scientific notation would be parseable but is not worth the risk.
  CHECK(line.find('e') == std::string::npos);
}

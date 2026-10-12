#include "audio_ring.h"

#include <catch2/catch_test_macros.hpp>

using namespace capture_eye;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

const Clock::time_point kT0{};

AudioPacket packet_at(Clock::duration offset) {
  return AudioPacket{.data = {1, 2, 3}, .captured_at = kT0 + offset, .duration_samples = 960};
}

} // namespace

TEST_CASE("AudioRing: between is (after, until], oldest first") {
  AudioRing ring{10s};
  for (int i = 0; i < 5; ++i) ring.push(packet_at(i * 20ms));

  const auto got = ring.between(kT0 + 20ms, kT0 + 60ms);
  REQUIRE(got.size() == 2);
  CHECK(got[0].captured_at == kT0 + 40ms);
  CHECK(got[1].captured_at == kT0 + 60ms);
}

TEST_CASE("AudioRing: a cursor at the last packet written picks up only later arrivals") {
  // The clip sink's pattern: write up to a frame, remember the last packet,
  // and resume from it — a packet that arrives after its frame is not lost.
  AudioRing ring{10s};
  ring.push(packet_at(0ms));
  ring.push(packet_at(20ms));

  auto first = ring.between(kT0 - 1ns, kT0 + 33ms);
  REQUIRE(first.size() == 2);
  const auto cursor = first.back().captured_at;

  ring.push(packet_at(40ms));  // trailing packet for the frame at 33ms
  ring.push(packet_at(60ms));
  const auto second = ring.between(cursor, kT0 + 66ms);
  REQUIRE(second.size() == 2);
  CHECK(second[0].captured_at == kT0 + 40ms);
}

TEST_CASE("AudioRing: packets older than the window are dropped") {
  AudioRing ring{100ms};
  for (int i = 0; i <= 10; ++i) ring.push(packet_at(i * 20ms));  // up to 200ms

  const auto all = ring.between(kT0 - 1ns, kT0 + 1s);
  REQUIRE_FALSE(all.empty());
  CHECK(all.front().captured_at == kT0 + 100ms);
  CHECK(all.back().captured_at == kT0 + 200ms);
}

TEST_CASE("AudioRing: an empty ring or empty window returns nothing") {
  AudioRing ring{1s};
  CHECK(ring.between(kT0, kT0 + 1s).empty());
  ring.push(packet_at(500ms));
  CHECK(ring.between(kT0 + 500ms, kT0 + 1s).empty());  // `after` is exclusive
}

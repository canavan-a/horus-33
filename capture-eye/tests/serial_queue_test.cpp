#include "serial_queue.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stop_token>
#include <string>
#include <variant>

using namespace capture_eye;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] bool is_command(const SerialQueue::Item& item, std::string_view expected) {
  return std::holds_alternative<std::string>(item) && std::get<std::string>(item) == expected;
}

[[nodiscard]] bool is_track(const SerialQueue::Item& item) {
  return std::holds_alternative<TrackMessage>(item);
}

} // namespace

TEST_CASE("SerialQueue: a fresh queue with nothing pushed times out rather than blocking forever") {
  SerialQueue queue;
  std::stop_source stop;
  const auto item = queue.take_blocking_for(stop.get_token(), 10ms);
  CHECK_FALSE(item.has_value());
}

TEST_CASE("SerialQueue: a track publishes and is taken") {
  SerialQueue queue;
  queue.publish_track(TrackMessage{.x = 0.5f, .c = 0.9f, .seq = std::nullopt});
  std::stop_source stop;
  const auto item = queue.take_blocking_for(stop.get_token(), 10ms);
  REQUIRE(item.has_value());
  REQUIRE(is_track(*item));
  CHECK(std::get<TrackMessage>(*item).x == 0.5f);
}

TEST_CASE("SerialQueue: a burst of tracks collapses to the newest, same as Slot") {
  SerialQueue queue;
  queue.publish_track(TrackMessage{.x = 0.1f, .seq = std::nullopt});
  queue.publish_track(TrackMessage{.x = 0.2f, .seq = std::nullopt});
  queue.publish_track(TrackMessage{.x = 0.3f, .seq = std::nullopt});
  CHECK(queue.tracks_dropped() == 2);

  std::stop_source stop;
  const auto item = queue.take_blocking_for(stop.get_token(), 10ms);
  REQUIRE(item.has_value());
  CHECK(std::get<TrackMessage>(*item).x == 0.3f);

  // Only one track was ever queued at a time — nothing left to take.
  const auto next = queue.take_blocking_for(stop.get_token(), 10ms);
  CHECK_FALSE(next.has_value());
}

TEST_CASE("SerialQueue: commands drain ahead of a pending track") {
  SerialQueue queue;
  queue.publish_track(TrackMessage{.x = 0.7f, .seq = std::nullopt});
  REQUIRE(queue.push_command("{\"t\":\"set\"}"));

  std::stop_source stop;
  const auto first = queue.take_blocking_for(stop.get_token(), 10ms);
  REQUIRE(first.has_value());
  CHECK(is_command(*first, "{\"t\":\"set\"}"));

  const auto second = queue.take_blocking_for(stop.get_token(), 10ms);
  REQUIRE(second.has_value());
  CHECK(is_track(*second));
}

TEST_CASE("SerialQueue: commands preserve FIFO order among themselves") {
  SerialQueue queue;
  REQUIRE(queue.push_command("a"));
  REQUIRE(queue.push_command("b"));
  REQUIRE(queue.push_command("c"));

  std::stop_source stop;
  CHECK(is_command(*queue.take_blocking_for(stop.get_token(), 10ms), "a"));
  CHECK(is_command(*queue.take_blocking_for(stop.get_token(), 10ms), "b"));
  CHECK(is_command(*queue.take_blocking_for(stop.get_token(), 10ms), "c"));
}

TEST_CASE("SerialQueue: a full command queue reports overflow rather than dropping silently") {
  SerialQueue queue{2};  // small capacity for the test
  CHECK(queue.push_command("a"));
  CHECK(queue.push_command("b"));
  CHECK_FALSE(queue.push_command("c"));  // full — the caller must know this failed

  std::stop_source stop;
  // "a" and "b" are still both there; "c" was never accepted.
  CHECK(is_command(*queue.take_blocking_for(stop.get_token(), 10ms), "a"));
  CHECK(is_command(*queue.take_blocking_for(stop.get_token(), 10ms), "b"));
}

TEST_CASE("SerialQueue: close() wakes a blocked take with nullopt") {
  SerialQueue queue;
  queue.close();
  std::stop_source stop;
  const auto item = queue.take_blocking(stop.get_token());
  CHECK_FALSE(item.has_value());
}

TEST_CASE("SerialQueue: a stopped token wakes take_blocking with nullopt") {
  SerialQueue queue;
  std::stop_source stop;
  stop.request_stop();
  const auto item = queue.take_blocking(stop.get_token());
  CHECK_FALSE(item.has_value());
}

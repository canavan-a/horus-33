#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "channel.h"
#include "frame_pool.h"
#include "geometry.h"
#include "slot.h"

using namespace capture_eye;
using Catch::Approx;

TEST_CASE("iou is 1 for identical boxes and 0 for disjoint ones") {
  const BoxF a{0, 0, 10, 10};
  CHECK(iou(a, a) == Approx(1.0f));
  CHECK(iou(a, BoxF{100, 100, 110, 110}) == Approx(0.0f));
  CHECK(iou(a, BoxF{10, 10, 20, 20}) == Approx(0.0f));  // touching corners only
}

TEST_CASE("iou of half-overlapping boxes") {
  // Union 150, intersection 50.
  CHECK(iou(BoxF{0, 0, 10, 10}, BoxF{5, 0, 15, 10}) == Approx(50.0f / 150.0f));
}

TEST_CASE("degenerate boxes do not divide by zero") {
  CHECK(iou(BoxF{0, 0, 0, 0}, BoxF{0, 0, 0, 0}) == Approx(0.0f));
  CHECK(iou(BoxF{10, 10, 0, 0}, BoxF{0, 0, 10, 10}) == Approx(0.0f));  // inverted
}

TEST_CASE("box_from_center round-trips") {
  const auto box = box_from_center(50, 100, 20, 40);
  CHECK(box.center_x() == Approx(50.0f));
  CHECK(box.center_y() == Approx(100.0f));
  CHECK(box.width() == Approx(20.0f));
  CHECK(box.height() == Approx(40.0f));
}

// The whole rate-decoupling design rests on this behaviour.
TEST_CASE("Slot keeps only the newest value") {
  Slot<int> slot;
  slot.publish(1);
  slot.publish(2);
  slot.publish(3);

  std::stop_source stop;
  const auto value = slot.take_blocking(stop.get_token());
  REQUIRE(value.has_value());
  CHECK(*value == 3);
  CHECK(slot.dropped() == 2);
}

TEST_CASE("Slot take consumes, so a second take waits") {
  Slot<int> slot;
  slot.publish(7);

  std::stop_source stop;
  REQUIRE(slot.take_blocking(stop.get_token()).has_value());

  stop.request_stop();  // otherwise this blocks forever, which is the point
  CHECK_FALSE(slot.take_blocking(stop.get_token()).has_value());
}

TEST_CASE("Slot peek does not consume") {
  Slot<int> slot;
  slot.publish(5);
  CHECK(slot.peek().value() == 5);
  CHECK(slot.peek().value() == 5);
}

TEST_CASE("Slot close wakes a blocked waiter") {
  Slot<int> slot;
  std::stop_source stop;
  std::atomic<bool> returned{false};

  std::jthread waiter{[&] {
    const auto value = slot.take_blocking(stop.get_token());
    returned = !value.has_value();
  }};

  slot.close();
  waiter.join();
  CHECK(returned.load());
}

TEST_CASE("Slot survives a publisher racing a consumer") {
  Slot<int> slot;
  std::stop_source stop;
  std::atomic<int> taken{0};

  std::jthread consumer{[&] {
    while (const auto value = slot.take_blocking(stop.get_token())) {
      ++taken;
    }
  }};

  for (int i = 0; i < 10000; ++i) slot.publish(i);
  stop.request_stop();
  slot.close();
  consumer.join();

  // The consumer legitimately misses most values; it must never see more than
  // were published, and must not deadlock.
  CHECK(taken.load() <= 10000);
}

TEST_CASE("Channel drops the oldest item when full") {
  Channel<int> channel{2, Overflow::drop_oldest};
  channel.push(1);
  channel.push(2);
  channel.push(3);

  std::stop_source stop;
  CHECK(channel.pop_blocking(stop.get_token()).value() == 2);
  CHECK(channel.pop_blocking(stop.get_token()).value() == 3);
  CHECK(channel.dropped() == 1);
}

TEST_CASE("Channel drop_newest preserves the head") {
  Channel<int> channel{2, Overflow::drop_newest};
  channel.push(1);
  channel.push(2);
  channel.push(3);

  std::stop_source stop;
  CHECK(channel.pop_blocking(stop.get_token()).value() == 1);
  CHECK(channel.pop_blocking(stop.get_token()).value() == 2);
}

TEST_CASE("Channel close releases a blocked consumer") {
  Channel<int> channel{4, Overflow::drop_oldest};
  std::stop_source stop;
  std::atomic<bool> returned{false};

  std::jthread waiter{[&] {
    const auto value = channel.pop_blocking(stop.get_token());
    returned = !value.has_value();
  }};

  channel.close();
  waiter.join();
  CHECK(returned.load());
}

TEST_CASE("FramePool hands out distinct buffers and takes them back") {
  FramePool pool{2, 4, 4};
  CHECK(pool.available() == 2);

  auto first = pool.acquire();
  auto second = pool.acquire();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(first->frame().bytes.data() != second->frame().bytes.data());
  CHECK(first->frame().bytes.size() == 4 * 4 * 3);

  // Exhausted: capture must drop a frame rather than block the camera.
  CHECK_FALSE(pool.acquire().has_value());

  first.reset();
  CHECK(pool.available() == 1);
}

TEST_CASE("a moved-from lease does not double-release") {
  FramePool pool{1, 2, 2};
  {
    auto lease = pool.acquire();
    REQUIRE(lease.has_value());
    FrameLease moved{std::move(*lease)};
    CHECK(moved.valid());
    CHECK_FALSE(lease->valid());
  }
  CHECK(pool.available() == 1);
}

TEST_CASE("a shared lease returns its slab only when the last reader is done") {
  FramePool pool{1, 2, 2};
  {
    auto lease = pool.acquire();
    REQUIRE(lease.has_value());
    const FrameRef shared = std::make_shared<const FrameLease>(std::move(*lease));
    FrameRef second_reader = shared;
    CHECK(pool.available() == 0);
  }
  CHECK(pool.available() == 1);
}

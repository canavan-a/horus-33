#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace capture_eye {

// A single-slot overwrite mailbox: the publisher never blocks, and a consumer
// that falls behind sees only the newest value.
//
// This is what decouples capture from inference. Capture publishes every frame;
// a slow consumer wakes up holding the latest one and everything published while
// it was busy has already been discarded. There is no explicit drop logic
// anywhere because the data structure does it.
//
// Deliberately the same semantics as the firmware's own 1-deep mailbox
// (xQueueOverwrite in firmware/src/controls/motion_control.cpp:306).
template <typename T>
class Slot {
public:
  void publish(T value) {
    {
      const std::scoped_lock lock{mutex_};
      if (value_.has_value()) ++dropped_;
      value_ = std::move(value);
    }
    condition_.notify_one();
  }

  // Waits for a value and consumes it. Returns nullopt if stopped or closed.
  [[nodiscard]] std::optional<T> take_blocking(std::stop_token token) {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, token, [this] { return value_.has_value() || closed_; });
    if (!value_.has_value()) return std::nullopt;
    return std::exchange(value_, std::nullopt);
  }

  // Non-consuming read, for consumers that want the latest known value on every
  // pass rather than one value per publish.
  [[nodiscard]] std::optional<T> peek() const {
    const std::scoped_lock lock{mutex_};
    return value_;
  }

  void close() {
    {
      const std::scoped_lock lock{mutex_};
      closed_ = true;
    }
    condition_.notify_all();
  }

  // Values overwritten before anyone took them.
  [[nodiscard]] std::uint64_t dropped() const {
    const std::scoped_lock lock{mutex_};
    return dropped_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<T> value_;
  bool closed_ = false;
  std::uint64_t dropped_ = 0;
};

} // namespace capture_eye

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace capture_eye {

// What a bounded channel does when it is full. Made explicit at construction
// because silently choosing one is how pipelines acquire mysterious stalls.
enum class Overflow {
  drop_oldest,  // shed backlog; the newest data is the useful data
  drop_newest,  // preserve order at the cost of the newest item
};

// Bounded queue for stages that must see every item rather than only the latest
// (the overlay feeds video, so it cannot use a Slot).
template <typename T>
class Channel {
public:
  Channel(std::size_t capacity, Overflow policy) : capacity_{capacity}, policy_{policy} {}

  // Never blocks the producer: capture must not stall behind a slow sink.
  void push(T value) {
    {
      const std::scoped_lock lock{mutex_};
      if (queue_.size() >= capacity_) {
        ++dropped_;
        if (policy_ == Overflow::drop_newest) return;
        queue_.pop_front();
      }
      queue_.push_back(std::move(value));
    }
    condition_.notify_one();
  }

  [[nodiscard]] std::optional<T> pop_blocking(std::stop_token token) {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, token, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return std::nullopt;
    T value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }

  void close() {
    {
      const std::scoped_lock lock{mutex_};
      closed_ = true;
    }
    condition_.notify_all();
  }

  [[nodiscard]] std::uint64_t dropped() const {
    const std::scoped_lock lock{mutex_};
    return dropped_;
  }

  [[nodiscard]] std::size_t size() const {
    const std::scoped_lock lock{mutex_};
    return queue_.size();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<T> queue_;
  std::size_t capacity_;
  Overflow policy_;
  bool closed_ = false;
  std::uint64_t dropped_ = 0;
};

} // namespace capture_eye

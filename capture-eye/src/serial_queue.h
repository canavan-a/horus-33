#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

#include "track_message.h"

namespace capture_eye {

// What the serial-owning thread consumes: either a track (not yet encoded —
// encode_track still happens in inference_loop, so this stage stays a dumb
// writer, same as before) or a complete command line from a relay client.
//
// Neither Slot nor Channel fits here alone. Tracks want overwrite semantics —
// newest wins, exactly like the firmware's own 1-deep mailbox — but commands
// must never be dropped silently: losing a `set` is a much worse failure than
// losing one of 25 tracks/sec. Commands therefore queue in order (bounded,
// FIFO) and drain ahead of the track on every take, so a control edit is
// never stuck behind a backlog of video-rate frames.
class SerialQueue {
public:
  using Item = std::variant<TrackMessage, std::string>;

  explicit SerialQueue(std::size_t command_capacity = 16) : command_capacity_{command_capacity} {}

  void publish_track(TrackMessage message) {
    {
      const std::scoped_lock lock{mutex_};
      if (pending_track_.has_value()) ++tracks_dropped_;
      pending_track_ = std::move(message);
    }
    condition_.notify_one();
  }

  // False means the queue is full — the caller (the control relay) is
  // expected to disconnect that client rather than let the command vanish
  // with no trace, which is what a silent drop would be.
  [[nodiscard]] bool push_command(std::string line) {
    {
      const std::scoped_lock lock{mutex_};
      if (commands_.size() >= command_capacity_) return false;
      commands_.push_back(std::move(line));
    }
    condition_.notify_one();
    return true;
  }

  // Commands drain first. Returns nullopt if stopped or closed.
  [[nodiscard]] std::optional<Item> take_blocking(std::stop_token token) {
    std::unique_lock lock{mutex_};
    condition_.wait(
        lock, token, [this] { return !commands_.empty() || pending_track_.has_value() || closed_; });
    return take_ready(lock);
  }

  // Same as take_blocking, but also returns nullopt on a plain timeout (the
  // caller cannot tell that apart from stop/close from the return value
  // alone — check the stop_token if it matters). This is what lets the
  // device-owning thread poll for replies on a fixed cadence even when
  // nothing is queued to send, rather than only after writing something.
  template <typename Rep, typename Period>
  [[nodiscard]] std::optional<Item> take_blocking_for(std::stop_token token,
                                                       std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lock{mutex_};
    const bool ready = condition_.wait_for(
        lock, token, timeout,
        [this] { return !commands_.empty() || pending_track_.has_value() || closed_; });
    if (!ready) return std::nullopt;
    return take_ready(lock);
  }

  void close() {
    {
      const std::scoped_lock lock{mutex_};
      closed_ = true;
    }
    condition_.notify_all();
  }

  [[nodiscard]] std::uint64_t tracks_dropped() const {
    const std::scoped_lock lock{mutex_};
    return tracks_dropped_;
  }

private:
  // Caller already holds the lock and has confirmed something is ready.
  [[nodiscard]] std::optional<Item> take_ready(std::unique_lock<std::mutex>& /*lock*/) {
    if (!commands_.empty()) {
      std::string line = std::move(commands_.front());
      commands_.pop_front();
      return Item{std::move(line)};
    }
    if (pending_track_.has_value()) {
      return Item{std::exchange(pending_track_, std::nullopt).value()};
    }
    return std::nullopt;  // closed_ with nothing queued
  }

  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<TrackMessage> pending_track_;
  std::deque<std::string> commands_;
  std::size_t command_capacity_;
  bool closed_ = false;
  std::uint64_t tracks_dropped_ = 0;
};

} // namespace capture_eye

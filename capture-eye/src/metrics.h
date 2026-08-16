#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace capture_eye {

// Counters for every place a frame can be created or lost.
//
// These are the numbers that prove the architecture: capture should hold the
// camera's rate while inference runs far slower, with the difference showing up
// as slot drops rather than as a slower capture rate. Every drop has its own
// counter, because "frames went missing" is not a diagnosis.
struct Metrics {
  std::atomic<std::uint64_t> frames_captured{0};
  std::atomic<std::uint64_t> decode_failures{0};
  std::atomic<std::uint64_t> pool_starved{0};  // no free buffer; frame dropped
  std::atomic<std::uint64_t> inferences{0};
  std::atomic<std::uint64_t> inference_failures{0};
  std::atomic<std::uint64_t> frames_rendered{0};
  std::atomic<std::uint64_t> tracks_sent{0};
  std::atomic<std::uint64_t> sink_errors{0};
  std::atomic<std::uint64_t> track_errors{0};

  std::atomic<std::uint64_t> inference_latency_us{0};  // most recent
  std::atomic<std::uint64_t> render_lag_us{0};         // capture -> on screen
  std::atomic<std::uint64_t> detection_age_us{0};      // age of the drawn boxes
};

// Per-second deltas, rendered as the status line.
struct MetricsSnapshot {
  std::uint64_t frames_captured = 0;
  std::uint64_t inferences = 0;
  std::uint64_t frames_rendered = 0;
  std::uint64_t tracks_sent = 0;
  std::uint64_t inference_skipped = 0;  // frames overwritten in the slot
  std::uint64_t overlay_dropped = 0;    // frames shed by the overlay queue
  std::uint64_t pool_starved = 0;
  std::uint64_t decode_failures = 0;
  std::uint64_t inference_failures = 0;
  std::uint64_t sink_errors = 0;
  std::uint64_t track_errors = 0;
  std::uint64_t inference_latency_us = 0;
  std::uint64_t render_lag_us = 0;

  [[nodiscard]] std::string render() const;
};

} // namespace capture_eye

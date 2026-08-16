#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "geometry.h"

namespace capture_eye {

// COCO class 0. The model scores all 80 classes, but only this one steers the
// gimbal.
inline constexpr int kPersonClass = 0;

struct Detection {
  BoxF box;  // source-frame pixels
  float confidence = 0;
  int class_id = kPersonClass;
};

struct DetectionResult {
  std::uint64_t frame_seq = 0;
  std::chrono::steady_clock::time_point captured_at{};
  std::vector<Detection> people;
  std::chrono::microseconds latency{0};

  // The one the selector actually chose, carried alongside rather than
  // re-derived downstream: the overlay must highlight the box that is really
  // steering the gimbal, not a plausible guess at it.
  std::optional<Detection> selected;
};

} // namespace capture_eye

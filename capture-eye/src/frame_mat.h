#pragma once

#include <opencv2/core.hpp>

#include "frame.h"

namespace capture_eye {

// A cv::Mat header over a frame's pixels. No copy, no ownership — the Mat is
// valid exactly as long as the frame's lease is.
[[nodiscard]] inline cv::Mat mat_for(const Frame& frame) {
  return cv::Mat{frame.height, frame.width, CV_8UC3,
                 const_cast<std::byte*>(frame.bytes.data())};
}

// Flips a frame in place, mount-orientation correction for a camera that is
// physically upside-down or mirrored. Applied once, right after decode and
// before anything else touches the frame (inference, overlay, every sink),
// so the whole pipeline downstream sees corrected pixels and correct
// coordinates — there is no separate "unflip" anywhere. A no-op config-time
// setting, not a runtime one: flipping mid-run would need every in-flight
// detection's coordinates reinterpreted, for a correction that only ever
// needs to be set once for how the camera is mounted.
inline void apply_flip(cv::Mat& mat, bool horizontal, bool vertical) {
  if (!horizontal && !vertical) return;
  // cv::flip's flip-code: 0 = around the x-axis (vertical flip), positive =
  // around the y-axis (horizontal flip), negative = both.
  const int code = horizontal && vertical ? -1 : (horizontal ? 1 : 0);
  cv::flip(mat, mat, code);
}

} // namespace capture_eye

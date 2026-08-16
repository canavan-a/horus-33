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

} // namespace capture_eye

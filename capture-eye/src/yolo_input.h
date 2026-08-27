#pragma once

#include <array>
#include <vector>

#include <opencv2/core.hpp>

#include "frame.h"
#include "letterbox.h"

namespace capture_eye {

// Turns a captured frame into the NCHW float tensor a YOLO26 export expects:
// letterbox onto a square canvas, BGR -> RGB, /255, HWC -> CHW.
//
// Shared by every inference backend — the preprocessing is a property of the
// model, not of the runtime executing it, and duplicating it per backend is how
// two backends quietly stop agreeing on what a detection means. Buffers are
// allocated once and reused, so nothing here allocates in steady state.
class YoloInput {
 public:
  explicit YoloInput(int input_size);

  // Fills tensor() from the frame and updates transform() to match its size.
  void fill(const Frame& frame);

  [[nodiscard]] std::vector<float>& tensor() { return tensor_; }
  [[nodiscard]] const std::vector<float>& tensor() const { return tensor_; }
  [[nodiscard]] const LetterboxTransform& transform() const { return transform_; }
  [[nodiscard]] int size() const { return input_size_; }

 private:
  void prepare_for(int width, int height);

  int input_size_;
  std::vector<float> tensor_;
  cv::Mat canvas_;                // padded 8UC3 BGR
  cv::Mat rgb_;                   // 8UC3 RGB
  cv::Mat floats_;                // 32FC3 scaled
  std::array<cv::Mat, 3> planes_; // views onto tensor_
  LetterboxTransform transform_{};
  int prepared_width_ = 0;
  int prepared_height_ = 0;
};

} // namespace capture_eye

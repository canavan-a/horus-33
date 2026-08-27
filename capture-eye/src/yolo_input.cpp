#include "yolo_input.h"

#include <cstddef>

#include <opencv2/imgproc.hpp>

#include "frame_mat.h"

namespace capture_eye {
namespace {

// Ultralytics pads with this grey; a different value measurably shifts results.
constexpr double kPadValue = 114.0;

} // namespace

YoloInput::YoloInput(int input_size) : input_size_{input_size} {
  const auto side = static_cast<std::size_t>(input_size_);
  tensor_.resize(side * side * 3);
  for (std::size_t c = 0; c < 3; ++c) {
    planes_[c] = cv::Mat{input_size_, input_size_, CV_32FC1, tensor_.data() + c * side * side};
  }
}

void YoloInput::prepare_for(int width, int height) {
  if (width == prepared_width_ && height == prepared_height_) return;

  transform_ = letterbox_transform(width, height, input_size_);
  canvas_.create(input_size_, input_size_, CV_8UC3);
  canvas_.setTo(cv::Scalar::all(kPadValue));
  prepared_width_ = width;
  prepared_height_ = height;
}

void YoloInput::fill(const Frame& frame) {
  prepare_for(frame.width, frame.height);

  // Resize straight into the padded canvas's centre region, so the pad and the
  // resize are one pass rather than a resize followed by copyMakeBorder.
  const cv::Rect roi{transform_.pad_x, transform_.pad_y, transform_.scaled_width,
                     transform_.scaled_height};
  cv::Mat destination = canvas_(roi);
  cv::resize(mat_for(frame), destination, destination.size(), 0, 0, cv::INTER_LINEAR);

  // Frames are BGR; the model was exported expecting RGB.
  cv::cvtColor(canvas_, rgb_, cv::COLOR_BGR2RGB);
  rgb_.convertTo(floats_, CV_32FC3, 1.0 / 255.0);

  // Split writes the three channel planes directly into the tensor buffer, so
  // the HWC-to-CHW transpose costs no extra copy.
  cv::split(floats_, planes_.data());
}

} // namespace capture_eye

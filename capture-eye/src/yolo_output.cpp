#include "yolo_output.h"

#include <cmath>

namespace capture_eye {
namespace {

[[nodiscard]] float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

// Below roughly one pixel the box carries no usable centre for the PID loop.
constexpr float kMinBoxPixels = 1.0f;

} // namespace

std::vector<Detection> parse_yolo_output(std::span<const float> logits,
                                         std::span<const float> boxes, std::size_t queries,
                                         std::size_t classes,
                                         const LetterboxTransform& transform,
                                         float confidence_threshold, int class_id) {
  std::vector<Detection> detections;
  if (class_id < 0 || static_cast<std::size_t>(class_id) >= classes) return detections;
  if (logits.size() < queries * classes || boxes.size() < queries * 4) return detections;

  for (std::size_t query = 0; query < queries; ++query) {
    const float score = sigmoid(logits[query * classes + static_cast<std::size_t>(class_id)]);
    if (score < confidence_threshold) continue;

    const float cx = boxes[query * 4 + 0];
    const float cy = boxes[query * 4 + 1];
    const float w = boxes[query * 4 + 2];
    const float h = boxes[query * 4 + 3];

    const BoxF box = box_from_model(cx, cy, w, h, transform);
    if (box.width() < kMinBoxPixels || box.height() < kMinBoxPixels) continue;

    detections.push_back(Detection{.box = box, .confidence = score, .class_id = class_id});
  }
  return detections;
}

} // namespace capture_eye

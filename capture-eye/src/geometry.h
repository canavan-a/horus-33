#pragma once

#include <algorithm>

namespace capture_eye {

// Axis-aligned box in whatever space the holder documents; the pipeline uses
// source-frame pixels everywhere except inside the model adapter.
struct BoxF {
  float x1 = 0;
  float y1 = 0;
  float x2 = 0;
  float y2 = 0;

  [[nodiscard]] constexpr float width() const { return x2 - x1; }
  [[nodiscard]] constexpr float height() const { return y2 - y1; }
  [[nodiscard]] constexpr float area() const {
    return std::max(0.0f, width()) * std::max(0.0f, height());
  }
  [[nodiscard]] constexpr float center_x() const { return (x1 + x2) * 0.5f; }
  [[nodiscard]] constexpr float center_y() const { return (y1 + y2) * 0.5f; }
};

// Builds a box from a centre and size, the convention the model emits.
[[nodiscard]] constexpr BoxF box_from_center(float cx, float cy, float w, float h) {
  return BoxF{cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f};
}

[[nodiscard]] constexpr BoxF clamp_box(BoxF box, float max_x, float max_y) {
  return BoxF{std::clamp(box.x1, 0.0f, max_x), std::clamp(box.y1, 0.0f, max_y),
              std::clamp(box.x2, 0.0f, max_x), std::clamp(box.y2, 0.0f, max_y)};
}

// Intersection over union. Zero for disjoint or degenerate boxes.
[[nodiscard]] constexpr float iou(BoxF a, BoxF b) {
  const float ix1 = std::max(a.x1, b.x1);
  const float iy1 = std::max(a.y1, b.y1);
  const float ix2 = std::min(a.x2, b.x2);
  const float iy2 = std::min(a.y2, b.y2);
  const float intersection = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
  if (intersection <= 0.0f) return 0.0f;
  const float combined = a.area() + b.area() - intersection;
  return combined > 0.0f ? intersection / combined : 0.0f;
}

} // namespace capture_eye

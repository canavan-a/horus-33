#include <cmath>
#include <memory>
#include <numbers>
#include <thread>

#include "detector.h"

namespace capture_eye {

Detector make_fake_detector(std::chrono::milliseconds latency) {
  // Owned by the lambda, so the counter cannot become shared state between two
  // detectors; the inference stage is the only caller.
  struct State {
    std::uint64_t calls = 0;
  };
  auto state = std::make_shared<State>();

  Detector detector;
  detector.name = "fake";
  detector.detect = [state, latency](const Frame& frame) -> Result<std::vector<Detection>> {
    std::this_thread::sleep_for(latency);

    const auto phase = static_cast<float>(state->calls++) * 0.08f;
    const float width = static_cast<float>(frame.width);
    const float height = static_cast<float>(frame.height);

    // Sweeps horizontally across the middle 60% of the frame so the sign of the
    // x axis is visible on stdout, and dips vertically so y is visible too.
    const float cx = width * (0.5f + 0.3f * std::sin(phase));
    const float cy = height * (0.5f + 0.15f * std::sin(phase * 0.5f));
    const float box_w = width * 0.18f;
    const float box_h = height * 0.45f;

    std::vector<Detection> people;
    people.push_back(Detection{.box = box_from_center(cx, cy, box_w, box_h),
                               .confidence = 0.9f,
                               .class_id = kPersonClass});
    return people;
  };
  return detector;
}

} // namespace capture_eye

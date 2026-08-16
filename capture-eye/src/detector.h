#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "detection.h"
#include "error.h"
#include "frame.h"

namespace capture_eye {

// The inference seam. A value type holding a callable rather than a base class
// to inherit from: backends differ only in how they turn pixels into boxes.
//
// M1 runs a fake detector through this; M3 drops the ONNX one in behind the
// same signature with nothing upstream changing.
struct Detector {
  std::function<Result<std::vector<Detection>>(const Frame&)> detect;
  std::string name;
};

// Sleeps to imitate a real backend's latency and returns one box tracing a
// horizontal path across the frame. Exists so the whole pipeline — rates, drops,
// selection, emission, shutdown — can be proven before ONNX is involved.
[[nodiscard]] Detector make_fake_detector(std::chrono::milliseconds latency);

} // namespace capture_eye

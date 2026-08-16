#pragma once

#include <filesystem>
#include <string>

#include "config.h"
#include "detector.h"
#include "error.h"

namespace capture_eye {

// YOLO26 person detection on ONNX Runtime, CPU.
//
// Fits behind the same Detector seam as the fake one, so nothing upstream knows
// the difference.
[[nodiscard]] Result<Detector> make_onnx_detector(const InferenceConfig& config,
                                                  const std::filesystem::path& model);

// Human-readable dump of the model's real input and output tensors, for
// --dump-model-io. Verifying beats assuming: this project already shipped a plan
// built on the wrong output shape.
[[nodiscard]] Result<std::string> describe_model_io(const std::filesystem::path& model);

} // namespace capture_eye

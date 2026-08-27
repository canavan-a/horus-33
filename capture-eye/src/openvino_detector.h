#pragma once

#include <filesystem>

#include "config.h"
#include "detector.h"
#include "error.h"

namespace capture_eye {

// YOLO26 person detection on OpenVINO, CPU.
//
// Same Detector seam, same preprocessing (YoloInput) and same postprocessing
// (parse_yolo_output) as the ONNX backend — only the runtime executing the
// graph differs, which is exactly what makes the two comparable in a benchmark.
//
// `model` is either an OpenVINO IR .xml (its .bin sits beside it) or a plain
// .onnx — OpenVINO's ONNX frontend reads the latter, so the model store's
// downloads (variants included) work here exactly as they do for ONNX Runtime.
// IR skips the import step and is what a benchmark should use; a quantized
// .onnx keeps its quantization either way.
//
// Only compiled into a build configured with -DCAPTURE_EYE_OPENVINO=ON.
[[nodiscard]] Result<Detector> make_openvino_detector(const InferenceConfig& config,
                                                      const std::filesystem::path& model);

} // namespace capture_eye

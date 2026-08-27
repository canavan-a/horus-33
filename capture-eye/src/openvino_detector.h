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
// `model` must point at an OpenVINO IR .xml (its .bin sits beside it); the
// model store's auto-download only ever produces .onnx, so this backend
// requires an explicitly configured model path.
//
// Only compiled into a build configured with -DCAPTURE_EYE_OPENVINO=ON.
[[nodiscard]] Result<Detector> make_openvino_detector(const InferenceConfig& config,
                                                      const std::filesystem::path& model);

} // namespace capture_eye
